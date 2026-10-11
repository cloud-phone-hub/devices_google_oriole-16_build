#!/usr/bin/env python3
"""android.hardware.audio —— 自研 audio HAL 依赖准备（deps，自包含）。

本模块完全自包含，不依赖任何其他 vendor 模块的产物（不复制、不软链）。负责两类依赖：

1. 链接期 ABI 运行库：沿用仓库共享的 build/vendor/build/deps.py 研磨逻辑，
   来源全部为 note13 自己的 lineage-22.0 super（vendor_a 解包 /system_a.img），
   组装到 <out>/dist/vendor-compile-dep/（本模块只链接 libc++/libbinder_ndk，运行期
   由 note13 guest 自身的 /vendor/lib64 + /system/lib64 提供）。

2. 编译期三方代码（AOSP platform 头 + audio reference 实现）：独立 git 拉取到
   本模块 <out>/tmp/audio-ndk-work/platform-source（tag android-15.0.0_r36，
   .platform-complete 缓存标记）。包含 system 侧头（libfmq 模板 / cutils / utils /
   libbase / libsystem）+ hardware/interfaces（audio/aidl/default reference 实现
   + audio 头），不涉及 hidl/apex。

3. AIDL 依赖：官方 aidl_api 展开源固化在本模块 aidl/aosp/（含版本目录 + .hash），
   构建期校验各包 .hash 存在且非空。

用法：
  python3 .../android.hardware.audio/deps.py            # 校验依赖齐全
  python3 .../android.hardware.audio/deps.py --prepare  # 缺失时获取
  python3 .../android.hardware.audio/deps.py --abi      # 研磨 ABI 运行库（幂等）
  python3 .../android.hardware.audio/deps.py --check    # 只校验
"""

import argparse
import os
import shutil
import subprocess
import sys

# 本文件位于 build/vendor/source/android.hardware.audio/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import OUT_DIR  # noqa: E402
import deps as _shared_deps  # noqa: E402  共享运行库研磨（build/vendor/build/deps.py）

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))

# ---- 构建期缓存目录（中间产物，不进源码树）----
WORK_DIR = os.path.join(OUT_DIR, "tmp", "audio-ndk-work")
AIDL_DIR = os.path.join(MODULE_DIR, "aidl")
# 仓库内维护的官方 AIDL 展开源（编译 -I 根，随库固化，含 .hash）
AOSP_DIR = os.path.join(AIDL_DIR, "aosp")

# ---- platform-source：本模块自包含（一次性真实复制，非软链、非运行时共享）----
# 构建期只需要 AOSP 三方源码的头（libfmq 模板/cutils/utils/libbase/libsystem）
# 与 audio reference 实现（hardware/interfaces/audio/aidl/default）。本机有现成的
# platform-source 快照（android-15.0.0_r36）时**一次性复制**所需子目录为本模块自有
# 独立目录（cp -RP 保留源内软链本体，避免悬空链报错）；复制完成后与源无关、自包含，
# 不产生任何软链，也不在构建期引用其他模块。快照缺失时才独立 git 拉取。
# 注意：partial clone（--filter=blob:none）在弱网下 checkout 需持续联网取 blob，
# 独立全量拉取不可靠，故优先复用本机既有快照目录。
SHARED_PLATFORM_SOURCE = os.path.join(OUT_DIR, "tmp", "media-v412-work",
                                      "platform-source")
OWN_PLATFORM_SOURCE = os.path.join(WORK_DIR, "platform-source")

# 本模块编译 audio 服务所需的 AOSP platform 仓（快照缺失时独立 git 拉取）
AOSP_TAG = "android-15.0.0_r36"
PLATFORM_REPOS = [
    "external/libcxx", "external/libcxxabi",
    "hardware/interfaces",  # audio/aidl/default（reference 实现）+ audio 头
    "system/core", "system/libbase", "system/libfmq", "system/logging",
]
# 需复制到本模块 platform-source 的子目录（编译所需 system 头 + audio reference 源码）
PLATFORM_COPY_RELS = [
    "system/core/libcutils", "system/core/libutils", "system/core/libsystem",
    "system/libbase", "system/libfmq", "system/logging/liblog",
    "hardware/interfaces/audio/aidl",  # default/（reference 实现）+ 头
]
# 本模块编译 tip 的 include 路径（相对 platform-source）
PLATFORM_INCLUDE_RELS = [
    "system/libfmq/include", "system/libfmq/base",
    "system/core/libcutils/include",
    "system/core/libutils/include",
    "system/core/libsystem/include",
    "system/libbase/include",
]

# ---- AIDL 包：aidl/aosp 下官方 aidl_api 展开源（版本 + .hash）----
# 与 v412 相同：aidl_api/<pkg>/<version>/ 含 <pkg 路径>/ 与 .hash。
# 版本对应原 build.py AIDL_PACKAGES 的版本号（保持二进制接口一致）。
AIDL_PACKAGES = [
    ("common/aidl/aidl_api/android.hardware.common", "2"),
    ("common/fmq/aidl/aidl_api/android.hardware.common.fmq", "1"),
    ("audio/aidl/aidl_api/android.hardware.audio.common", "2"),
    ("audio/aidl/aidl_api/android.hardware.audio.core", "2"),
    ("audio/aidl/aidl_api/android.hardware.audio.core.sounddose", "1"),
    ("audio/aidl/aidl_api/android.hardware.audio.effect", "2"),
]
# framework 稳定类型（无版本）：android.media.audio.common，随库固化在 aosp/frameworks/
FRAMEWORK_TYPES_DIRS = [
    "frameworks/av/media/audio/aidl",  # android/media/audio/common/*
]


