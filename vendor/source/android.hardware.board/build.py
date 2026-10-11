#!/usr/bin/env python3
"""note13-5G-15 SoC 身份对齐：chipinfo 等价二进制构建（/vendor/bin/chipinfo）。

背景与真机链路见 src/chipinfo.c 头注释。要点：属性类型 vendor_mtk_soc_prop 属
vendor_restricted_property_type，plat neverallow（主体 coredomain）禁止
init/vendor_init 直接 set —— 必须由独立的**非 coredomain** 供应商域写入。gold
真机由官方 `/vendor/bin/chipinfo`（域 chipinfo）承担；云机无 MTK sysfs，本模块
产出同名同路径的等价二进制（硬编真机实测身份值）。

产物（相对路径即镜像内 /vendor 路径，落 out/dist/vendor 供 inject.py 注入）：
  bin/chipinfo                标签 u:object_r:chipinfo_exec:s0（0755）
  etc/init/chipinfo_init.rc   early-init exec_background 拉起（0644）

配套（本模块内自包含，与其它 source 子模块同构）：
  src/chipinfo.c                        身份值与 setprop 实现（唯一事实源）
  vendor/etc/init/chipinfo_init.rc      rc 源文件，随构建拷入 staging
  build/vendor/build/sepolicy.py chipinfo_cil()  chipinfo 域类型/转换/授权段

用法：
  python3 build/vendor/source/android.hardware.board/build.py
  python3 build/vendor/source/android.hardware.board/build.py --dry-run
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.board/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import OUT_DIR, VENDOR_STAGE_DIR, baselines_file  # noqa: E402

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(MODULE_DIR, "src", "chipinfo.c")
WORK_DIR = os.path.join(OUT_DIR, "tmp", "board-ndk-work")

TARGET = "aarch64-linux-android35"
BINARY = "chipinfo"
# rc 源文件在本模块 vendor/ 下，随构建拷到 staging 的 etc/init/
RC_SRC = os.path.join(MODULE_DIR, "vendor", "etc", "init", "chipinfo_init.rc")
RC_REL = "etc/init/chipinfo_init.rc"
# 标签登记键即 staging 树内相对路径（与 build/vendor/build/inject.py 同一套）
LABEL_BIN = "u:object_r:chipinfo_exec:s0"
LABEL_RC = "u:object_r:vendor_configs_file:s0"


def _fail(msg):
    print(f"[err] {msg}", file=sys.stderr)
    sys.exit(1)


def env(name, default):
    return os.environ.get(name) or default


def toolchain():
    ndk = env("CDH_NDK", os.path.expanduser(
        "~/Library/Android/sdk/ndk/29.0.14206865"))
    prebuilt = os.path.join(ndk, "toolchains", "llvm", "prebuilt",
                            "darwin-x86_64")
    clang = env("CDH_CLANG", os.path.join(prebuilt, "bin", "clang"))
    sysroot = os.path.join(prebuilt, "sysroot")
    if not os.path.isfile(clang):
        _fail(f"工具链缺失: {clang}（设 CDH_NDK 指向可用 NDK）")
    return {"clang": clang, "sysroot": sysroot}


def compile_binary(tc, out_bin, dry_run=False):
    """编译 PIE 可执行（仅 libc：不链平台 libc++/binder，NDK 默认驱动即产
    Android 动态可执行，与 /vendor 下 setprop 一类原厂小工具同形态）。"""
    out_bin = os.path.abspath(out_bin)
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "-std=c17", "-O2", "-fPIE", "-pie", "-Wall", "-Wextra",
        "-Wl,-z,relro", "-Wl,-z,now",
        SRC, "-o", out_bin,
    ]
    if dry_run:
        print("    (dry)", " ".join(argv))
        return
    os.makedirs(os.path.dirname(out_bin), exist_ok=True)
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout, file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        _fail("编译失败: chipinfo")
    os.chmod(out_bin, 0o755)
    print(f"    写 {os.path.relpath(out_bin, VENDOR_STAGE_DIR)}"
          f"（{os.path.getsize(out_bin)} B，0755）")


def write_asset(dry_run=False):
    """把模块内 vendor/ 下的 rc 拷到 staging（相对路径即镜像内 /vendor 路径）。"""
    if not os.path.isfile(RC_SRC):
        _fail(f"rc 源文件缺失: {RC_SRC}")
    dst = os.path.join(VENDOR_STAGE_DIR, RC_REL.replace("/", os.sep))
    if dry_run:
        print(f"[..] (dry) 写 {RC_REL}")
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(RC_SRC, encoding="utf-8") as f:
        content = f.read()
    with open(dst, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"    写 {RC_REL}")


def register_labels(dry_run=False):
    """登记 chipinfo 二进制与 rc 的 SELinux 标签（幂等）。"""
    path = baselines_file()
    if not path or not os.path.isfile(path):
        _fail(f"基线清单缺失: {path or 'None'}")
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    labels = doc.setdefault("vendor", {}).setdefault("selinux-labels", {})
    entries = {
        f"bin/{BINARY}": LABEL_BIN,
        RC_REL: LABEL_RC,
    }
    missing = {k: v for k, v in entries.items() if labels.get(k) != v}
    if not missing:
        print("[..] baselines 标签已齐，跳过")
        return
    if dry_run:
        print(f"[..] (dry) baselines 将登记标签 {sorted(missing)}")
        return
    labels.update(missing)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path),
                               prefix=".baselines.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(doc, f, ensure_ascii=False, indent=2)
            f.write("\n")
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise
    print(f"[ok] baselines 登记标签 {sorted(missing)}")


def build(dry_run=False):
    if not os.path.isfile(SRC):
        _fail(f"源码缺失: {SRC}")
    os.makedirs(WORK_DIR, exist_ok=True)
    tc = toolchain()

    print("==> 编译 /vendor/bin/chipinfo（SoC 身份，域 chipinfo_exec）")
    out_bin = os.path.join(WORK_DIR, BINARY)
    compile_binary(tc, out_bin, dry_run=dry_run)

    if not dry_run:
        dst = os.path.join(VENDOR_STAGE_DIR, "bin", BINARY)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(out_bin, dst)
        os.chmod(dst, 0o755)

    print("==> rc 落盘（early-init 拉起 chipinfo）")
    write_asset(dry_run=dry_run)

    print("==> SELinux 标签登记")
    register_labels(dry_run=dry_run)

    print("[ok] chipinfo 构建完成")
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 chipinfo 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()