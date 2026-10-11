#!/usr/bin/env python3
"""android.hardware.media.v412 —— note13-5G-15 V4L2 硬件编解码 Codec2 store 构建。

移植自 pixel6-15 vendor/codec2_v4l2（ChromeOS v4l2_codec2 真实硬件编解码 HAL，
服务注册 android.hardware.media.c2.IComponentStore/default——注意与
android.hardware.media 模块的 MTK 假登记 store 存在同名实例占用问题，见 README）。

三段式（对齐 pixel6 build/main.go）：
  1. 编译全部业务源（src/v4l2_codec2/{common,components,v4l2,plugin_store,service}）
     到 out/tmp/media-v412-work/obj；
  2a. 链接 plugin 共享库 libc2plugin_store.so（-lcodec2_vndk）；
  2b. 编译 libyuv 源并以 llvm-ar 归档 libyuv_cdh.a（platform-source/external/libyuv）；
  3. 链接 PIE 服务 android.hardware.media.c2@1.2-service-v4l2-64，复制一份为
     android.hardware.media.c2-service-v4l2-64。

构建期依赖（已在 deps.py 维护，带缓存，命中离线复用）：
  - aidl/aosp：官方 AIDL 展开源（编译 -I 根，随库固化，5 包含 .hash）
  - AOSP platform 源 15 仓 git 拉取（tag android-15.0.0_r36，--depth=1 --filter=blob:none）
    -> out/tmp/media-v412-work/platform-source（.platform-complete 标记缓存）
  - hidl-gen 工具下载（prebuilts/build-tools，sha256 校验）-> hidl-tool/
  - 20 个 HIDL 包头生成（期望 219 个）-> hidl-gen/
  - ABI 库：deps.py 从 lineage super 研磨（system: libhidlbase/libstagefright_* 等；
    apex: media-swcodec 的 libcodec2_*）-> out/dist/vendor-compile-dep/media-v412
  运行时再生成 AIDL NDK 绑定（5 个 version 化包 common/fmq/graphics.common/bufferpool2/c2
  + frameworks 稳定类型 Surface/HardwareBuffer/LayerMetadataKey 快照）。

产物落 VENDOR_STAGE_DIR（相对路径即镜像内 /vendor）：
  bin/hw/android.hardware.media.c2@1.2-service-v4l2-64（+ legacy 更名副本）
  lib64/libc2plugin_store.so
  lib64/<运行期依赖库>（deps.py VENDOR_LIBS：非 LLNDK 的 system 库 + APEX 私有库，
    vendor 进程 linker namespace 看不到它们，必须随模块落盘）
  etc/init/android.hardware.media.c2@1.2-service-v4l2-64.rc
  etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy
  media_codecs_c2.xml（**合并**：与 android.hardware.media 同路径，追加 c2.v4l2.* 组，
  保留 MTK 全集，绝不覆盖）
  etc/（vintf manifest 由 android.hardware.media 声明 IComponentStore/default；本模块
  同类服务不再重复注入，避免 vintf 重复声明冲突）

用法：
  python3 build/vendor/source/android.hardware.media.v412/build.py
  python3 build/vendor/source/android.hardware.media.v412/build.py --dry-run
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.media.v412/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import VENDOR_STAGE_DIR  # noqa: E402

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))

import importlib.util  # noqa: E402
_spec = importlib.util.spec_from_file_location(
    "media_v412_deps", os.path.join(MODULE_DIR, "deps.py"))
media_v412_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(media_v412_deps)

AIDL_ROOT = os.path.join(MODULE_DIR, "aidl", "aosp")
WORK_DIR = media_v412_deps.WORK_DIR

TARGET = "aarch64-linux-android35"
MIN_SDK = 35

# ---- 产物名（rc 引用）----
SERVICE_BINARY = "android.hardware.media.c2@1.2-service-v4l2-64"
SERVICE_BINARY_LEGACY = "android.hardware.media.c2-service-v4l2-64"
PLUGIN_SONAME = "libc2plugin_store.so"
LIBYUV_ARCHIVE = "libyuv_cdh.a"

# ---- SELinux 标签（bin 走 codec2 exec 类型；rc/xml/vintf 走 vendor_configs_file）----
LABEL_BIN = "u:object_r:hal_codec2_default_exec:s0"
LABEL_CONFIG = "u:object_r:vendor_configs_file:s0"
LABEL_LIB = "u:object_r:same_process_hal_file:s0"
LABELS = {
    f"bin/hw/{SERVICE_BINARY}": LABEL_BIN,
    f"bin/hw/{SERVICE_BINARY_LEGACY}": LABEL_BIN,
    f"lib64/{PLUGIN_SONAME}": LABEL_LIB,
    "etc/init/android.hardware.media.c2@1.2-service-v4l2-64.rc": LABEL_CONFIG,
    "etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy": LABEL_CONFIG,
    "etc/vintf/manifest/android.hardware.media.c2@1.2-service-v4l2.xml": LABEL_CONFIG,
    "etc/media_codecs_c2.xml": LABEL_CONFIG,
}
# 需随模块落盘到 /vendor/lib64 的库（vendor 进程 linker namespace 看不到非 LLNDK 的
# system 库，见 deps.py VENDOR_LIBS 的判定依据）
for _lib in media_v412_deps.VENDOR_LIBS:
    LABELS[f"lib64/{_lib}"] = LABEL_LIB
BASELINES = sorted(glob.glob(os.path.join(_DEVICE_ROOT, "build", "baselines",
                                          "gold_images_*.json")))

# ---- AIDL 包：<快照相对路径> -> 版本 ----
AIDL_PACKAGES = [
    ("common/aidl/aidl_api/android.hardware.common", "2"),
    ("common/fmq/aidl/aidl_api/android.hardware.common.fmq", "1"),
    ("graphics/common/aidl/aidl_api/android.hardware.graphics.common", "5"),
    ("media/bufferpool/aidl/aidl_api/android.hardware.media.bufferpool2", "2"),
    ("media/c2/aidl/aidl_api/android.hardware.media.c2", "1"),
]
FRAMEWORKS_TYPES = [
    "frameworks/base/core/java",
    "frameworks/native/aidl/gui",
]

# ---- 业务源（对齐 pixel6 main.go businessSources）----
BUSINESS_SOURCES = [
    "common/Common.cpp", "common/EncodeHelpers.cpp", "common/FormatConverter.cpp",
    "common/Fourcc.cpp", "common/H264.cpp", "common/H264NalParser.cpp",
    "common/HEVCNalParser.cpp", "common/NalParser.cpp", "common/VideoPixelFormat.cpp",
    "common/VideoTypes.cpp",
    "components/VideoFrame.cpp", "components/VideoFramePool.cpp",
    "components/ComponentStore.cpp", "components/DecodeComponent.cpp",
    "components/DecodeInterface.cpp", "components/VideoDecoder.cpp",
    "components/EncodeComponent.cpp", "components/EncodeInterface.cpp",
    "components/VideoEncoder.cpp",
    "v4l2/V4L2ComponentCommon.cpp", "v4l2/V4L2ComponentFactory.cpp",
    "v4l2/V4L2ComponentStore.cpp", "v4l2/V4L2Decoder.cpp", "v4l2/V4L2DecodeMemory.cpp",
    "v4l2/V4L2DecodeComponent.cpp", "v4l2/V4L2Device.cpp", "v4l2/V4L2DevicePoller.cpp",
    "v4l2/V4L2Encoder.cpp", "v4l2/V4L2EncodeComponent.cpp", "v4l2/V4L2Memory.cpp",
]
# plugin 单独链接共享库（避免重复出现，从 business 中去重）
PLUGIN_SOURCES = [
    "plugin_store/DmabufHelpers.cpp", "plugin_store/V4L2PluginStore.cpp",
    "plugin_store/VendorAllocatorLoader.cpp",
]
SERVICE_SOURCE = "service/service.cpp"

# libyuv 静态库源（platform-source/external/libyuv）
LIBYUV_SOURCES = [
    "compare.cc", "compare_common.cc", "compare_gcc.cc", "compare_msa.cc",
    "compare_neon.cc", "compare_neon64.cc", "convert.cc", "convert_argb.cc",
    "convert_from.cc", "convert_from_argb.cc", "convert_to_argb.cc",
    "convert_to_i420.cc", "cpu_id.cc", "planar_functions.cc",
    "rotate.cc", "rotate_any.cc", "rotate_argb.cc", "rotate_common.cc",
    "rotate_gcc.cc", "rotate_msa.cc", "rotate_neon.cc", "rotate_neon64.cc",
    "row_any.cc", "row_common.cc", "row_gcc.cc", "row_msa.cc", "row_neon.cc",
    "row_neon64.cc", "row_rvv.cc",
    "scale.cc", "scale_any.cc", "scale_argb.cc", "scale_common.cc",
    "scale_gcc.cc", "scale_msa.cc", "scale_neon.cc", "scale_neon64.cc",
    "scale_rgb.cc", "scale_rvv.cc", "scale_uv.cc", "video_common.cc",
]

# ---- 构建期三方依赖（platform 源 / hidl-gen / HIDL 包 / aidl/aosp）
# 已在 deps.py 维护（git 拉取/下载/生成，全部走代码库），build.py 仅调用 prepare_platform()。

# ---- include 目录（对齐 pixel6 includeDirs）----
def include_dirs(src, aosp, libyuv, gen_aidl, gen_hidl):
    dirs = []
    for rel in (
        "v4l2/include", "components/include", "common/include",
        "plugin_store/include",
    ):
        dirs.append("-I" + os.path.join(src, rel))
    for rel in (
        "frameworks/av/media/codec2/core/include",
        "frameworks/av/media/codec2/vndk/include",
        "frameworks/av/media/codec2/vndk/internal",
        "frameworks/av/media/codec2/hal/common/include",
        "frameworks/av/media/codec2/hal/aidl/include",
        "frameworks/av/media/codec2/components/base/include",
        "frameworks/av/media/codec2/sfplugin/utils",
        "frameworks/av/media/module/foundation/include",
        "frameworks/av/media/libstagefright/include",
        "frameworks/av/media/module/minijail",
        "frameworks/av/media/module/bufferpool/2.0/include",
        "frameworks/av/media/codec2/hal/hidl/1.0/utils/include",
        "frameworks/av/media/codec2/hal/hidl/1.1/utils/include",
        "frameworks/av/media/codec2/hal/hidl/1.2/utils/include",
        "frameworks/native/libs/binder/include",
        "frameworks/native/libs/binder/ndk/include_ndk",
        "frameworks/native/libs/binder/ndk/include_cpp",
        "frameworks/native/libs/binder/ndk/include_platform",
        "frameworks/native/libs/ui/include",
        "frameworks/native/libs/nativewindow/include",
        "frameworks/native/libs/nativebase/include",
        "frameworks/native/libs/math/include",
        "frameworks/native/libs/arect/include",
        "frameworks/native/headers/media_plugin",
        "system/libbase/include",
        "system/core/libcutils/include",
        "system/core/libutils/include",
        "system/core/libsystem/include",
        "system/logging/liblog/include",
        "system/libhidl/base/include",
        "system/libhidl/transport/include",
        "system/libfmq/include",
        "system/libfmq/base",
        "system/libhwbinder/include",
        "hardware/libhardware/include",
        "external/libchrome",
        "external/googletest/googletest/include",
    ):
        dirs.append("-I" + os.path.join(aosp, rel))
    dirs.append("-I" + os.path.join(libyuv, "include"))
    dirs.extend(["-I" + gen_aidl, "-I" + gen_hidl])
    return dirs


# ---- 工具链 ----
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
        "ar": env("CDH_LLVM_AR", os.path.join(prebuilt, "bin", "llvm-ar")),
        "sysroot": os.path.join(prebuilt, "sysroot"),
        "clang_rt_dir": ndk_clang_rt_dir(prebuilt),
        "binder": env("CDH_BINDER",
                      os.path.expanduser(
                          "~/Library/Android/sdk/platforms/android-36/"
                          "optional/libbinder_ndk_cpp")),
        "binder_platform": os.path.join(
            os.path.dirname(MODULE_DIR), "common", "ndk-binder-platform"),
        "bionic": os.path.join(
            os.path.dirname(MODULE_DIR), "common", "bionic15"),
        "libcxx_config": os.path.join(
            os.path.dirname(MODULE_DIR), "common", "libcxx-platform"),
    }


def run(argv, dry_run=False):
    if dry_run:
        print("[..] (dry) " + " ".join(argv))
        return 0
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout, file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        sys.exit(1)
    return 0


def codegen_aidl(tc, gen_src, gen_hdr, dry_run):
    os.makedirs(gen_src, exist_ok=True)
    os.makedirs(gen_hdr, exist_ok=True)
    # -I 根：5 个包目录 + frameworks 类型目录
    snap_dirs = []
    for rel, version in AIDL_PACKAGES:
        snap_dirs.append(os.path.join(AIDL_ROOT, rel, version))
    fw_dirs = [os.path.join(AIDL_ROOT, rel) for rel in FRAMEWORKS_TYPES]
    inputs = []
    for d in snap_dirs:
        for dirpath, _dirs, files in os.walk(d):
            for name in sorted(files):
                if name.endswith(".aidl"):
                    inputs.append(os.path.join(dirpath, name))
    for i, (rel, version) in enumerate(AIDL_PACKAGES):
        base = snap_dirs[i]
        hash_path = os.path.join(base, ".hash")
        if not os.path.isfile(hash_path):
            print(f"[err] 缺 AIDL 包 hash: {hash_path}", file=sys.stderr)
            sys.exit(1)
        with open(hash_path, encoding="ascii") as f:
            snap_hash = f.read().split()[-1]
        # 该包自己的输入
        pkg_inputs = []
        for dirpath, _dirs, files in os.walk(base):
            for name in sorted(files):
                if name.endswith(".aidl"):
                    pkg_inputs.append(os.path.join(dirpath, name))
        argv = [tc["aidl"], "--lang=ndk", "--structured", "--stability=vintf",
                f"--min_sdk_version={MIN_SDK}",
                f"--version={version}", f"--hash={snap_hash}",
                "-I", AIDL_ROOT] + [f"-I{d}" for d in snap_dirs + fw_dirs] \
                + ["--out=" + gen_src, "--header_out=" + gen_hdr] + pkg_inputs
        run(argv, dry_run=dry_run)
        print(f"    {rel}: {len(pkg_inputs)} aidl (v{version}, hash {snap_hash[:12]})")
    return inputs


def collect_cpps(root):
    out = []
    for dirpath, _dirs, files in os.walk(root):
        for name in sorted(files):
            if name.endswith(".cpp"):
                out.append(os.path.join(dirpath, name))
    return sorted(out)


def libcxx_args(tc):
    headers = os.path.join(tc["sysroot"], "usr", "include", "c++", "v1")
    return ["-nostdinc++", "-I" + tc["libcxx_config"], "-isystem", headers]


def merge_media_codecs_c2(stage_etc):
    """media_codecs_c2.xml 是与 android.hardware.media 同路径文件：合并 c2.v4l2.*

    android.hardware.media 注入的是真机 MTK 全集；本模块追加 c2.v4l2.* 组件定义，
    **保留 MTK 条目**（覆盖会摧毁已对齐的 MediaCodecList）。目标不存在时直接写入。
    """
    def v4l2_fragment():
        return (
            '    <Decoders>\n'
            '        <MediaCodec name="c2.v4l2.avc.decoder" type="video/avc">\n'
            '            <Feature name="adaptive-playback" />\n'
            '        </MediaCodec>\n'
            '        <MediaCodec name="c2.v4l2.hevc.decoder" type="video/hevc">\n'
            '            <Feature name="adaptive-playback" />\n'
            '        </MediaCodec>\n'
            '    </Decoders>\n'
            '    <Encoders>\n'
            '        <MediaCodec name="c2.v4l2.avc.encoder" type="video/avc">\n'
            '            <Limit name="size" min="32x32" max="1920x1088" />\n'
            '            <Limit name="alignment" value="2x2" />\n'
            '            <Limit name="block-size" value="16x16" />\n'
            '            <Limit name="bitrate" range="1-40000000" />\n'
            '            <Feature name="bitrate-modes" value="VBR,CBR" />\n'
            '        </MediaCodec>\n'
            '    </Encoders>\n'
        )

    src = os.path.join(MODULE_DIR, "vendor", "etc", "media_codecs_c2.xml")
    dst = os.path.join(stage_etc, "media_codecs_c2.xml")
    with open(src, encoding="utf-8") as f:
        v4l2_xml = f.read()
    if os.path.isfile(dst):
        with open(dst, encoding="utf-8") as f:
            existing = f.read()
        if "c2.v4l2.avc.decoder" in existing:
            print("    media_codecs_c2.xml 已含 c2.v4l2.*，跳过合并")
            return
        if "<Encoders>" in existing and "<Decoders>" in existing:
            merged = existing.replace(
                "    <Encoders>", v4l2_fragment() + "    <Encoders>", 1)
        else:
            merged = existing.rstrip() + "\n" + v4l2_xml + "\n"
        with open(dst, "w", encoding="utf-8") as f:
            f.write(merged)
        print("    media_codecs_c2.xml 合并 c2.v4l2.*（保留 MTK 全集）")
    else:
        with open(dst, "w", encoding="utf-8") as f:
            f.write(v4l2_xml)
        print("    media_codecs_c2.xml 写入 v4l2 定义（目标不存在）")


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


def register_labels(dry_run=False):
    """登记新增文件的 SELinux 标签进 baselines vendor.selinux-labels（幂等）。

    vendor 阶段对注入树每个文件都要求 baselines 有标签（缺失即报错退出），
    本模块新增文件必须在此登记（同 android.hardware.media 的机制）。
    """
    if not BASELINES:
        print(f"[err] baselines 清单缺失: {_DEVICE_ROOT}/build/baselines/", file=sys.stderr)
        sys.exit(1)
    baselines_path = BASELINES[0]
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


# ---- 主构建 ----
def build(dry_run=False):
    tc = toolchain(dry_run=dry_run)
    abi = media_v412_deps.prepare_abi(dry_run=dry_run)
    abi_sys = os.path.join(abi, "system", "lib64")
    abi_apex = os.path.join(abi, "apex", "media-swcodec", "lib64")

    src = os.path.join(MODULE_DIR, "src", "v4l2_codec2")
    platform_root = os.path.join(WORK_DIR, "platform-source")
    libyuv = os.path.join(platform_root, "external", "libyuv")
    gen_aidl = os.path.join(WORK_DIR, "aidl-gen", "include")
    gen_aidl_src = os.path.join(WORK_DIR, "aidl-gen", "src")
    gen_hidl = os.path.join(WORK_DIR, "hidl-gen")
    obj_dir = os.path.join(WORK_DIR, "obj")
    lib_out = os.path.join(WORK_DIR, "lib")
    bin_out = os.path.join(WORK_DIR, "bin")
    libyuv_obj = os.path.join(WORK_DIR, "libyuv-obj")

    os.makedirs(WORK_DIR, exist_ok=True)

    print("==> 构建期依赖（platform-source / hidl-gen / HIDL 头，见 deps.py）")
    media_v412_deps.prepare_platform(dry_run=dry_run)

    print("==> AIDL 代码生成")
    if not dry_run:
        for d in (gen_aidl_src, gen_aidl):
            if os.path.isdir(d):
                shutil.rmtree(d)
    codegen_aidl(tc, gen_aidl_src, gen_aidl, dry_run)
    gen_cpps = collect_cpps(gen_aidl_src)

    print("==> 编译业务源")
    for d in (obj_dir, lib_out, bin_out):
        os.makedirs(d, exist_ok=True)
    inc = include_dirs(src, platform_root, libyuv, gen_aidl, gen_hidl)
    cxx = libcxx_args(tc)
    flat_flags = cxx + [
        "-std=c++17", "-fPIC", "-O2", "-fno-exceptions", "-fno-rtti",
        "-include", "cmath", "-D_LIBCPP_DISABLE_AVAILABILITY", "-DNDEBUG",
        "-DV4L2_CODEC2_SERVICE_V4L2_STORE", "-DV4L2_CODEC2_ENABLE_ENCODERS=1",
        "-DV4L2_CODEC2_ENABLE_VP8_DECODER=0", "-DV4L2_CODEC2_ENABLE_VP9_DECODER=0",
        "-DV4L2_CODEC2_ENABLE_SECURE_DECODERS=0",
        "-Wno-invalid-specialization", "-Wno-unused-parameter",
        "-Wno-unused-variable", "-Wno-deprecated-declarations",
        "-Wno-inconsistent-missing-override",
    ]
    all_business = BUSINESS_SOURCES + PLUGIN_SOURCES + [SERVICE_SOURCE]
    for rel in all_business:
        obj = os.path.join(obj_dir,
                           rel.replace("/", "_").replace(".cpp", ".o"))
        argv = [tc["clang"], "--target=" + TARGET,
                "--sysroot=" + tc["sysroot"], "-nostdlib",
                "-fno-stack-protector", "-mno-outline-atomics",
                "-mbranch-protection=standard", "-c"] + flat_flags + inc + \
                [os.path.join(src, rel), "-o", obj]
        run(argv, dry_run=dry_run)

    # 2a. plugin 共享库：仅 plugin 三个源
    print("==> 链接 plugin 共享库")
    plugin_objs = [os.path.join(obj_dir,
                                s.replace("/", "_").replace(".cpp", ".o"))
                   for s in PLUGIN_SOURCES]
    plugin_out = os.path.join(lib_out, PLUGIN_SONAME)
    if not dry_run:
        argv = [tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
                "--ld-path=" + tc["lld"], "-nostdlib", "-fno-stack-protector",
                "-mno-outline-atomics", "-mbranch-protection=standard",
                "-shared", "-Wl,-soname," + PLUGIN_SONAME, "-o", plugin_out] + \
                plugin_objs + [
                "-L" + lib_out, "-L" + abi_apex, "-L" + abi_sys,
                "-Wl,-rpath-link," + lib_out, "-Wl,-rpath-link," + abi_apex,
                "-Wl,-rpath-link," + abi_sys,
                "-lcodec2_vndk", "-llog", "-lc++"]
        run(argv)
    else:
        print(f"[..] (dry) 链接 {PLUGIN_SONAME}")

    # 2b. libyuv 编译 + 归档
    print("==> libyuv 编译 + llvm-ar 归档")
    os.makedirs(libyuv_obj, exist_ok=True)
    yuv_flags = cxx + ["-std=c++17", "-fPIC", "-O2",
                       "-DLIBYUV_DISABLE_SME", "-DLIBYUV_DISABLE_SVE",
                       "-DLIBYUV_UNLIMITED_DATA",
                       "-Xclang", "-target-feature", "-Xclang", "+dotprod",
                       "-Xclang", "-target-feature", "-Xclang", "+i8mm",
                       "-Wno-unused-parameter"]
    yuv_objs = []
    for rel in LIBYUV_SOURCES:
        obj = os.path.join(libyuv_obj, rel.replace(".cc", ".o"))
        argv = [tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
                "-nostdlib", "-fno-stack-protector", "-mno-outline-atomics",
                "-mbranch-protection=standard", "-c"] + yuv_flags + \
                ["-I" + os.path.join(libyuv, "include"),
                 os.path.join(libyuv, "source", rel), "-o", obj]
        run(argv, dry_run=dry_run)
        yuv_objs.append(obj)
    archive = os.path.join(lib_out, LIBYUV_ARCHIVE)
    if not dry_run:
        run([tc["ar"], "rcs", archive] + yuv_objs)
    else:
        print(f"[..] (dry) ar rcs {LIBYUV_ARCHIVE}")

    # 3. 服务 PIE
    print("==> 链接服务 PIE")
    crtbegin = os.path.join(tc["bionic"], "cdh_crtbegin_dynamic.cpp")
    crtend = os.path.join(tc["bionic"], "cdh_crtend_android.S")
    startup_objs = [os.path.join(WORK_DIR, "cdh_crtbegin_dynamic.o"),
                    os.path.join(WORK_DIR, "cdh_crtend_android.o")]
    if not dry_run:
        for s, o in ((crtbegin, startup_objs[0]), (crtend, startup_objs[1])):
            run([tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
                 "-nostdlib", "-fno-stack-protector", "-mno-outline-atomics",
                 "-mbranch-protection=standard", "-fPIE", "-c", s, "-o", o])
    service_objs = [os.path.join(obj_dir, rel.replace("/", "_").replace(".cpp", ".o"))
                    for rel in BUSINESS_SOURCES] + \
                   [os.path.join(obj_dir, SERVICE_SOURCE.replace("/", "_").replace(".cpp", ".o"))]
    service_path = os.path.join(bin_out, SERVICE_BINARY_LEGACY)
    if not dry_run:
        sysibs = [os.path.join(abi_sys, l) for l in [
            "android.hardware.graphics.common@1.0.so",
            "android.hardware.media.c2@1.0.so",
            "android.hardware.media.c2@1.1.so",
            "android.hardware.media.c2@1.2.so",
            "libavservices_minijail.so", "libbase.so", "libbinder_ndk.so",
            "libchrome.so", "libcutils.so", "libhidlbase.so", "liblog.so",
            "libsfplugin_ccodec_utils.so",
            "libstagefright_bufferqueue_helper.so",
            "libstagefright_foundation.so", "libui.so", "libutils.so"]]
        apexibs = [os.path.join(abi_apex, l) for l in [
            "libcodec2_vndk.so", "libcodec2_hidl@1.0.so",
            "libcodec2_hidl@1.1.so", "libcodec2_hidl@1.2.so",
            "libcodec2_soft_common.so", "libcodec2_aidl.so",
            "android.hardware.media.c2-V1-ndk.so",
            "android.hardware.media.bufferpool2-V2-ndk.so",
            "android.hardware.common-V2-ndk.so"]]
        argv = [tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
                "--ld-path=" + tc["lld"], "-nostdlib", "-fno-stack-protector",
                "-mno-outline-atomics", "-mbranch-protection=standard",
                "-pie", "-Wl,-e,_start", "-Wl,--dynamic-linker,/system/bin/linker64",
                "-Wl,-z,relro", "-Wl,-z,now", "-o", service_path] + \
                startup_objs[:1] + service_objs + \
                ["-L" + lib_out, "-L" + abi_apex, "-L" + abi_sys,
                 "-Wl,-rpath-link," + lib_out, "-Wl,-rpath-link," + abi_apex,
                 "-Wl,-rpath-link," + abi_sys,
                 "-Wl,--as-needed", "-Wl,--allow-shlib-undefined",
                 os.path.join(lib_out, PLUGIN_SONAME)] + sysibs + apexibs + \
                [os.path.join(lib_out, LIBYUV_ARCHIVE), "-lc++", "-lc", "-lm",
                 "-ldl"] + startup_objs[1:]
        run(argv)
        shutil.copy2(service_path, os.path.join(bin_out, SERVICE_BINARY))
    else:
        print(f"[..] (dry) 链接 {SERVICE_BINARY} 与 legacy 副本")

    # 发布产物
    bin_hw = os.path.join(VENDOR_STAGE_DIR, "bin", "hw")
    lib64 = os.path.join(VENDOR_STAGE_DIR, "lib64")
    os.makedirs(bin_hw, exist_ok=True)
    os.makedirs(lib64, exist_ok=True)
    for name in (SERVICE_BINARY, SERVICE_BINARY_LEGACY):
        dst = os.path.join(bin_hw, name)
        if not dry_run:
            shutil.copy2(os.path.join(bin_out, name), dst)
            os.chmod(dst, 0o755)
        print(f"    写 bin/hw/{name}")
    if not dry_run:
        shutil.copy2(plugin_out, os.path.join(lib64, PLUGIN_SONAME))
        os.chmod(os.path.join(lib64, PLUGIN_SONAME), 0o755)
    print(f"    写 lib64/{PLUGIN_SONAME}")
    # 运行期库落盘到 /vendor/lib64：vendor 进程 linker namespace 只含 /vendor/lib64 与
    # LLNDK 白名单，非 LLNDK 的 system 库与 APEX 私有库都必须由本模块提供
    # （见 deps.py VENDOR_LIBS）
    for _lib, _src in media_v412_deps.VENDOR_LIBS.items():
        src_dir = abi_sys if _src == "system" else abi_apex
        dst = os.path.join(lib64, _lib)
        if not dry_run:
            shutil.copy2(os.path.join(src_dir, _lib), dst)
            os.chmod(dst, 0o755)
        print(f"    写 lib64/{_lib}")

    print("==> 资产落盘")
    write_asset("vendor/etc/init/android.hardware.media.c2@1.2-service-v4l2-64.rc",
                os.path.join(VENDOR_STAGE_DIR, "etc", "init",
                             "android.hardware.media.c2@1.2-service-v4l2-64.rc"),
                dry_run=dry_run)
    write_asset("vendor/etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy",
                os.path.join(VENDOR_STAGE_DIR, "etc", "seccomp_policy",
                             "android.hardware.media.c2-extended-seccomp_policy"),
                dry_run=dry_run)
    # vintf manifest：本服务注册 IComponentStore/default（mtk 假登记已让出该实例，
    # 见 android.hardware.media 的 manifest_media_c2_default.xml 改 default1）。
    # 源在 src/v4l2_codec2/service/，落盘到 /vendor/etc/vintf/manifest/。
    write_asset("src/v4l2_codec2/service/android.hardware.media.c2@1.2-service-v4l2.xml",
                os.path.join(VENDOR_STAGE_DIR, "etc", "vintf", "manifest",
                             "android.hardware.media.c2@1.2-service-v4l2.xml"),
                dry_run=dry_run)
    if not dry_run:
        merge_media_codecs_c2(os.path.join(VENDOR_STAGE_DIR, "etc"))
    else:
        print("[..] (dry) media_codecs_c2.xml 合并 c2.v4l2.*")

    print("[ok] media-v412 (codec2 v4l2) 构建完成")
    register_labels(dry_run=dry_run)
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 V4L2 Codec2 store 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()