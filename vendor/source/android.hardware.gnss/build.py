#!/usr/bin/env python3
"""note13-5G-15 gnss HAL 自研服务构建（本机 NDK 工具链 + 官方 gnss AIDL 依赖）。

背景：lineage virtio 底座没有 GNSS HAL（/vendor/bin/hw 无 gnss 服务、vendor VINTF
也未声明 android.hardware.gnss.IGnss），GnssLocationProvider 因此不创建，LocationManager
里没有 gps provider。本模块按 pixel6-15 vendor/gnss 的架构移植：自研 AIDL 服务实现
android.hardware.gnss.IGnss 及其全部扩展子接口，按真机（MTK MNLD）的 systemInfo/
能力位/config 默认 fix 上报；start/stop 驱动状态机，位置仅来自 config 默认值、
LocationManager 注入或控制文件（/data/vendor/gps/fix）。

源码布局（本模块内，自包含）：
  aidl/aosp/           官方 AOSP gnss AIDL 展开快照（android.hardware.gnss@4），
                       由本模块 deps.py 同步；
  config/gnss.json      默认 fix / 卫星数 / 能力位 / 真机 systemInfo，build.py 据此
                       生成 gnss_target.inc；
  src/                 业务实现（IGnss + 12 个扩展子接口）。
  vendor/etc/permissions/android.hardware.location.gps.xml
                       真机的 GPS 特性声明（location + location.gps），否则
                       PackageManager 看不到 android.hardware.location.gps。

工具链（env 可覆盖；默认与本机实测一致）：同 radio/sensors/thermal/vibrator/lights
模块（NDK r29 + 本机 aidl）。

产物（相对路径即镜像内 /vendor 路径）：
  bin/hw/android.hardware.gnss-service.cdh-note13   (0755, hal_gnss_default_exec)
  etc/init/android.hardware.gnss-service.cdh-note13.rc
  etc/vintf/manifest/android.hardware.gnss-service.xml
  etc/permissions/android.hardware.location.gps.xml

SELinux：二进制/rc/vintf/permissions 标签经 baselines 登记；hal_gnss_default 域的
service add/find 与控制文件读写由 build/vendor/build/sepolicy.py
gnss_hal_patch_commands 追加。

用法：
  python3 build/vendor/source/android.hardware.gnss/build.py
  python3 build/vendor/source/android.hardware.gnss/build.py --dry-run
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.gnss/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import (  # noqa: E402
    OUT_DIR, VENDOR_STAGE_DIR, baselines_file,
)
import deps  # noqa: E402   note13 自身 lineage super 的编译依赖组装（运行库）

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
COMMON_DIR = os.path.join(os.path.dirname(MODULE_DIR), "common")
SRC_DIR = os.path.join(MODULE_DIR, "src")
AIDL_ROOT = os.path.join(MODULE_DIR, "aidl", "aosp")
CONFIG_PATH = os.path.join(MODULE_DIR, "config", "gnss.json")
WORK_DIR = os.path.join(OUT_DIR, "tmp", "gnss-ndk-work")

# 本模块的 deps.py（AIDL 依赖）与 build/vendor/build/deps.py（运行库）同名，用绝对路径加载
import importlib.util  # noqa: E402
_spec = importlib.util.spec_from_file_location(
    "gnss_aidl_deps", os.path.join(MODULE_DIR, "deps.py"))
gnss_aidl_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gnss_aidl_deps)

# 目标
TARGET = "aarch64-linux-android35"
BINARY = "android.hardware.gnss-service.cdh-note13"
RC_NAME = "android.hardware.gnss-service.cdh-note13.rc"
VINTF_NAME = "android.hardware.gnss-service.xml"
PERM_NAME = "android.hardware.location.gps.xml"
TARGETS_INC = "gnss_target.inc"

# AIDL 包：<展开相对路径> -> <版本>（.hash 随包，传给 aidl --hash）
AIDL_PACKAGES = {
    "android/hardware/gnss": 4,
}

# 运行时链接库（全部来自基座/lineage，只参与链接；运行期由设备解析）
RUNTIME_LIBS = ["libc++.so", "libbinder_ndk.so"]
EXTRA_LIBS = ["-llog", "-ldl", "-lm", "-lc"]

LABEL_BIN = "u:object_r:hal_gnss_default_exec:s0"
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
        # 编译期组件随包放在 build/vendor/source/common/（自包含）
        "binder_platform": os.path.join(COMMON_DIR, "ndk-binder-platform"),
        "bionic": os.path.join(COMMON_DIR, "bionic15"),
        "libcxx_config": os.path.join(COMMON_DIR, "libcxx-platform"),
        # 运行库目录：默认由 deps.py 从 note13 自身 lineage-22.0 super 组装
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
            # .hash 可能含多个字段，取最后一个非空字段为原生 hash
            snap_hash = f.read().split()[-1]
        argv = [tc["aidl"], "--lang=ndk", "--structured", "--stability=vintf",
                "--min_sdk_version=35",
                f"--version={version}", f"--hash={snap_hash}",
                "-I", AIDL_ROOT, "--out=" + gen_src, "--header_out=" + gen_hdr]
        argv.extend(inputs)
        run(argv, dry_run=dry_run)
        print(f"    {rel}: {len(inputs)} aidl (v{version}, hash {snap_hash[:12]})")


def write_gnss_targets(path, config_path, dry_run=False):
    """由 config/gnss.json 生成 gnss_target.inc，提供源文件 include 的编译期常量：

      kControlPath                           fix 注入控制文件（真机私有 lbs 链路的替代）
      kDefaultLongitude/Latitude/Altitude/Speed/HorizontalAccuracy/DefaultSatellites
                                             start() 与未注入时的默认 fix
      kTargetGnssCapabilities                IGnssCallback.gnssSetCapabilitiesCb
      kTargetPowerCapabilities               IGnssPowerIndicationCallback.setCapabilitiesCb
      kTargetMeasurementCorrectionsCapabilities
      kTargetYearOfHw / kTargetSystemInfoName  IGnssCallback.GnssSystemInfo

    等效 pixel6 的 Go 生成器（gnss_target.inc）。double 用 repr 保证精度（%g 只有
    6 位有效数字，会把 120.008549 截断成 120.009）。
    """
    with open(config_path, encoding="utf-8") as f:
        doc = json.load(f)
    t = doc["targets"]
    name = str(t["systemInfoName"])
    lines = [
        "static constexpr const char *kControlPath = " +
        json.dumps(str(doc["controlFile"])) + ";",
        "static constexpr double kDefaultLongitude = " +
        repr(float(t["defaultLongitude"])) + ";",
        "static constexpr double kDefaultLatitude = " +
        repr(float(t["defaultLatitude"])) + ";",
        "static constexpr double kDefaultAltitudeMeters = " +
        repr(float(t["defaultAltitudeMeters"])) + ";",
        "static constexpr double kDefaultSpeedMetersPerSecond = " +
        repr(float(t["defaultSpeedMetersPerSecond"])) + ";",
        "static constexpr double kDefaultHorizontalAccuracyMeters = " +
        repr(float(t["horizontalAccuracyMeters"])) + ";",
        "static constexpr int kDefaultSatellites = " +
        str(int(t["defaultSatellites"])) + ";",
        "static constexpr int32_t kTargetGnssCapabilities = " +
        str(int(t["gnssCapabilities"])) + ";",
        "static constexpr int32_t kTargetPowerCapabilities = " +
        str(int(t["powerCapabilities"])) + ";",
        "static constexpr int32_t kTargetMeasurementCorrectionsCapabilities = " +
        str(int(t["measurementCorrectionsCapabilities"])) + ";",
        "static constexpr int32_t kTargetYearOfHw = " +
        str(int(t["yearOfHw"])) + ";",
        "static constexpr const char *kTargetSystemInfoName = " +
        json.dumps(name) + ";",
        "",
    ]
    if dry_run:
        print(f"[..] (dry) 生成 {TARGETS_INC}（默认 fix "
              f"{t['defaultLongitude']},{t['defaultLatitude']}，"
              f"{t['defaultSatellites']} 星，capabilities={t['gnssCapabilities']}）")
        return int(doc.get("aidlVersion", 4))
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"// Generated from {os.path.relpath(config_path, _DEVICE_ROOT)}."
                " Do not edit by hand.\n" + "\n".join(lines))
    print(f"    生成 {TARGETS_INC}（默认 fix {t['defaultLongitude']},"
          f"{t['defaultLatitude']}，{t['defaultSatellites']} 星，"
          f"capabilities={t['gnssCapabilities']}）")
    return int(doc.get("aidlVersion", 4))


def compile_one(tc, gen_hdr, work_dir, src, obj, aidl_version, dry_run):
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "-std=c++17", "-fPIC", "-fno-exceptions",
        "-fno-rtti", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter",
        f"-DCDH_GNSS_AIDL_VERSION={aidl_version}",
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


def collect_gen_cpps(gen_src):
    cpps = []
    for dirpath, _dirs, files in os.walk(gen_src):
        for name in sorted(files):
            if name.endswith(".cpp"):
                cpps.append(os.path.join(dirpath, name))
    return sorted(cpps)


def link_service(tc, gen_hdr, work_dir, objs, crtbegin, crtend, runtime_lib_dir,
                 out_bin, aidl_version, dry_run):
    builtins = os.path.join(tc["clang_rt_dir"], "lib", "linux",
                            "libclang_rt.builtins-aarch64-android.a")
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "--ld-path=" + tc["lld"], "-nostdlib", "-fno-stack-protector",
        "-mno-outline-atomics", "-mbranch-protection=standard",
        "-Wl,--dynamic-linker,/system/bin/linker64", "-Wl,-z,relro",
        "-Wl,-z,now", "-std=c++17", "-fPIE", "-pie",
        f"-DCDH_GNSS_AIDL_VERSION={aidl_version}",
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
    """把模块内 vendor/ 下的源文件拷到 staging。"""
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
        f"etc/permissions/{PERM_NAME}": LABEL_CONFIG,
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
    gnss_aidl_deps.prepare(quiet=dry_run)

    print("==> 默认 fix / 能力位（config/gnss.json）")
    aidl_version = write_gnss_targets(os.path.join(WORK_DIR, TARGETS_INC),
                                      CONFIG_PATH, dry_run=dry_run)

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
        compile_one(tc, gen_hdr, WORK_DIR, src, obj, aidl_version, dry_run)
        objs.append(obj)

    crtbegin = os.path.join(tc["bionic"], "cdh_crtbegin_dynamic.cpp")
    crtend = os.path.join(tc["bionic"], "cdh_crtend_android.S")

    print("==> 链接服务二进制")
    out_bin = os.path.join(VENDOR_STAGE_DIR, "bin", "hw", BINARY)
    os.makedirs(os.path.dirname(out_bin), exist_ok=True)
    link_service(tc, gen_hdr, WORK_DIR, objs, crtbegin, crtend,
                 tc["runtime_lib_dir"], out_bin, aidl_version, dry_run)
    if not dry_run:
        os.chmod(out_bin, 0o755)
        print(f"    写 {os.path.relpath(out_bin, VENDOR_STAGE_DIR)}")

    print("==> rc / vintf / permissions")
    write_asset(os.path.join("vendor", "etc", "init", RC_NAME),
                os.path.join(VENDOR_STAGE_DIR, "etc", "init", RC_NAME),
                dry_run=dry_run)
    write_asset(os.path.join("vendor", "etc", "vintf", "manifest", VINTF_NAME),
                os.path.join(VENDOR_STAGE_DIR, "etc", "vintf", "manifest",
                             VINTF_NAME),
                dry_run=dry_run)
    write_asset(os.path.join("vendor", "etc", "permissions", PERM_NAME),
                os.path.join(VENDOR_STAGE_DIR, "etc", "permissions", PERM_NAME),
                dry_run=dry_run)

    print("==> SELinux 标签登记")
    register_labels(baselines_file(), dry_run=dry_run)

    print("[ok] 自研 gnss HAL 构建完成")
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 自研 gnss HAL 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()
