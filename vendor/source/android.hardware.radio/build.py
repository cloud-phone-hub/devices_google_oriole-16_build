#!/usr/bin/env python3
"""note13-5G-15 radio HAL 自研服务构建（本机 NDK 工具链 + 官方 radio AIDL v3 依赖）。

背景：lineage virtio 底座没有 radio HAL（/vendor/bin/hw 无 android.hardware.radio-service*，
vintf 也未声明），telephony 栈拿不到 IRadio 接口。本模块按 pixel6-15 vendor/radio 的
架构移植：guest 侧服务经 /dev/hvc4 与 host 侧 radio provider 以 cdh 频道协议收发
快照/命令/短信，实现只依赖 libbinder_ndk/libc++/liblog。
（v3 顺延 virtconsole 布局：radio = hvc4；hvc0/1 留给固件 console，hvc2 空置
预留，keymint 占 Cuttlefish 基线编号 hvc3。）

源码布局（本模块内，自包含）：
  aidl/aosp/ 官方 AOSP radio AIDL v3 展开快照（8 包 204 个 .aidl + 各包 .hash），
             由本模块 deps.py 同步；官方源为模块内缓存 aidl/cache-aidl-api
             （或 CDH_AIDL_AOSP 显式指定），不引用其他设备目录；
  aidl/mtk/  MTK radioex 补充接口（vendor.mediatek.hardware.mtkradioex.*，无官方源，
             随库维护，当前不参与编译，见 deps.py 注释）；
  src/        业务实现（channel 频道协议 + state/sim_file/data_policy + 7 个接口实现 + main）。

工具链（env 可覆盖；默认与本机实测一致）：
  CDH_AIDL     Android SDK build-tools aidl      （默认 36.1.0）
  CDH_CLANG    宿主 clang（默认 NDK r29 自带 clang 21）
  CDH_LLD      宿主 ld.lld（默认 NDK r29 自带 ld.lld）
  CDH_NDK      NDK r29                           （默认 SDK ndk/29.0.14206865）
  CDH_BINDER   platform libbinder_ndk_cpp 头     （默认 platforms/android-36）
  CDH_RUNTIME  运行库目录（libc++/libbinder_ndk/liblog）；默认由 deps.py 从
               note13 自身 lineage-22.0 super 组装，不借用其他设备镜像

编译期公共组件（binder 平台头 / bionic crt / libcxx __config_site）随包放在
build/vendor/source/common/ 下，自包含；radio 不依赖 AOSP platform 头
（无 fmq/cutils/utils，比 audio 模块更简）。

产物（相对路径即镜像内 /vendor 路径）：
  bin/hw/android.hardware.radio-service.cdh-note13   (0755, hal_radio_default_exec)
  etc/init/android.hardware.radio-service.cdh-note13.rc
  etc/vintf/manifest/android.hardware.radio-service.xml

SELinux：二进制/rc/vintf 文件标签经 baselines 登记；hal_radio_default 域的
service add/find、/dev/hvc4（console_device）访问与 net_admin/netlink 由
build/vendor/build/sepolicy.py radio_hal_patch_commands 追加（随 vendor 重打包
写入 vendor_sepolicy.cil + vendor_service_contexts）。

用法：
  python3 build/vendor/source/android.hardware.radio/build.py
  python3 build/vendor/source/android.hardware.radio/build.py --dry-run
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.radio/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import (  # noqa: E402
    OUT_DIR, VENDOR_STAGE_DIR, baselines_file,
)
import deps  # noqa: E402   note13 自身 lineage super 的编译依赖组装（运行库）

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
# AIDL 官方依赖同步脚本（本模块 deps.py）。注意：build/vendor/build/deps.py 已用
# `deps` 名字（运行时序库研磨），此处以绝对路径加载并绑定 radio_aidl_deps 避免冲突。
_radio_deps_spec = None
if os.path.isfile(os.path.join(MODULE_DIR, "deps.py")):
    import importlib.util  # noqa: E402
    _radio_deps_spec = importlib.util.spec_from_file_location(
        "radio_aidl_deps", os.path.join(MODULE_DIR, "deps.py"))
    radio_aidl_deps = importlib.util.module_from_spec(_radio_deps_spec)
    _radio_deps_spec.loader.exec_module(radio_aidl_deps)
# 公共编译组件（binder 平台头/bionic crt/libcxx config），随包于 build/vendor/source/common/
COMMON_DIR = os.path.join(os.path.dirname(MODULE_DIR), "common")
SRC_DIR = os.path.join(MODULE_DIR, "src")
AIDL_ROOT = os.path.join(MODULE_DIR, "aidl", "aosp")
# MTK radioex 补充 AIDL（vendor.mediatek.hardware.mtkradioex.*，无官方源、随库维护）。
# 独立 root：与 aosp 快照分开维护，codegen 时两个 -I 同时传入。
AIDL_MTK_ROOT = os.path.join(MODULE_DIR, "aidl", "mtk")
WORK_DIR = os.path.join(OUT_DIR, "tmp", "radio-ndk-work")

# 目标
TARGET = "aarch64-linux-android35"
BINARY = "android.hardware.radio-service.cdh-note13"
RC_NAME = "android.hardware.radio-service.cdh-note13.rc"
VINTF_NAME = "android.hardware.radio-service.xml"
AIDL_VERSION = 3

# 随包 AIDL 包：aidl/aosp 下相对路径（.hash 随包，逐包传给 aidl --hash）
AIDL_PACKAGES = [
    "android/hardware/radio",
    "android/hardware/radio/config",
    "android/hardware/radio/data",
    "android/hardware/radio/messaging",
    "android/hardware/radio/modem",
    "android/hardware/radio/network",
    "android/hardware/radio/sim",
    "android/hardware/radio/voice",
]

# MTK radioex 包（AIDL_MTK_ROOT 下）。无 .hash：ROM 侧接口未冻结
# （getInterfaceHash 为 default 方法），与我们生成的默认 hash 一致。
# 版本按 gold 的 VINTF 声明取 2（/vendor/etc/vintf/manifest.xml 里
# vendor.mediatek.hardware.mtkradioex.sim 是 <version>2</version>），本模块的
# manifest 用同一版本，避免声明版本与服务自报版本不一致。
AIDL_MTK_PACKAGES = [
    "vendor/mediatek/hardware/mtkradioex/rsu",
    "vendor/mediatek/hardware/mtkradioex/se",
    "vendor/mediatek/hardware/mtkradioex/sim",
]
AIDL_MTK_VERSION = 2

# 运行时链接库（全部来自基座/lineage，只参与链接；运行期由设备解析）
RUNTIME_LIBS = ["libc++.so", "libbinder_ndk.so"]
EXTRA_LIBS = ["-llog", "-ldl", "-lm", "-lc"]

LABEL_BIN = "u:object_r:hal_radio_default_exec:s0"
LABEL_CONFIG = "u:object_r:vendor_configs_file:s0"


def env(name, default):
    return os.environ.get(name) or default


def ndk_clang_rt_dir(prebuilt):
    """NDK clang 资源目录 lib/clang/<ver>，glob 探测避免写死版本号。"""
    base = os.path.join(prebuilt, "lib", "clang")
    cands = sorted(d for d in glob.glob(os.path.join(base, "*"))
                   if os.path.isdir(d))
    if not cands:
        raise RuntimeError(f"NDK clang 资源目录缺失: {base}")
    return cands[-1]


def toolchain(dry_run=False):
    ndk = env("CDH_NDK", os.path.expanduser(
        "~/Library/Android/sdk/ndk/29.0.14206865"))
    # NDK 自带工具链：clang/lld/sysroot/builtins 同源，不依赖宿主 brew llvm
    prebuilt = os.path.join(ndk, "toolchains", "llvm", "prebuilt",
                            "darwin-x86_64")
    return {
        "aidl": env("CDH_AIDL",
                    os.path.expanduser(
                        "~/Library/Android/sdk/build-tools/36.1.0/aidl")),
        "clang": env("CDH_CLANG", os.path.join(prebuilt, "bin", "clang")),
        "lld": env("CDH_LLD", os.path.join(prebuilt, "bin", "ld.lld")),
        "sysroot": os.path.join(prebuilt, "sysroot"),
        "clang_rt_dir": ndk_clang_rt_dir(prebuilt),
        "binder": env("CDH_BINDER",
                      os.path.expanduser(
                          "~/Library/Android/sdk/platforms/android-36/"
                          "optional/libbinder_ndk_cpp")),
        # 以下编译期组件随包放在 build/vendor/source/common/（自包含）
        "binder_platform": os.path.join(COMMON_DIR, "ndk-binder-platform"),
        "bionic": os.path.join(COMMON_DIR, "bionic15"),
        "libcxx_config": os.path.join(COMMON_DIR, "libcxx-platform"),
        # 运行库目录：默认由 deps.py 从 **note13 自身** lineage-22.0 super 组装
        "runtime_lib_dir": env("CDH_RUNTIME", None) or
        deps.prepare_runtime_libs(dry_run=dry_run),
    }


def run(argv, dry_run=False):
    if dry_run:
        print("[..] (dry)", " ".join(argv))
        return 0
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout, file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        sys.exit(1)
    return 0


def list_aidl(aidl_root, pkg_dir, recursive=True):
    """列出包下 .aidl。公共包 android/hardware/radio 的根含子包目录，
    逐包生成时只取其直属文件（子包各自单独走一次 codegen）。"""
    base = os.path.join(aidl_root, pkg_dir)
    out = []
    if recursive:
        for root, _dirs, files in os.walk(base):
            for f in sorted(files):
                if f.endswith(".aidl"):
                    out.append(os.path.join(root, f))
    else:
        for f in sorted(os.listdir(base)):
            if f.endswith(".aidl"):
                out.append(os.path.join(base, f))
    return out


def codegen(tc, gen_src, gen_hdr, dry_run):
    os.makedirs(gen_src, exist_ok=True)
    os.makedirs(gen_hdr, exist_ok=True)
    # 两类包：aosp 快照（带 .hash，逐包 --hash 校验）+ mtk radioex 补充（无 hash，
    # 版本 2，与 gold VINTF 声明一致）
    units = [(AIDL_ROOT, p, True, AIDL_VERSION) for p in AIDL_PACKAGES]
    units += [(AIDL_MTK_ROOT, p, False, AIDL_MTK_VERSION)
              for p in AIDL_MTK_PACKAGES]
    for aidl_root, pkg, has_hash, version in units:
        # 公共包 android/hardware/radio 是其余 7 个包的父目录，只取直属文件
        inputs = list_aidl(aidl_root, pkg,
                           recursive=(pkg != "android/hardware/radio"))
        if not inputs:
            print(f"[err] AIDL 包为空: {pkg}", file=sys.stderr)
            sys.exit(1)
        argv = [tc["aidl"], "--lang=ndk", "--structured", "--stability=vintf",
                "--min_sdk_version=35",
                f"--version={version}"]
        snap_hash = None
        if has_hash:
            hash_path = os.path.join(aidl_root, pkg, ".hash")
            if not os.path.isfile(hash_path):
                print(f"[err] 缺 AIDL 包 hash: {hash_path}", file=sys.stderr)
                sys.exit(1)
            with open(hash_path, encoding="ascii") as f:
                snap_hash = f.read().strip()
            argv.append(f"--hash={snap_hash}")
        argv.extend(["-I", AIDL_ROOT, "-I", AIDL_MTK_ROOT,
                     "--out=" + gen_src, "--header_out=" + gen_hdr])
        argv.extend(inputs)
        run(argv, dry_run=dry_run)
        if snap_hash:
            print(f"    {pkg}: {len(inputs)} aidl (hash {snap_hash[:12]})")
        else:
            print(f"    {pkg}: {len(inputs)} aidl (mtk, 无 hash)")


def compile_one(tc, gen_hdr, src, obj, dry_run):
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "-std=c++20", "-fPIC", "-fno-exceptions",
        "-fno-rtti", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-deprecated-declarations",
        "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
        "-nostdinc++", "-I" + tc["libcxx_config"],
        "-isystem", os.path.join(tc["sysroot"], "usr", "include", "c++", "v1"),
    ]
    argv.extend(["-c", src, "-o", obj])
    if dry_run:
        return
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout, file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        sys.exit(1)


def collect_gen_cpps(gen_src):
    cpps = []
    for root, _dirs, files in os.walk(gen_src):
        for f in sorted(files):
            if f.endswith(".cpp"):
                cpps.append(os.path.join(root, f))
    return sorted(cpps)


def link_service(tc, gen_hdr, objs, crtbegin, crtend, runtime_lib_dir,
                 out_bin, dry_run):
    # aarch64 LSE/atomic 完备实现（__aarch64_ldadd* 等）来自 NDK compiler-rt builtins
    builtins = os.path.join(tc["clang_rt_dir"], "lib", "linux",
                            "libclang_rt.builtins-aarch64-android.a")
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "--ld-path=" + tc["lld"], "-nostdlib", "-fno-stack-protector",
        "-mno-outline-atomics", "-mbranch-protection=standard",
        "-Wl,--dynamic-linker,/system/bin/linker64", "-Wl,-z,relro",
        "-Wl,-z,now", "-std=c++20", "-fPIE", "-pie",
        "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
        "-nostdinc++", "-I" + tc["libcxx_config"],
        "-isystem", os.path.join(tc["sysroot"], "usr", "include", "c++", "v1"),
        crtbegin,
    ]
    argv.extend(objs)
    argv.extend([crtend, "-L" + runtime_lib_dir])
    argv.extend(os.path.join(runtime_lib_dir, lib) for lib in RUNTIME_LIBS)
    argv.extend(EXTRA_LIBS)
    argv.extend([builtins, "-o", out_bin])
    run(argv, dry_run=dry_run)


def write_rc(dst):
    """拷贝 init rc 到 staging。源文件位于模块内 vendor/etc/init/ 下。"""
    src = os.path.join(MODULE_DIR, "vendor", "etc", "init", RC_NAME)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(src, encoding="utf-8") as f:
        content = f.read()
    with open(dst, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"    写 {os.path.relpath(dst, VENDOR_STAGE_DIR)}")


def write_vintf(dst):
    """拷贝 VINTF manifest 到 staging。源文件位于模块内 vendor/etc/vintf/manifest/ 下。"""
    src = os.path.join(MODULE_DIR, "vendor", "etc", "vintf", "manifest",
                       os.path.basename(dst))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(src, encoding="utf-8") as f:
        content = f.read()
    with open(dst, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"    写 {os.path.relpath(dst, VENDOR_STAGE_DIR)}")


def register_labels(baselines_path, dry_run=False):
    if not baselines_path or not os.path.isfile(baselines_path):
        print(f"[err] 基线清单缺失: {baselines_path or 'None'}", file=sys.stderr)
        sys.exit(1)
    with open(baselines_path, encoding="utf-8") as f:
        doc = json.load(f)
    labels = doc.setdefault("vendor", {}).setdefault("selinux-labels", {})
    entries = {
        f"bin/hw/{BINARY}": LABEL_BIN,
        f"etc/init/{RC_NAME}": LABEL_CONFIG,
        f"etc/vintf/manifest/{VINTF_NAME}": LABEL_CONFIG,
    }
    missing = {k: v for k, v in entries.items() if labels.get(k) != v}
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


def build(dry_run=False):
    tc = toolchain(dry_run=dry_run)
    gen_src = os.path.join(WORK_DIR, "gen", "src")
    gen_hdr = os.path.join(WORK_DIR, "gen", "hdr")
    obj_dir = os.path.join(WORK_DIR, "obj")
    os.makedirs(WORK_DIR, exist_ok=True)
    os.makedirs(obj_dir, exist_ok=True)

    print("==> AIDL 官方依赖同步")
    radio_aidl_deps.prepare(quiet=dry_run)

    print("==> AIDL 代码生成")
    if not dry_run:
        for d in (gen_src, gen_hdr):
            if os.path.isdir(d):
                for root, _dirs, files in os.walk(d):
                    for f in files:
                        os.unlink(os.path.join(root, f))
    codegen(tc, gen_src, gen_hdr, dry_run)

    gen_cpps = collect_gen_cpps(gen_src)
    src_cpps = [f for f in sorted(os.listdir(SRC_DIR)) if f.endswith(".cpp")]
    print(f"==> 编译 {len(gen_cpps)}+{len(src_cpps)} 个源文件")

    all_srcs = gen_cpps + [os.path.join(SRC_DIR, f) for f in src_cpps]
    objs = []
    for i, src in enumerate(all_srcs):
        obj = os.path.join(obj_dir, f"o{i}.o")
        compile_one(tc, gen_hdr, src, obj, dry_run)
        objs.append(obj)

    crtbegin = os.path.join(tc["bionic"], "cdh_crtbegin_dynamic.cpp")
    crtend = os.path.join(tc["bionic"], "cdh_crtend_android.S")

    print("==> 链接服务二进制")
    out_bin = os.path.join(VENDOR_STAGE_DIR, "bin", "hw", BINARY)
    os.makedirs(os.path.dirname(out_bin), exist_ok=True)
    link_service(tc, gen_hdr, objs, crtbegin, crtend,
                 tc["runtime_lib_dir"], out_bin, dry_run)
    if not dry_run:
        os.chmod(out_bin, 0o755)
        print(f"    写 {os.path.relpath(out_bin, VENDOR_STAGE_DIR)}")

    print("==> rc / vintf")
    write_rc(os.path.join(VENDOR_STAGE_DIR, "etc", "init", RC_NAME))
    write_vintf(os.path.join(VENDOR_STAGE_DIR, "etc", "vintf", "manifest",
                             VINTF_NAME))

    print("==> SELinux 标签登记")
    register_labels(baselines_file(), dry_run=dry_run)

    print("[ok] 自研 radio HAL 构建完成")
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 自研 radio HAL 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()
