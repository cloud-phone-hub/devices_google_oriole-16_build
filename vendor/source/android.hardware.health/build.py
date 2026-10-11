#!/usr/bin/env python3
"""note13-5G-15 health HAL 自研服务构建（本机 NDK 工具链 + 官方 health AIDL 依赖）。

形态与真机（gold）一致：
  /vendor/bin/hw/android.hardware.health-service.example         (0755, hal_health_default_exec)
  /vendor/etc/init/android.hardware.health-service.example.rc    (vendor.health-default)
  /vendor/etc/vintf/manifest/android.hardware.health-service.example.xml（IHealth/default v3）

背景：lineage virtio 底座把 health HAL 打进 com.google.cf.health.apex，本机不重定向
/vendor/apex → MIUI BatteryService 崩溃循环（见 issue-vendor-apex-health）。此前方案是
从 APEX 提取 cuttlefish 二进制落盘；本模块改为**源码移植**：AOSP 接口（aidl/aosp 固化
v3 冻结快照）+ 自研实现 src/cdh_note13_health_service.cpp（电池数据按真机口径模拟），
用本机 NDK 工具链编译成与真机同名的普通 vendor 服务。

工具链（env 可覆盖）：同 lights/radio/sensors 模块（NDK r29 + 本机 aidl）。

用法:
  python3 build/vendor/source/android.hardware.health/build.py
  python3 build/vendor/source/android.hardware.health/build.py --dry-run
"""

import argparse
import glob
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.health/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import OUT_DIR, VENDOR_STAGE_DIR, baselines_file  # noqa: E402
import deps as runlib_deps  # noqa: E402   lineage super 运行库组装

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
COMMON_DIR = os.path.join(os.path.dirname(MODULE_DIR), "common")
SRC_DIR = os.path.join(MODULE_DIR, "src")
AIDL_ROOT = os.path.join(MODULE_DIR, "aidl", "aosp")
WORK_DIR = os.path.join(OUT_DIR, "tmp", "health-ndk-work")

# 模块 deps.py（AIDL）与 build/vendor/build/deps.py（运行库）同名，按绝对路径加载
_spec = importlib.util.spec_from_file_location(
    "health_aidl_deps", os.path.join(MODULE_DIR, "deps.py"))
health_aidl_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(health_aidl_deps)

TARGET = "aarch64-linux-android35"
BINARY = "android.hardware.health-service.example"
RC_NAME = "android.hardware.health-service.example.rc"
VINTF_NAME = "android.hardware.health-service.example.xml"

AIDL_PACKAGES = {
    "android/hardware/health": 3,
}

RUNTIME_LIBS = ["libc++.so", "libbinder_ndk.so"]
EXTRA_LIBS = ["-llog", "-ldl", "-lm", "-lc"]

LABEL_BIN = "u:object_r:hal_health_default_exec:s0"
LABEL_CONFIG = "u:object_r:vendor_configs_file:s0"

# 旧方案（APEX 提取 cuttlefish 二进制）的产物：构建时清理，避免两个 health HAL 抢注
# android.hardware.health.IHealth/default（实测同实例双注册 init 报错）。
LEGACY_FILES = [
    "bin/hw/android.hardware.health-service.cuttlefish",
    "etc/init/android.hardware.health-service.cuttlefish.rc",
    "etc/vintf/manifest/android.hardware.health-service.cuttlefish.xml",
]


def env(name, default):
    return os.environ.get(name) or default


def ndk_clang_rt_dir(prebuilt):
    base = os.path.join(prebuilt, "lib", "clang")
    cands = sorted(d for d in glob.glob(os.path.join(base, "*"))
                   if os.path.isdir(d))
    if not cands:
        raise RuntimeError(f"NDK clang 资源目录缺失: {base}")
    return cands[-1]


def toolchain(dry_run=False):
    ndk = env("CDH_NDK", os.path.expanduser(
        "~/Library/Android/sdk/ndk/29.0.14206865"))
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
        "binder_platform": os.path.join(COMMON_DIR, "ndk-binder-platform"),
        "bionic": os.path.join(COMMON_DIR, "bionic15"),
        "libcxx_config": os.path.join(COMMON_DIR, "libcxx-platform"),
        "runtime_lib_dir": env("CDH_RUNTIME", None) or
        runlib_deps.prepare_runtime_libs(dry_run=dry_run),
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


