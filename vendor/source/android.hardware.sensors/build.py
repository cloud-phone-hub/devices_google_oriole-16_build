#!/usr/bin/env python3
"""note13-5G-15 sensors HAL 自研服务构建（本机 NDK 工具链 + 官方 sensors AIDL 依赖）。

背景：lineage virtio 底座没有 sensors HAL（/vendor/bin/hw 无 sensors 服务、vendor
VINTF/vendor_service_contexts 也没有对应表项），SensorService 起不来
（`dumpsys sensorservice` = "No Sensors on the device"）。本模块按 pixel6-15
vendor/sensors 的架构移植：自研 AIDL 服务实现 android.hardware.sensors.ISensors，
上报 note13 真机的传感器清单并生成虚拟事件流（FMQ 上报），只依赖
libbinder_ndk / libc++ / liblog。

源码布局（本模块内，自包含）：
  aidl/aosp/  官方 AOSP sensors AIDL 展开快照（3 包 + .hash），由本模块 deps.py 同步；
  config/sensors.json  传感器清单（真机 dumpsys + getSensorList 回包导出），
               build.py 据此生成 note13_sensor_targets.inc；
  src/        业务实现（sensors 服务 + 事件模型 + FMQ 支撑）；
  platform-shim/log/log.h  FMQ 模板需要的 log 头（NDK 真 android/log.h）。

工具链（env 可覆盖；默认与本机实测一致）：
  CDH_AIDL / CDH_CLANG / CDH_LLD / CDH_NDK / CDH_BINDER / CDH_RUNTIME 同 radio 模块。

产物（相对路径即镜像内 /vendor 路径）：
  bin/hw/android.hardware.sensors-service.cdh-note13   (0755, hal_sensors_default_exec)
  etc/init/android.hardware.sensors-service.cdh-note13.rc
  etc/vintf/manifest/android.hardware.sensors-service.xml

SELinux：二进制/rc/vintf 标签经 baselines 登记；hal_sensors_default 域的
service add/find、ashmem（FMQ）访问由 build/vendor/build/sepolicy.py
sensors_hal_patch_commands 追加（随 vendor 重打包写入 vendor_sepolicy.cil +
vendor_service_contexts）。

用法：
  python3 build/vendor/source/android.hardware.sensors/build.py
  python3 build/vendor/source/android.hardware.sensors/build.py --dry-run
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.sensors/ 下，向上 5 级为设备根
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
CONFIG_PATH = os.path.join(MODULE_DIR, "config", "sensors.json")
WORK_DIR = os.path.join(OUT_DIR, "tmp", "sensors-ndk-work")

# 本模块的 deps.py（AIDL 依赖）与 build/vendor/build/deps.py（运行库）同名，用绝对路径加载
import importlib.util  # noqa: E402
_spec = importlib.util.spec_from_file_location(
    "sensors_aidl_deps", os.path.join(MODULE_DIR, "deps.py"))
sensors_aidl_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sensors_aidl_deps)

# 目标
TARGET = "aarch64-linux-android35"
BINARY = "android.hardware.sensors-service.cdh-note13"
RC_NAME = "android.hardware.sensors-service.cdh-note13.rc"
VINTF_NAME = "android.hardware.sensors-service.xml"
TARGETS_INC = "note13_sensor_targets.inc"

# AIDL 包：<展开相对路径> -> <版本>（.hash 随包，逐包传给 aidl --hash）
AIDL_PACKAGES = {
    "android/hardware/common/fmq": 1,
    "android/hardware/common": 2,
    "android/hardware/sensors": 2,
}

# 运行时链接库（全部来自基座/lineage，只参与链接；运行期由设备解析）
RUNTIME_LIBS = ["libc++.so", "libbinder_ndk.so"]
EXTRA_LIBS = ["-llog", "-ldl", "-lm", "-lc"]

LABEL_BIN = "u:object_r:hal_sensors_default_exec:s0"
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
        # AOSP platform 头（libfmq 模板、cutils/utils/libbase），仅编译期使用
        "platform_source": os.path.join(COMMON_DIR, "platform-headers"),
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


def platform_extras(tc):
    """AOSP platform 头（libfmq 模板等）——用 -isystem 避免其自带警告触发 -Werror。"""
    ps = tc["platform_source"]
    return ["-isystem", os.path.join(ps, "system", "libfmq", "include"),
            "-isystem", os.path.join(ps, "system", "libfmq", "base"),
            "-isystem", os.path.join(ps, "system", "core", "libcutils", "include"),
            "-isystem", os.path.join(ps, "system", "core", "libutils", "include"),
            # 本模块的 log/log.h shim（NDK 真 android/log.h；不用 liblog_stub 以免遮蔽）
            "-isystem", os.path.join(MODULE_DIR, "platform-shim"),
            "-isystem", os.path.join(ps, "system", "libbase", "include")]


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
            snap_hash = f.read().strip()
        argv = [tc["aidl"], "--lang=ndk", "--structured", "--stability=vintf",
                "--min_sdk_version=35",
                f"--version={version}", f"--hash={snap_hash}",
                "-I", AIDL_ROOT, "--out=" + gen_src, "--header_out=" + gen_hdr]
        argv.extend(inputs)
        run(argv, dry_run=dry_run)
        print(f"    {rel}: {len(inputs)} aidl (v{version}, hash {snap_hash[:12]})")


def write_targets_inc(path, config_path, dry_run=False):
    """由 config/sensors.json 生成 note13_sensor_targets.inc（源 include 该文件）。

    字段顺序与 cdh_note13_sensors_service.cpp 的 SensorTarget 一致；
    maxDelayUs 直接取 config/sensors.json 的 maxDelay（真机 getSensorList 实测值，
    真机各传感器该值并不统一：连续型 20000/80000/200000、批处理型 1000000，
    不再按旧规则近似为 0/200000）。
    """
    with open(config_path, encoding="utf-8") as f:
        doc = json.load(f)
    lines = [f"// Generated from {os.path.relpath(config_path, _DEVICE_ROOT)}.",
             "// Do not edit by hand.",
             "static const SensorTarget kNote13SensorTargets[] = {"]
    for sensor in doc["sensors"]:
        min_delay = int(sensor["minDelay"])
        max_delay = int(sensor["maxDelay"])
        lines.append(
            "    {{{handle}, {name}, {vendor}, {version}, {type}, {type_string}, "
            "{max_range}f, {resolution}f, {power}f, {min_delay}, {fifo_reserved}, "
            "{fifo_max}, {max_delay}, {wake_up}, {reporting_mode}}},".format(
                handle=int(sensor["handle"]),
                name=json.dumps(sensor["name"], ensure_ascii=False),
                vendor=json.dumps(sensor["vendor"], ensure_ascii=False),
                version=int(sensor["version"]),
                type=int(sensor["type"]),
                type_string=json.dumps(sensor["typeString"], ensure_ascii=False),
                max_range=repr(float(sensor["maximumRange"])),
                resolution=repr(float(sensor["resolution"])),
                power=repr(float(sensor["power"])),
                min_delay=min_delay,
                fifo_reserved=int(sensor["fifoReservedEventCount"]),
                fifo_max=int(sensor["fifoMaxEventCount"]),
                max_delay=max_delay,
                wake_up="true" if sensor["wakeUpSensor"] else "false",
                reporting_mode=int(sensor["reportingMode"])))
    lines.append("};")
    if dry_run:
        print(f"[..] (dry) 生成 {TARGETS_INC}（{len(doc['sensors'])} 个传感器）")
        return len(doc["sensors"])
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"    生成 {TARGETS_INC}（{len(doc['sensors'])} 个传感器）")
    return len(doc["sensors"])


def compile_one(tc, gen_hdr, work_dir, src, obj, dry_run):
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "-std=c++17", "-fPIC", "-fno-exceptions",
        "-fno-rtti", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter",
        "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
        "-I" + work_dir,
        "-nostdinc++", "-I" + tc["libcxx_config"],
        "-isystem", os.path.join(tc["sysroot"], "usr", "include", "c++", "v1"),
    ]
    argv.extend(platform_extras(tc))
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
    for dirpath, _dirs, files in os.walk(gen_src):
        for name in sorted(files):
            if name.endswith(".cpp"):
                cpps.append(os.path.join(dirpath, name))
    return sorted(cpps)


def link_service(tc, gen_hdr, work_dir, objs, crtbegin, crtend, runtime_lib_dir,
                 out_bin, dry_run):
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
    sensors_aidl_deps.prepare(quiet=dry_run)

    print("==> 传感器清单（config/sensors.json）")
    write_targets_inc(os.path.join(WORK_DIR, TARGETS_INC), CONFIG_PATH,
                      dry_run=dry_run)

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
                             VINTF_NAME),
                dry_run=dry_run)

    print("==> SELinux 标签登记")
    register_labels(baselines_file(), dry_run=dry_run)

    print("[ok] 自研 sensors HAL 构建完成")
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 自研 sensors HAL 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()
