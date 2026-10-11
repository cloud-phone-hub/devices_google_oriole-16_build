#!/usr/bin/env python3
"""note13-5G-15 camera HAL 虚拟 provider 构建（本机 NDK 工具链 + AIDL 快照）。

背景：lineage virtio 底座没有 camera HAL（vendor 无 camera 服务、vendor VINTF
/service_contexts 也无对应表项），CameraService 起不来（dumpsys camera 无设备）。
本模块自研 AIDL ICameraProvider 虚拟 provider：
  * 上报像素级摄像头清单（config/camera.json，移植自 pixel6-15 虚拟 HAL），
    供 CameraService 枚举 2 个虚拟摄像头（后摄 0 / 前摄 1）；
  * 虚拟 camera device 由 provider 进程动态按需注册（device@1.0/internal/0|1）；
  * 只依赖 libbinder_ndk / libc++ / liblog（AIDL NDK 后端默认），零 NDK 交叉负担。

源码布局（本模块内，自包含）：
  aidl/android15-release/  android-15.0.0_r36 AIDL 接口快照（7 包 aidl_api 形态），
    由本模块 deps.py 同步（构建产物，gitignored；缓存/官方源见 deps.py 文档）：
    camera.common@1 / camera.metadata@3 / camera.device@3 / camera.provider@3 /
    graphics.common@5 / common@2 / common.fmq@1（各带 .hash）
  config/camera.json      摄像头元数据固定契约（schema/device 已切到 note13-5G-15）
  src/cdh_note13_camera_provider_service.cpp    provider 实现（原 pixel6 移植改名）
  src/camera-metadata/     camera_metadata 头/源（构建产物，由 deps.py 同步：
                           AOSP system/media + liblog/libcutils + 自研 log_shim）

工具链（env 可覆盖；默认与本机实测一致）：
  CDH_AIDL / CDH_CLANG / CDH_LLD / CDH_NDK / CDH_BINDER / CDH_RUNTIME 同 sensors 模块；
  运行库由 deps.py（build/vendor/build/deps.py）从 note13 自身 lineage super 组装。

产物（相对路径即镜像内 /vendor 路径）：
  bin/hw/cdh-note13-camera-provider-service      (0755, hal_camera_default_exec)
  etc/init/cdh-note13-camera.rc                  (0644, vendor_configs_file)
  etc/vintf/manifest/android.hardware.camera-service.xml  (同 libs)

用法：
  python3 build/vendor/source/android.hardware.camera/build.py
  python3 build/vendor/source/android.hardware.camera/build.py --dry-run
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.camera/ 下，向上 5 级为设备根
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

# 本模块的 deps.py（AIDL 依赖）与 build/vendor/build/deps.py（运行库）同名，用绝对路径加载
import importlib.util  # noqa: E402

_camera_deps_spec = importlib.util.spec_from_file_location(
    "camera_aidl_deps", os.path.join(MODULE_DIR, "deps.py"))
camera_aidl_deps = importlib.util.module_from_spec(_camera_deps_spec)
_camera_deps_spec.loader.exec_module(camera_aidl_deps)

CONFIG_PATH = os.path.join(MODULE_DIR, "config", "camera.json")
AIDL_ROOT = os.path.join(MODULE_DIR, "aidl", "android15-release")
WORK_DIR = os.path.join(OUT_DIR, "tmp", "camera-ndk-work")

TARGET = "aarch64-linux-android35"
BINARY = "cdh-note13-camera-provider-service"
RC_NAME = "cdh-note13-camera.rc"
VINTF_NAME = "android.hardware.camera-service.xml"

# AIDL 包：<包名> -> (<展开相对路径(相对 AIDL_ROOT)>, <版本>)。
# 快照为 aidl_api 形态：<AIDL_ROOT>/<rel>/aidl/aidl_api/<包名>/<版本>/android/...
AIDL_PACKAGES = {
    "android.hardware.common": ("common/aidl/aidl_api/android.hardware.common", 2),
    "android.hardware.common.fmq": ("common/fmq/aidl/aidl_api/android.hardware.common.fmq", 1),
    "android.hardware.graphics.common": ("graphics/common/aidl/aidl_api/android.hardware.graphics.common", 5),
    "android.hardware.camera.common": ("camera/common/aidl/aidl_api/android.hardware.camera.common", 1),
    "android.hardware.camera.metadata": ("camera/metadata/aidl/aidl_api/android.hardware.camera.metadata", 3),
    "android.hardware.camera.device": ("camera/device/aidl/aidl_api/android.hardware.camera.device", 3),
    "android.hardware.camera.provider": ("camera/provider/aidl/aidl_api/android.hardware.camera.provider", 3),
}

# 运行时链接库（全部来自基座/lineage，只参与链接；运行期由设备解析）
RUNTIME_LIBS = ["libc++.so", "libbinder_ndk.so"]
EXTRA_LIBS = ["-llog", "-ldl", "-lm", "-lc"]

LABEL_BIN = "u:object_r:hal_camera_default_exec:s0"
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
        "binder_platform": os.path.join(COMMON_DIR, "ndk-binder-platform"),
        "bionic": os.path.join(COMMON_DIR, "bionic15"),
        "libcxx_config": os.path.join(COMMON_DIR, "libcxx-platform"),
        "platform_source": os.path.join(COMMON_DIR, "platform-headers"),
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
            "-isystem", os.path.join(ps, "system", "libbase", "include")]


def codegen(tc, gen_src, gen_hdr, dry_run):
    os.makedirs(gen_src, exist_ok=True)
    os.makedirs(gen_hdr, exist_ok=True)
    snapshots = []  # 每个 aidl_api 快照根（-I 用），含 android/...
    for pkg, (rel, version) in AIDL_PACKAGES.items():
        snap_root = os.path.join(AIDL_ROOT, rel, str(version))
        if not os.path.isdir(snap_root):
            print(f"[err] AIDL 快照缺失: {snap_root}", file=sys.stderr)
            sys.exit(1)
        snapshots.append((pkg, version, snap_root))
    for pkg, version, snap_root in snapshots:
        # 该包自己的 .aidl（去掉外层 aidl_api/<pkg>/<ver> 前缀，位于 android/... 下）
        inputs = []
        for dirpath, _dirs, files in os.walk(snap_root):
            for name in sorted(files):
                if name.endswith(".aidl"):
                    inputs.append(os.path.join(dirpath, name))
        if not inputs:
            print(f"[err] AIDL 包为空: {pkg} ({snap_root})", file=sys.stderr)
            sys.exit(1)
        hash_path = os.path.join(snap_root, ".hash")
        if not os.path.isfile(hash_path):
            print(f"[err] 缺 AIDL 包 hash: {hash_path}", file=sys.stderr)
            sys.exit(1)
        with open(hash_path, encoding="ascii") as f:
            snap_hash = f.read().strip()
        # -I 全部快照根：包间 import（provider→device/metadata/common/graphics…）可解析
        argv = [tc["aidl"], "--lang=ndk", "--structured", "--stability=vintf",
                "--min_sdk_version=35", f"--version={version}",
                f"--hash={snap_hash}"]
        for _, _, root in snapshots:
            argv += ["-I", root]
        argv += ["--out=" + gen_src, "--header_out=" + gen_hdr]
        argv.extend(sorted(inputs))
        run(argv, dry_run=dry_run)
        print(f"    {pkg}@{version}: {len(inputs)} aidl (hash {snap_hash[:12]})")


def list_aidl_pkg(snap_root):
    out = []
    for root, _dirs, files in os.walk(snap_root):
        for f in sorted(files):
            if f.endswith(".aidl"):
                out.append(os.path.join(root, f))
    return out


def compile_one(tc, gen_hdr, work_dir, src, obj, dry_run):
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "-std=c++17", "-fPIC", "-fno-exceptions",
        "-fno-rtti", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter",
        "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
        "-I" + work_dir, "-I" + os.path.join(SRC_DIR, "camera-metadata"),
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


def compile_c(tc, src, obj, hidden_visibility, dry_run):
    """编译 C 源（camera_metadata.c / log_shim，-std=c11 非 C++17）。

    hidden_visibility 参考 Go 原版：camera_metadata.c 带 -fvisibility=hidden
    （隐藏内部符号，避免污染动态符号表），log_shim 不带。
    """
    argv = [
        tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-fPIE",
        "-I" + os.path.join(SRC_DIR, "camera-metadata"),
    ]
    if hidden_visibility:
        argv += ["-fvisibility=hidden"]
    argv.extend(["-c", src, "-o", obj])
    if dry_run:
        print("[..] (dry)", " ".join(argv))
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
        "-I" + work_dir, "-I" + os.path.join(SRC_DIR, "camera-metadata"),
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


def write_targets_summary(config_path, dry_run=False):
    """打印 config/camera.json 的目标合约摘要（移植自 pixel6 校验逻辑的宽容版）。"""
    with open(config_path, encoding="utf-8") as f:
        doc = json.load(f)
    cam = doc.get("targets", {})
    print(f"    schema={doc.get('schema')} device={doc.get('device')} "
          f"android={doc.get('androidMajor')} aosp={doc.get('aospTag')}")
    print(f"    provider={doc.get('providerAidlVersion')} "
          f"device={doc.get('deviceAidlVersion')} "
          f"common={doc.get('commonAidlVersion')} "
          f"metadata={doc.get('metadataAidlVersion')} "
          f"graphics={doc.get('graphicsCommonAidlVersion')}")
    print(f"    cameras={len(doc.get('cameras', []))} "
          f"target={cam.get('cameraCount')} "
          f"legacy={cam.get('legacyCameraCount')} "
          f"devices={cam.get('aidlDeviceNames')}")
    return len(doc.get("cameras", []))


def build(dry_run=False):
    tc = toolchain(dry_run=dry_run)
    gen_src = os.path.join(WORK_DIR, "gen", "src")
    gen_hdr = os.path.join(WORK_DIR, "gen", "hdr")
    obj_dir = os.path.join(WORK_DIR, "obj")
    os.makedirs(WORK_DIR, exist_ok=True)
    os.makedirs(obj_dir, exist_ok=True)

    print("==> 目标契约（config/camera.json）")
    write_targets_summary(CONFIG_PATH)

    print("==> AIDL 官方依赖同步")
    camera_aidl_deps.prepare(quiet=dry_run)

    print("==> camera_metadata 官方依赖同步")
    camera_aidl_deps.prepare_camera_meta(quiet=dry_run)

    print("==> AIDL 代码生成（7 包 snapshots）")
    if not dry_run:
        for d in (gen_src, gen_hdr):
            if os.path.isdir(d):
                for root, _dirs, files in os.walk(d):
                    for f in files:
                        os.unlink(os.path.join(root, f))
    codegen(tc, gen_src, gen_hdr, dry_run)

    gen_cpps = collect_gen_cpps(gen_src)
    src_cpps = [f for f in sorted(os.listdir(SRC_DIR)) if f.endswith(".cpp")]
    print(f"==> 编译 {len(gen_cpps)}+{len(src_cpps)} 个 C++ 源 + camera_metadata C")

    all_srcs = gen_cpps + [os.path.join(SRC_DIR, f) for f in src_cpps]
    objs = []
    for i, src in enumerate(all_srcs):
        obj = os.path.join(obj_dir, f"o{i}.o")
        compile_one(tc, gen_hdr, WORK_DIR, src, obj, dry_run)
        objs.append(obj)

    metadir = os.path.join(SRC_DIR, "camera-metadata")
    metadata_src = os.path.join(metadir, "camera_metadata.c")
    metadata_obj = os.path.join(obj_dir, "camera_metadata.o")
    compile_c(tc, metadata_src, metadata_obj, True, dry_run)
    objs.append(metadata_obj)
    log_shim_src = os.path.join(metadir, "camera_metadata_log_shim.c")
    log_shim_obj = os.path.join(obj_dir, "camera_metadata_log_shim.o")
    compile_c(tc, log_shim_src, log_shim_obj, False, dry_run)
    objs.append(log_shim_obj)

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

    print("[ok] 自研 camera HAL 构建完成")
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 自研 camera HAL 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()