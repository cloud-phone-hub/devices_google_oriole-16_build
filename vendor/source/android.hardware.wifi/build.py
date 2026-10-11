#!/usr/bin/env python3
"""note13-5G-15 wifi HAL 自研服务构建（本机 NDK 工具链 + 官方 wifi AIDL 依赖）。

背景：lineage virtio 底座有 wificond 与 supplicant/hostapd HAL，但没有 IWifi HAL
（/vendor/bin/hw 无 wifi 服务、vendor VINTF 未声明 android.hardware.wifi.IWifi），
WifiService 拿不到 chip/iface，Wi-Fi 不可用。本模块按 pixel6-15 vendor/wifi 的架构移植：
自研 AIDL 服务实现 android.hardware.wifi.IWifi（v2，含 IWifiChip/IWifiStaIface），
扫描结果与连接 BSSID 由内核模块 mac80211_hwsim（v8 改造）的 scan_profile 驱动
（假扫描注入 + 假连接伪响应；nl80211 仅用于查询连接态），不驱动真实射频。

源码布局（本模块内，自包含）：
  aidl/aosp/android/hardware/wifi/**         官方 wifi AIDL 展开快照（android.hardware.wifi@2）
  aidl/aosp/android/hardware/wifi/common/**  同上的 public 子包（android.hardware.wifi.common@1）
                                             两者共用 -I 根，由本模块 deps.py 同步；
  config/wifi.json       服务/接口契约（wlan0 + backing eth0）；
  include/               cdh_wifi_scan_model.h（scan_profile 编解码，随 src 相对包含）；
  src/                   业务实现（IWifi/IWifiChip/IWifiStaIface）；
  probe/                 virt_wifi_probe.cpp（独立诊断工具，不参与本服务编译，同 pixel6）。

平台 parcelable：wifi AIDL 引用 android.os.PersistableBundle，编译时用
-p framework-ndk.aidl 指向本模块生成的预编译声明（cpp_header/ndk_header 指向 binder 侧头）。

工具链（env 可覆盖；默认与本机实测一致）：同 radio/sensors/thermal/vibrator/lights/gnss
模块（NDK r29 + 本机 aidl）。

产物（相对路径即镜像内 /vendor 路径）：
  bin/hw/android.hardware.wifi-service.cdh-note13   (0755, hal_wifi_default_exec)
  etc/init/android.hardware.wifi-service.cdh-note13.rc
  etc/vintf/manifest/android.hardware.wifi-service.xml

SELinux：二进制/rc/vintf 标签经 baselines 登记；hal_wifi_default 域的 service add/find、
netlink/udp/capability 授权与 scan_profile sysfs 标签（cdh_wifi_scan_param）由
build/vendor/build/sepolicy.py wifi_hal_patch_commands 追加。

用法：
  python3 build/vendor/source/android.hardware.wifi/build.py
  python3 build/vendor/source/android.hardware.wifi/build.py --dry-run
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.wifi/ 下，向上 5 级为设备根
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
CONFIG_PATH = os.path.join(MODULE_DIR, "config", "wifi.json")
WORK_DIR = os.path.join(OUT_DIR, "tmp", "wifi-ndk-work")

# 本模块的 deps.py（AIDL 依赖）与 build/vendor/build/deps.py（运行库）同名，用绝对路径加载
import importlib.util  # noqa: E402
_spec = importlib.util.spec_from_file_location(
    "wifi_aidl_deps", os.path.join(MODULE_DIR, "deps.py"))
wifi_aidl_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(wifi_aidl_deps)

# 目标
TARGET = "aarch64-linux-android35"
BINARY = "android.hardware.wifi-service.cdh-note13"
RC_NAME = "android.hardware.wifi-service.cdh-note13.rc"
VINTF_NAME = "android.hardware.wifi-service.xml"
FRAMEWORK_AIDL = "framework-ndk.aidl"
TARGETS_INC = "wifi_target.inc"
MIN_SDK_VERSION = 35

# 平台 parcelable 预编译声明（与 pixel6 同内容）：wifi AIDL 里用到 android.os.PersistableBundle
FRAMEWORK_AIDL_BODY = (
    "@JavaOnlyStableParcelable @NdkOnlyStableParcelable parcelable"
    " android.os.PersistableBundle cpp_header \"binder/PersistableBundle.h\""
    " ndk_header \"android/persistable_bundle_aidl.h\";\n"
)

# AIDL 包：<展开相对路径> -> <版本>（.hash 随包，传给 aidl --hash）
AIDL_PACKAGES = {
    "android/hardware/wifi/common": 1,
    "android/hardware/wifi": 2,
}

# 运行时链接库（全部来自基座/lineage，只参与链接；运行期由设备解析）
RUNTIME_LIBS = ["libc++.so", "libbinder_ndk.so"]
EXTRA_LIBS = ["-llog", "-ldl", "-lm", "-lc"]

LABEL_BIN = "u:object_r:hal_wifi_default_exec:s0"
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


def read_config(config_path):
    """读 config/wifi.json 并校验服务契约（与 pixel6 validateTarget 等价）。"""
    with open(config_path, encoding="utf-8") as f:
        doc = json.load(f)
    if not str(doc.get("schema", "")).endswith(".wifi.v1"):
        print(f"[err] wifi 目标契约错误: {doc.get('schema')}", file=sys.stderr)
        sys.exit(1)
    if int(doc.get("aidlVersion", 0)) != 2 or \
            int(doc.get("commonAidlVersion", 0)) != 1:
        print(f"[err] wifi AIDL 版本错误: wifi={doc.get('aidlVersion')} "
              f"common={doc.get('commonAidlVersion')}", file=sys.stderr)
        sys.exit(1)
    if doc.get("serviceName") != "android.hardware.wifi.IWifi/default" or \
            not doc.get("interfaceName") or not doc.get("defaultEnabled"):
        print("[err] wifi service 契约错误", file=sys.stderr)
        sys.exit(1)
    return doc


def write_wifi_targets(path, config_path, dry_run=False):
    """由 config/wifi.json 生成 wifi_target.inc，提供源文件 include 的编译期常量：

      kHostChannelPath        host wifi 服务通道（/dev/hvc9，QEMU unix socket chardev）
      kScanProfilePath        内核模块 mac80211_hwsim 的扫描档位参数
      kHostFrameMax           单帧上限（与 hvc7 castevent 一致 1 MiB）
      kHostReconnectDelayMs   通道断线重连间隔
      kHostReadTimeoutMs      读超时（超时发 ping 保活）
      kHostProtocolVersion    通道协议版本（hello 帧上报）
    """
    with open(config_path, encoding="utf-8") as f:
        doc = json.load(f)
    lines = [
        "static constexpr const char *kHostChannelPath = " +
        json.dumps(str(doc["hostChannel"])) + ";",
        "static constexpr const char *kScanProfilePath = " +
        json.dumps(str(doc["scanProfilePath"])) + ";",
        "static constexpr int32_t kHostFrameMax = " +
        str(int(doc.get("hostFrameMax", 1 << 20))) + ";",
        "static constexpr int32_t kHostReconnectDelayMs = " +
        str(int(doc.get("hostReconnectDelayMs", 1000))) + ";",
        "static constexpr int32_t kHostReadTimeoutMs = " +
        str(int(doc.get("hostReadTimeoutMs", 30000))) + ";",
        "static constexpr int32_t kHostProtocolVersion = " +
        str(int(doc.get("hostProtocolVersion", 1))) + ";",
        "",
    ]
    if dry_run:
        print(f"[..] (dry) 生成 {TARGETS_INC}（通道 {doc['hostChannel']}）")
        return
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"// Generated from {os.path.relpath(config_path, _DEVICE_ROOT)}."
                " Do not edit by hand.\n" + "\n".join(lines))
    print(f"    生成 {TARGETS_INC}（通道 {doc['hostChannel']}，"
          f"档位 {doc['scanProfilePath']}）")


def write_framework_aidl(path, dry_run=False):
    """写平台 parcelable 预编译声明，供 aidl 以 -p 引用。"""
    if dry_run:
        print(f"[..] (dry) 写 {FRAMEWORK_AIDL}（android.os.PersistableBundle）")
        return
    with open(path, "w", encoding="utf-8") as f:
        f.write(FRAMEWORK_AIDL_BODY)
    print(f"    写 {FRAMEWORK_AIDL}（android.os.PersistableBundle）")


def codegen(tc, gen_src, gen_hdr, framework_aidl, dry_run):
    """对每个包跑 aidl：同一 -I 根（两包互为 import），-p 引用平台 parcelable。"""
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
                f"--min_sdk_version={MIN_SDK_VERSION}",
                f"--version={version}", f"--hash={snap_hash}",
                "-p" + framework_aidl,
                "-I", AIDL_ROOT, "--out=" + gen_src, "--header_out=" + gen_hdr]
        argv.extend(inputs)
        run(argv, dry_run=dry_run)
        print(f"    {rel}: {len(inputs)} aidl (v{version}, hash {snap_hash[:12]})")


def compile_one(tc, gen_hdr, work_dir, src, obj, dry_run):
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "-std=c++17", "-fPIC",
        "-fno-rtti", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter",
        "-DCDH_WIFI_AIDL_VERSION=2",
        "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
        "-I" + work_dir, "-I" + MODULE_DIR,
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
                 out_bin, dry_run):
    builtins = os.path.join(tc["clang_rt_dir"], "lib", "linux",
                            "libclang_rt.builtins-aarch64-android.a")
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "--ld-path=" + tc["lld"], "-nostdlib", "-fno-stack-protector",
        "-mno-outline-atomics", "-mbranch-protection=standard",
        "-Wl,--dynamic-linker,/system/bin/linker64", "-Wl,-z,relro",
        "-Wl,-z,now", "-std=c++17", "-fPIE", "-pie",
        "-DCDH_WIFI_AIDL_VERSION=2",
        "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
        "-I" + work_dir, "-I" + MODULE_DIR,
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

    cfg = read_config(CONFIG_PATH)
    print(f"==> 服务契约: {cfg['serviceName']}（{cfg['interfaceName']}，"
          f"backing {cfg['backingInterface']}）")

    print("==> 宿主通道常量（config/wifi.json）")
    write_wifi_targets(os.path.join(WORK_DIR, TARGETS_INC), CONFIG_PATH,
                       dry_run=dry_run)

    print("==> AIDL 官方依赖同步")
    wifi_aidl_deps.prepare(quiet=dry_run)

    print("==> 平台 parcelable（framework-ndk.aidl）")
    framework_aidl = os.path.join(WORK_DIR, FRAMEWORK_AIDL)
    write_framework_aidl(framework_aidl, dry_run=dry_run)

    print("==> AIDL 代码生成")
    if not dry_run:
        for d in (gen_src, gen_hdr):
            if os.path.isdir(d):
                for root, _dirs, files in os.walk(d):
                    for name in files:
                        os.unlink(os.path.join(root, name))
    codegen(tc, gen_src, gen_hdr, framework_aidl, dry_run)

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

    print("[ok] 自研 wifi HAL 构建完成")
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 自研 wifi HAL 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()
