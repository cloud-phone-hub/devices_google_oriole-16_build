#!/usr/bin/env python3
"""note13-5G-15 Remote KeyMint HAL（android.hardware.security.keymint）落地。

背景：lineage 底包自带的 /vendor/bin/hw/android.hardware.security.keymint-service
是软件 keymint（AndroidKeyMintDevice，密钥材料在 guest 内存，日志可见
AndroidKeyMintDevice 符号）。本模块把它替换为 Cuttlefish 远程 keymint
（android.hardware.security.keymint-service.remote），经 /dev/hvc3 与 host
secure_env（KeymasterChannel）收发 keymaster 消息，密钥/证明能力全在 host。
（v3 顺延 virtconsole 布局：keymint = hvc3 = Cuttlefish 基线编号，hvc0/1
留给固件 console，hvc2 空置预留。）

方案（与 pixel6-15 同款：单服务同名替换，避免双 keymint 争抢 IKeyMintDevice/default）：
  1. 编译：委托 cuttlefish/scripts/build-guest-keymint.sh（AIDL 生成 + 单发
     clang 链接 + llvm-readelf 校验的单一事实来源），产物直接以基线同名
     android.hardware.security.keymint-service 落盘 out/dist/vendor/bin/hw；
  2. rc：以基座 /etc/init/android.hardware.security.keymint-service.rc 为底，
     在 service 声明行后插 `    file /dev/kmsg w`（同 pixel6 patchKeymintRC）；
  3. vintf：以基座 manifest 为底，换注释头并删除 RKP 组件块（同 pixel6
     generateKeymintVintfXML：RKP 注册前不声明）。

三条路径（/vendor/bin、/vendor/etc、/vendor/etc/vintf）全部命中内核
getname_flags 白名单 -> 运行时读到的都是 /mnt/vendor/...（patch.img），
注入文件遮蔽基座同名件，service vendor.keymint-default 由新二进制提供。

SELinux：二进制沿用基座文件标签 hal_keymint_default_exec（基座 vendor_file_contexts
已含该路径）；/dev/hvc3 设备标签（keymaster_device）与 hal_keymint_default 的
chr_file/serialno_prop/kmsg 授权由 sepolicy.keymint_hal_patch_commands 追加
（build/vendor/build/sepolicy.py，随 vendor 重打包写入 vendor_sepolicy.cil +
vendor_file_contexts）。

host 侧依赖：设备规格 FactoryTEE=true 时 daemon 拉起 secure_env 并把 socketpair
guest 端挂到 QEMU hvc3 chardev（core/host/daemon/qemu/qemu.go，dense 布局）。

用法：
  python3 build/vendor/source/android.hardware.keymint/build.py
  python3 build/vendor/source/android.hardware.keymint/build.py --dry-run
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.keymint/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import (  # noqa: E402
    LINEAGE_OUT_DIR, PROJECT_ROOT, VENDOR_STAGE_DIR, baselines_file,
)
import deps  # noqa: E402   note13 自身 lineage super 的编译依赖组装

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
STANDALONE_DIR = os.path.join(MODULE_DIR, "standalone")
# note13 运行库兼容垫片：note13 lineage-22.0 的 libkeymaster_messages.so 未导出
# keymaster::TAG_MODULE_HASH，HAL 静态初始化引用它 -> 链接失败。用强定义补齐，
# 保持运行期不依赖 guest 旧库（详见该文件头注释）。
COMPAT_SOURCES = (
    os.path.join(STANDALONE_DIR, "keymaster_tags_module_hash.cpp"),
)

# 编译脚本（单一事实来源：AIDL 生成 + 链接 + readelf 校验）
COMPILE_SCRIPT = os.path.join(PROJECT_ROOT, "cuttlefish", "scripts",
                              "build-guest-keymint.sh")
# 依赖源工作区（缺任一目录则优雅跳过，不阻塞 vendor 重打包）
SOURCE_WORKSPACE = os.path.join(PROJECT_ROOT, "cuttlefish", "source")
REQUIRED_SOURCE_DIRS = [
    "system-libbase/include",
    "system-logging/liblog/include",
    "frameworks-native/libs/binder/ndk/include_platform",
    "external-boringssl/src/include",
]
# 运行库目录：默认由 deps.py 从 **note13 自身** lineage-22.0 super 组装
# （unpacked/vendor/lib64 4 个 + system_a /system/lib64 2 个），链接期输入，
# 运行时由 note13 guest 自己的 /vendor/lib64 + /system/lib64 提供。
# 可用 `--runtime-lib-dir` 参数或 env CDH_RUNTIME 显式覆盖（仅排查用）。
RUNTIME_LIBS = [
    "libc++.so", "libbase.so", "libbinder_ndk.so", "libcrypto.so",
    "libkeymaster_messages.so", "liblog.so",
]


def resolve_runtime_lib_dir(arg):
    """--runtime-lib-dir 参数 > env CDH_RUNTIME > deps（note13 lineage super）。"""
    if arg:
        return arg
    if os.environ.get("CDH_RUNTIME"):
        return os.environ["CDH_RUNTIME"]
    return deps.prepare_runtime_libs()

# 落盘（相对 VENDOR_STAGE_DIR 即镜像内 /vendor 路径）
BIN_NAME = "android.hardware.security.keymint-service"
DEST_BIN = os.path.join(VENDOR_STAGE_DIR, "bin", "hw", BIN_NAME)
DEST_RC = os.path.join(VENDOR_STAGE_DIR, "etc", "init",
                       "android.hardware.security.keymint-service.rc")
DEST_VINTF = os.path.join(VENDOR_STAGE_DIR, "etc", "vintf", "manifest",
                          "android.hardware.security.keymint-service.xml")

# 基座同名文件（rc/vintf 以基座为底改写；二进制编译后直接遮蔽）
BASE_RC = os.path.join(LINEAGE_OUT_DIR, "unpacked", "vendor", "etc", "init",
                       "android.hardware.security.keymint-service.rc")
BASE_VINTF = os.path.join(LINEAGE_OUT_DIR, "unpacked", "vendor",
                          "etc", "vintf", "manifest",
                          "android.hardware.security.keymint-service.xml")

# 新增/遮蔽文件的 SELinux 标签（二进制沿用基座 file_contexts 路径标签）
LABELS = {
    f"bin/hw/{BIN_NAME}": "u:object_r:hal_keymint_default_exec:s0",
    "etc/init/android.hardware.security.keymint-service.rc":
        "u:object_r:vendor_configs_file:s0",
    "etc/vintf/manifest/android.hardware.security.keymint-service.xml":
        "u:object_r:vendor_configs_file:s0",
}

# rc 改写：service 声明行后插 `    file /dev/kmsg w`（与 pixel6 patchKeymintRC 同款）
# 服务行尾带通道参数 /dev/hvc3（v3 顺延 virtconsole 布局；keymint = hvc3 =
# Cuttlefish 基线编号，hvc0/1 留给固件 console，hvc2 空置预留）：patch_rc
# 匹配基座原始 service 行（无参数）后追加。
# RODATA 校验仍需带 /dev/hvc3 默认串（见 build-guest-keymint.sh 校验段；
# v3 布局下通道参数与默认值一致）。
RC_SERVICE = b"service vendor.keymint-default " \
             b"/vendor/bin/hw/android.hardware.security.keymint-service"
KEYMINT_CHANNEL_ARG = b" /dev/hvc3"
KMSG_LINE = b"    file /dev/kmsg w"

# vintf 改写：换注释头 + 删除 RKP 组件块（与 pixel6 generateKeymintVintfXML 同款）
VINTF_HEADER = "<!-- note13-5G-15 Remote KeyMint实际能力清单：RKP注册前不声明。 -->"
RKP_FQ = b"IRemotelyProvisionedComponent/default"


def env(name, default):
    return os.environ.get(name) or default


def _missing(what, path):
    print(f"[err] {what} 缺失: {path}", file=sys.stderr)
    sys.exit(1)


def check_prereqs(runtime_lib_dir):
    """编译前置校验：脚本、依赖源工作区、运行库。缺失即报错退出。"""
    if not os.path.isfile(COMPILE_SCRIPT):
        _missing("keymint 编译脚本", COMPILE_SCRIPT)
    for sub in REQUIRED_SOURCE_DIRS:
        if not os.path.isdir(os.path.join(SOURCE_WORKSPACE, sub)):
            _missing("依赖源目录", os.path.join(SOURCE_WORKSPACE, sub))
    if not os.path.isdir(runtime_lib_dir):
        _missing("运行库目录", runtime_lib_dir)
    for lib in RUNTIME_LIBS:
        if not os.path.isfile(os.path.join(runtime_lib_dir, lib)):
            _missing("运行库", os.path.join(runtime_lib_dir, lib))


def compile_remote(dry_run=False, runtime_lib_dir=None):
    """委托 build-guest-keymint.sh 编译远程 keymint，产物直接落成基线同名。"""
    argv = [
        "bash", COMPILE_SCRIPT,
        "--out", DEST_BIN,
        "--runtime-lib-dir", runtime_lib_dir,
        "--standalone-dir", STANDALONE_DIR,
    ]
    argv += [arg for src in COMPAT_SOURCES for arg in ("--extra-source", src)]
    if dry_run:
        print("[..] (dry)", " ".join(argv))
        return
    res = subprocess.run(argv)
    if res.returncode != 0:
        print(f"[err] keymint 编译失败（exit={res.returncode}）", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(DEST_BIN):
        _missing("keymint 编译产物", DEST_BIN)


def patch_rc(base: bytes) -> bytes:
    """基座 rc → late-fs 等 /dev/hvc3 + disabled（根因：/dev/hvc3 竞态）。

    基座 rc 为 `class early_hal`：early_hal 在 boot 早期启动，而 /dev/hvc3
    依赖 virtio_console.ko —— note13 内核把 virtio_console 编成模块，由
    vendor.dlkm_loader（init.virt.rc，class main + oneshot，on post-fs 触发）
    后期加载。keymint 早于模块加载启动时 open("/dev/hvc3") 失败 → LOG(FATAL)
    → exit status 1（实测：keymint 5.779s 启动 vs virtio_console.ko 5.820s
    加载）。

    第一版门控（`init.svc.vendor.dlkm_loader=stopped` + disabled）实例 24/25
    失败：dlkm_loader 加载完 8 个模块后卡在 SetProperty（被 SELinux 拒）不退出，
    stopped 永不触发 -> keymint 永不启动 -> keystore2 无 HAL 崩溃(10.88s ptrace)
    -> vold 永远等 keystore2。且即便 dlkm_loader 正常退出（≥15.6s），keymint
    也晚于 keystore2 首次查 HAL（~5.79s 启动，10s 崩溃），顺序依旧错误。

    改为：`on late-fs`（晚于 post-fs，此时 dlkm_loader 已被 start 且在加载
    模块）内 `wait /dev/hvc3 30` —— /dev/hvc3 在 virtio_console.ko 加载完成
    （~5.86s）时由 ueventd 创建，sepolicy v2 已放行 vendor_init getattr，wait
    即刻返回 -> start keymint（~5.9s），远早于 keystore2 崩溃点，且不再依赖
    dlkm_loader 进程的退出。

    改写：
      * 去 `class early_hal`，加 `disabled`：不再由 init 早期 class 启动；
      * 前置 on 块：`on late-fs` 内 `wait /dev/hvc3 30` + `start`，保证
        keymint 在 hvc3 出现后立即启动。
    保留 `    file /dev/kmsg w`（基座已含时去重）。
    """
    head = (b"on late-fs\n"
            b"    write /dev/kmsg \"CDH-KEYMINT-RC-V3-MARKER\\n\"\n"
            b"    wait /dev/hvc3 30\n"
            b"    write /dev/kmsg \"CDH-KEYMINT-RC-V3-STARTING\\n\"\n"
            b"    start vendor.keymint-default\n\n")
    lines = base.split(b"\n")
    body = []
    inserted = 0
    for line in lines:
        fields = line.split()
        if len(fields) >= 3 and fields[0] == b"file" and \
                fields[1] == b"/dev/kmsg" and fields[2] == b"w":
            continue  # 基座已含 kmsg 行时去重
        if fields and fields[0] == b"class" and fields[1] == b"early_hal":
            continue  # 去掉 early_hal 类（见 docstring 竞态说明）
        body.append(line)
        if line.startswith(RC_SERVICE):
            # v3 顺延布局：service 行尾追加通道参数（/dev/hvc3）
            body.pop()
            body.append(line + KEYMINT_CHANNEL_ARG)
            body.append(KMSG_LINE)
            body.append(b"    disabled")
            inserted += 1
    if inserted != 1:
        print(f"[err] keymint rc service 声明命中 {inserted} 处（期望 1）",
              file=sys.stderr)
        sys.exit(1)
    return head + b"\n".join(body) + b"\n"


def patch_vintf(base: bytes) -> bytes:
    """基座 vintf：去 4 行注释头 -> 换基线头 -> 删 RKP 组件块。"""
    lines = base.split(b"\n")
    if len(lines) >= 4 and lines[0] == b"<!--" and lines[3] == b"-->":
        lines = lines[4:]
    lines = [VINTF_HEADER.encode("utf-8")] + lines
    idx = next((i for i, l in enumerate(lines) if RKP_FQ in l), -1)
    if idx >= 0:
        start = idx
        while b"<hal" not in lines[start]:
            start -= 1
        end = idx
        while b"</hal>" not in lines[end]:
            end += 1
        lines = lines[:start] + lines[end + 1:]
    return b"\n".join(lines)


def _write(dest, data, mode):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(data)
    os.chmod(dest, mode)
    print(f"    写 {os.path.relpath(dest, VENDOR_STAGE_DIR)} ({len(data)} B)")


def write_rc_vintf(dry_run=False):
    """读基座 rc/vintf，改写后落盘（遮蔽基座同名件）。"""
    if not os.path.isfile(BASE_RC) or not os.path.isfile(BASE_VINTF):
        print(f"[err] 基座 keymint rc/vintf 缺失: {BASE_RC} / {BASE_VINTF}",
              file=sys.stderr)
        sys.exit(1)
    with open(BASE_RC, "rb") as f:
        rc = patch_rc(f.read())
    with open(BASE_VINTF, "rb") as f:
        vintf = patch_vintf(f.read())
    if dry_run:
        print(f"[..] (dry) 将写入 {os.path.relpath(DEST_RC, VENDOR_STAGE_DIR)}"
              f" / {os.path.relpath(DEST_VINTF, VENDOR_STAGE_DIR)}")
        return
    _write(DEST_RC, rc, 0o644)
    _write(DEST_VINTF, vintf, 0o644)


def register_labels(baselines_path, dry_run=False):
    """登记新增文件的 SELinux 标签进 baselines vendor.selinux-labels（幂等）。"""
    if not baselines_path or not os.path.isfile(baselines_path):
        _missing("基线清单", baselines_path)
    with open(baselines_path, encoding="utf-8") as f:
        doc = json.load(f)
    labels = doc.setdefault("vendor", {}).setdefault("selinux-labels", {})
    missing = {k: v for k, v in LABELS.items() if labels.get(k) != v}
    if not missing:
        print("[..] baselines 标签已齐，跳过")
        return
    labels.update(missing)
    if dry_run:
        print(f"[..] (dry) baselines 将登记标签 {len(missing)} 条")
        return
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(baselines_path),
                               prefix=".baselines.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(doc, f, ensure_ascii=False, indent=2)
            f.write("\n")
        os.replace(tmp, baselines_path)
    except BaseException:
        os.unlink(tmp)
        raise
    print(f"[ok] baselines 登记标签 {len(missing)} 条")


def build(dry_run=False, runtime_lib_dir=None):
    runtime_lib_dir = resolve_runtime_lib_dir(runtime_lib_dir)
    check_prereqs(runtime_lib_dir)
    print(f"==> 编译远程 keymint（{COMPILE_SCRIPT}）"
          + (f" runtime-lib-dir={runtime_lib_dir}" if dry_run else ""))
    compile_remote(dry_run=dry_run, runtime_lib_dir=runtime_lib_dir)

    print("==> rc / vintf（基座改写）")
    write_rc_vintf(dry_run=dry_run)

    print("==> SELinux 标签登记")
    register_labels(baselines_file(), dry_run=dry_run)

    print("[ok] Remote KeyMint HAL 落盘完成")
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 Remote KeyMint HAL 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    ap.add_argument("--runtime-lib-dir", default=None,
                    help="链接期运行库目录（默认 deps.py 组装 note13 lineage super；"
                         "env CDH_RUNTIME 或本参数可覆盖）")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run, runtime_lib_dir=args.runtime_lib_dir))


if __name__ == "__main__":
    main()