def codegen(tc, gen_src, gen_hdr, dry_run):
    os.makedirs(gen_src, exist_ok=True)
    os.makedirs(gen_hdr, exist_ok=True)
    for rel, version in AIDL_PACKAGES.items():
        base = os.path.join(AIDL_ROOT, rel)
        inputs = []
        for dirpath, _dirs, files in os.walk(base):
            for name in sorted(files):
                if name.endswith(".aidl"):
                    inputs.append(os.path.join(dirpath, name))
        if not inputs:
            print(f"[err] AIDL 包为空: {rel}", file=sys.stderr)
            sys.exit(1)
        hash_path = os.path.join(base, ".hash")
        if not os.path.isfile(hash_path):
            print(f"[err] 缺 AIDL 包 hash: {hash_path}", file=sys.stderr)
            sys.exit(1)
        with open(hash_path, encoding="ascii") as f:
            snap_hash = f.read().split()[-1]
        argv = [tc["aidl"], "--lang=ndk", "--structured",
                "--stability=vintf", "--min_sdk_version=35",
                f"--version={version}", f"--hash={snap_hash}",
                "-I", AIDL_ROOT, "--out=" + gen_src,
                "--header_out=" + gen_hdr]
        argv.extend(inputs)
        run(argv, dry_run=dry_run)
        print(f"    {rel}: {len(inputs)} aidl (v{version}, hash {snap_hash[:12]})")


def collect_gen_cpps(gen_src):
    cpps = []
    for dirpath, _dirs, files in os.walk(gen_src):
        for name in sorted(files):
            if name.endswith(".cpp"):
                cpps.append(os.path.join(dirpath, name))
    return sorted(cpps)


def compile_one(tc, gen_hdr, work_dir, src, obj, dry_run):
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "-std=c++17", "-fPIC", "-fno-exceptions", "-fno-rtti",
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
        "-I" + work_dir,
        "-nostdinc++", "-I" + tc["libcxx_config"],
        "-isystem", os.path.join(tc["sysroot"], "usr", "include", "c++", "v1"),
        "-c", src, "-o", obj,
    ]
    if dry_run:
        return
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout, file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        sys.exit(1)


def link_service(tc, gen_hdr, work_dir, objs, crtbegin, crtend,
                 runtime_lib_dir, out_bin, dry_run):
    builtins = os.path.join(tc["clang_rt_dir"], "lib", "linux",
                            "libclang_rt.builtins-aarch64-android.a")
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "--ld-path=" + tc["lld"], "-nostdlib", "-fno-stack-protector",
        "-mno-outline-atomics", "-mbranch-protection=standard",
        "-Wl,--dynamic-linker,/system/bin/linker64", "-Wl,-z,relro",
        "-Wl,-z,now", "-std=c++17", "-fPIE", "-pie",
        "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
        "-I" + work_dir,
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


def write_asset(src_rel, dst, dry_run=False):
    src = os.path.join(MODULE_DIR, src_rel)
    if dry_run:
        print(f"[..] (dry) 写 {os.path.relpath(dst, VENDOR_STAGE_DIR)}")
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(src, encoding="utf-8") as f:
        content = f.read()
    with open(dst, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"    写 {os.path.relpath(dst, VENDOR_STAGE_DIR)}")


def cleanup_legacy():
    """删除旧方案（APEX 提取）落到 staging 的三个文件。"""
    removed = []
    for rel in LEGACY_FILES:
        p = os.path.join(VENDOR_STAGE_DIR, rel)
        if os.path.isfile(p):
            os.unlink(p)
            removed.append(rel)
    if removed:
        print(f"[ok] 清理旧 APEX 提取产物 {len(removed)} 个: "
              + ", ".join(removed))