def aosp_ready():
    """aidl/aosp 是否就绪：所有版本化包目录存在且 .hash 非空。"""
    for rel, version in AIDL_PACKAGES:
        hash_path = os.path.join(AOSP_DIR, rel, version, ".hash")
        if not (os.path.isfile(hash_path) and os.path.getsize(hash_path) > 0):
            return False
    return True


def git_in(cwd, *argv):
    res = subprocess.run(["git", *argv], cwd=cwd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[err] git {' '.join(argv)} 失败 (rc={res.returncode})\n"
              f"  {res.stderr.strip()}", file=sys.stderr)
        sys.exit(1)


def prepare_platform_source(platform_root, dry_run=False):
    """确保 platform-source 就位：本模块自包含（真实复制，非软链、非共享）。

    优先从本机既有 android-15.0.0_r36 快照一次性复制所需子目录（cp -RP，
    源内软链按链接复制，避免悬空链报错）；复制完成后与源无关。快照缺失时
    独立 git 拉取。不产生任何软链，也不在构建期引用其他模块。
    """
    if dry_run:
        print(f"[..] (dry) 确保 platform-source 就位（复制 {len(PLATFORM_COPY_RELS)} 子目录或 git 拉取）")
        return platform_root
    marker = os.path.join(platform_root, ".platform-complete")
    if os.path.isfile(marker):
        print(f"[..] platform-source 已就位: {platform_root}")
        return platform_root
    os.makedirs(platform_root, exist_ok=True)
    if os.path.isdir(SHARED_PLATFORM_SOURCE):
        # 从既有快照一次性复制所需子目录（真实复制，非软链）
        for rel in PLATFORM_COPY_RELS:
            src = os.path.join(SHARED_PLATFORM_SOURCE, rel)
            dst = os.path.join(platform_root, rel)
            if not os.path.isdir(src):
                print(f"[err] 快照缺子目录: {src}", file=sys.stderr)
                sys.exit(1)
            if os.path.isdir(dst):
                shutil.rmtree(dst)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            # -R 递归；-P 保留源内符号链接本身（不解析目标），
            # 避免 AOSP 源里悬空软链（如 libcxx test 的坏链接）导致复制失败
            subprocess.run(["cp", "-RP", src, dst], check=True)
        print(f"[..] 从快照复制 {len(PLATFORM_COPY_RELS)} 个子目录 -> {platform_root}（自包含）")
    else:
        # 快照缺失：独立 git 拉取（partial clone 弱网下可能失败，见头注释）
        for rel in PLATFORM_REPOS:
            dest = os.path.join(platform_root, rel)
            if os.path.isdir(dest):
                shutil.rmtree(dest)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            tmp = os.path.join(os.path.dirname(dest),
                               f".plat-fetch-{os.getpid()}")
            if os.path.isdir(tmp):
                shutil.rmtree(tmp)
            os.makedirs(tmp)
            url = "https://android.googlesource.com/platform/" + rel
            print(f"[..] 拉取 platform 仓 {rel}")
            git_in(tmp, "init", "--quiet")
            git_in(tmp, "remote", "add", "origin", url)
            git_in(tmp, "fetch", "--quiet", "--depth=1", "--filter=blob:none",
                   "origin", "tag", AOSP_TAG)
            git_in(tmp, "checkout", "--quiet", "--detach", AOSP_TAG)
            os.rename(tmp, dest)
        print(f"[..] 独立拉取 {len(PLATFORM_REPOS)} 仓 -> {platform_root}")
    with open(marker, "w", encoding="ascii") as f:
        f.write(AOSP_TAG + "\n")
    print(f"[ok] platform-source 就位: {platform_root}")
    return platform_root


def prepare_abi(dry_run=False):
    """链接期 ABI 运行库（libc++/libbinder_ndk，来源 note13 lineage super）。"""
    if dry_run:
        print("[..] (dry) 研磨链接期 ABI 运行库（共享 deps）")
        return _shared_deps.prepare_runtime_libs(dry_run=True)
    return _shared_deps.prepare_runtime_libs(dry_run=False)


def check():
    ok = True
    if not aosp_ready():
        print("[deps] aidl/aosp 未就绪：std 版本包 .hash 缺失或为空")
        ok = False
    else:
        print(f"[deps] aidl/aosp 就绪（{len(AIDL_PACKAGES)} 包）")
    platform_root = OWN_PLATFORM_SOURCE
    if not os.path.isfile(os.path.join(platform_root, ".platform-complete")):
        print(f"[deps] platform-source 缺失（{platform_root} 无 .platform-complete）")
        ok = False
    else:
        print("[deps] platform-source 就绪（本模块独立拉取）")
    if ok:
        print("[deps] audio HAL 依赖齐全")
        return 0
    return 1


def main():
    ap = argparse.ArgumentParser(description="audio HAL 依赖准备")
    ap.add_argument("--prepare", action="store_true", help="缺失时获取")
    ap.add_argument("--force", action="store_true", help="强制重建")
    ap.add_argument("--abi", action="store_true", help="研磨链接期 ABI 运行库")
    ap.add_argument("--check", action="store_true", help="只校验依赖齐全")
    args = ap.parse_args()
    if args.check:
        return check()
    if args.prepare or args.force or args.abi:
        prepare_abi()
        if args.prepare or args.force:
            prepare_platform_source(OWN_PLATFORM_SOURCE)
        return 0
    return check()


if __name__ == "__main__":
    sys.exit(main())