def update_labels(baselines_path, dry_run=False):
    """baselines 标签：登记新三件套并移除旧产物标签（inject.py 要求逐文件有标签）。"""
    if not baselines_path or not os.path.isfile(baselines_path):
        print(f"[err] 基线清单缺失: {baselines_path or 'None'}", file=sys.stderr)
        sys.exit(1)
    with open(baselines_path, encoding="utf-8") as f:
        doc = json.load(f)
    labels = doc.setdefault("vendor", {}).setdefault("selinux-labels", {})
    want = {
        f"bin/hw/{BINARY}": LABEL_BIN,
        f"etc/init/{RC_NAME}": LABEL_CONFIG,
        f"etc/vintf/manifest/{VINTF_NAME}": LABEL_CONFIG,
    }
    drop = [k for k in LEGACY_FILES if k in labels]
    missing = {k: v for k, v in want.items() if labels.get(k) != v}
    if not missing and not drop:
        print("[..] baselines 标签已齐，跳过")
        return
    labels.update(missing)
    for k in drop:
        labels.pop(k, None)
    if dry_run:
        print(f"[..] (dry) baselines 登记 {len(missing)} 条、移除 {len(drop)} 条")
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
    print(f"[ok] baselines 标签: +{len(missing)} -{len(drop)}")


def build(dry_run=False):
    tc = toolchain(dry_run=dry_run)
    gen_src = os.path.join(WORK_DIR, "gen", "src")
    gen_hdr = os.path.join(WORK_DIR, "gen", "hdr")
    obj_dir = os.path.join(WORK_DIR, "obj")
    os.makedirs(WORK_DIR, exist_ok=True)
    os.makedirs(obj_dir, exist_ok=True)

    print("==> AIDL 官方依赖同步")
    health_aidl_deps.prepare(quiet=dry_run)

    print("==> AIDL 代码生成")
    if not dry_run:
        for d in (gen_src, gen_hdr):
            if os.path.isdir(d):
                for root, _dirs, files in os.walk(d):
                    for name in files:
                        os.unlink(os.path.join(root, name))
    codegen(tc, gen_src, gen_hdr, dry_run)

    gen_cpps = collect_gen_cpps(gen_src)
    src_cpps = [f for f in sorted(os.listdir(SRC_DIR)) if f.endswith(".cpp")]
    print(f"==> 编译 {len(gen_cpps)}+{len(src_cpps)} 个源文件")

    all_srcs = gen_cpps + [os.path.join(SRC_DIR, f) for f in src_cpps]
    objs = []
    for i, src in enumerate(all_srcs):
        obj = os.path.join(obj_dir, f"o{i}.o")
        compile_one(tc, gen_hdr, WORK_DIR, src, obj, dry_run)
        objs.append(obj)

    crtbegin = os.path.join(tc["bionic"], "cdh_crtbegin_dynamic.cpp")
    crtend = os.path.join(tc["bionic"], "cdh_crtend_android.S")

    print("==> 链接服务二进制")
    out_bin = os.path.join(VENDOR_STAGE_DIR, "bin", "hw", BINARY)
    os.makedirs(os.path.dirname(out_bin), exist_ok=True)
    link_service(tc, gen_hdr, WORK_DIR, objs, crtbegin, crtend,
                 tc["runtime_lib_dir"], out_bin, dry_run)
    if not dry_run:
        os.chmod(out_bin, 0o755)
        print(f"    写 {os.path.relpath(out_bin, VENDOR_STAGE_DIR)}")

    print("==> rc / vintf")
    write_asset(os.path.join("vendor", "etc", "init", RC_NAME),
                os.path.join(VENDOR_STAGE_DIR, "etc", "init", RC_NAME),
                dry_run=dry_run)
    write_asset(os.path.join("vendor", "etc", "vintf", "manifest", VINTF_NAME),
                os.path.join(VENDOR_STAGE_DIR, "etc", "vintf", "manifest",
                             VINTF_NAME), dry_run=dry_run)

    print("==> 旧产物清理")
    if not dry_run:
        cleanup_legacy()

    print("==> SELinux 标签登记")
    update_labels(baselines_file(), dry_run=dry_run)

    print("[ok] health HAL（源码移植，与真机同形）构建完成")
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 health HAL 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()