#!/usr/bin/env python3
"""radio HAL 官方 AIDL 依赖准备（deps）。

radio 的接口定义（android.hardware.radio.*@3，8 包）由 AOSP
hardware/interfaces/radio/aidl/aidl_api 提供，属外部官方依赖：
  * aidl/aosp/ —— 仓库内维护的「官方 AIDL 展开源」（编译 -I 根，8 包 204 个
    .aidl + 各包 .hash），随代码固化；构建时直接使用，不重复下载。
  * out/tmp/radio-ndk-work/aidl-cache-api/ —— 官方 aidl_api 快照的「下载缓存」
    （中间产物，不进源码树）；仅当 aidl/aosp 缺失或 --force 时才被拉取/用于
    重新生成 aosp。

获取策略（prepare 判定）：
  * aidl/aosp 8 包已就绪（.hash 非空）且非 --force：直接复用，不联网不生成。
  * 否则按优先级获取并展开到 aidl/aosp：
      1. CDH_AIDL_AOSP —— 环境变量：在线 URL（googlesource 目录 / github 快照 tar）
         或本地目录（aidl_api 或展开形态均可）；未设置时回退缓存；
      2. 下载缓存 out/tmp/radio-ndk-work/aidl-cache-api（aidl_api 形态）：
         由 `deps.py --import <官方 aidl_api 快照目录>` 导入（一次性）。
  网络不可达且缓存未导入时，报错并提示 --import。

针对 MTK 补充的接口（vendor.mediatek.hardware.mtkradioex.*，无 AOSP 官方源）
随库放到 aidl/mtk（见 prepare_mtk）。当前自研 radio HAL 采用「别名注册 +
VINTF 不声明」（见 src/main.cpp 注释），MTK 接口不参与编译；若未来实现方案 1
（真正桥接 MTK radioex 服务），再把 aidl/mtk 各包补全并加入 build.py 的
AIDL_PACKAGES。

用法：
  python3 build/vendor/source/android.hardware.radio/deps.py            # 检查 aosp 就绪
  python3 build/vendor/source/android.hardware.radio/deps.py --prepare  # aosp 缺失时获取
  python3 build/vendor/source/android.hardware.radio/deps.py --force    # 强制重新获取并展开
  python3 build/vendor/source/android.hardware.radio/deps.py --import <快照目录>  # 导入下载缓存
"""

import argparse
import os
import shutil
import sys

# 本文件位于 build/vendor/source/android.hardware.radio/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))

from buildenv import OUT_DIR  # noqa: E402

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
AIDL_DIR = os.path.join(MODULE_DIR, "aidl")
# 仓库内维护的官方 AIDL 展开源（编译 -I 根，随代码固化）
AOSP_DIR = os.path.join(AIDL_DIR, "aosp")
# 仓库内 MTK 补充接口（无官方源，随库维护）
MTK_DIR = os.path.join(AIDL_DIR, "mtk")
# 官方 aidl_api 快照的下载缓存（中间产物，随 radio-ndk-work 构建产物区）
CACHE_DIR = os.path.join(OUT_DIR, "tmp", "radio-ndk-work", "aidl-cache-api")

# AIDL v3 快照版本与官方 TAG（.hash 由快照自带，逐包校验）
AIDL_VERSION = 3
AOSP_TAG = "android-15.0.0_r36"

# 与 build.py AIDL_PACKAGES 对应：<aidl 包名> -> <展开相对路径>
PACKAGES = {
    "android.hardware.radio": "android/hardware/radio",
    "android.hardware.radio.config": "android/hardware/radio/config",
    "android.hardware.radio.data": "android/hardware/radio/data",
    "android.hardware.radio.messaging": "android/hardware/radio/messaging",
    "android.hardware.radio.modem": "android/hardware/radio/modem",
    "android.hardware.radio.network": "android/hardware/radio/network",
    "android.hardware.radio.sim": "android/hardware/radio/sim",
    "android.hardware.radio.voice": "android/hardware/radio/voice",
}

# MTK 补充接口包：<包名> -> <展开相对路径>（仅目录归位，不参与编译）
MTK_PACKAGES = {
    "vendor.mediatek.hardware.mtkradioex.data": "vendor/mediatek/hardware/mtkradioex/data",
    "vendor.mediatek.hardware.mtkradioex.messaging": "vendor/mediatek/hardware/mtkradioex/messaging",
    "vendor.mediatek.hardware.mtkradioex.modem": "vendor/mediatek/hardware/mtkradioex/modem",
    "vendor.mediatek.hardware.mtkradioex.network": "vendor/mediatek/hardware/mtkradioex/network",
    "vendor.mediatek.hardware.mtkradioex.sim": "vendor/mediatek/hardware/mtkradioex/sim",
    "vendor.mediatek.hardware.mtkradioex.voice": "vendor/mediatek/hardware/mtkradioex/voice",
}


def _aosp_ready():
    """aidl/aosp 是否就绪：8 包 .hash 均存在且非空。"""
    return all(
        (lambda h: os.path.isfile(h) and os.path.getsize(h) > 0)(
            os.path.join(AOSP_DIR, rel, ".hash"))
        for rel in PACKAGES.values())


def _inspect(path):
    """判断给定目录形态：'aidl_api'（<pkg>/<ver>/android/...）或 'flat'（<rel>/...）。"""
    if not os.path.isdir(path):
        raise RuntimeError(f"AIDL 源不存在: {path}")
    if all(os.path.isfile(os.path.join(path, pkg, str(AIDL_VERSION), ".hash"))
           for pkg in PACKAGES):
        return "aidl_api", path
    if all(os.path.isfile(os.path.join(path, rel, ".hash"))
           for rel in PACKAGES.values()):
        return "flat", path
    raise RuntimeError(
        f"AIDL 源形态不识别（缺 <pkg>/<ver>/.hash 或 <rel>/.hash）: {path}")


def _pick_acquire_source():
    """aosp 缺失时从此获取官方 AIDL：CDH_AIDL_AOSP > 下载缓存。"""
    env = os.environ.get("CDH_AIDL_AOSP", "").strip()
    if env:
        return _inspect(os.path.expanduser(env))
    if os.path.isdir(CACHE_DIR):
        return _inspect(CACHE_DIR)
    raise RuntimeError(
        "无法获取官方 AIDL：请设置 CDH_AIDL_AOSP，或先运行 "
        "`deps.py --import <官方 aidl_api 快照目录>` 导入下载缓存")


def _copy_pkg_to_aosp(kind, src_root, pkg, rel):
    """把某包从源复制/展开到 aidl/aosp。成功返回 True。"""
    if kind == "aidl_api":
        # <src>/<pkg>/<ver>/<rel>/...(aidl) + <pkg>/<ver>/.hash
        pkg_root = os.path.join(src_root, pkg, str(AIDL_VERSION))
        src = os.path.join(pkg_root, rel)
        if not os.path.isdir(src):
            return False
        os.makedirs(os.path.join(AOSP_DIR, rel), exist_ok=True)
        for dp, _dirs, files in os.walk(src):
            for f in files:
                if not f.endswith(".aidl"):
                    continue
                s = os.path.join(dp, f)
                # 相对 pkg_root 去掉 <pkg>/<ver> 层级，落盘 <rel>/...（编译 -I 根形态）
                d = os.path.join(AOSP_DIR, os.path.relpath(s, pkg_root))
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copy2(s, d)
        h = os.path.join(pkg_root, ".hash")
        if os.path.isfile(h):
            shutil.copy2(h, os.path.join(AOSP_DIR, rel, ".hash"))
        return True
    # 展开形态：<src>/<rel>/*.aidl + .hash
    src = os.path.join(src_root, rel)
    if not os.path.isdir(src):
        return False
    for dp, _dirs, files in os.walk(src):
        for f in files:
            if not (f.endswith(".aidl") or f == ".hash"):
                continue
            s = os.path.join(dp, f)
            d = os.path.join(AOSP_DIR, os.path.relpath(s, src_root))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(s, d)
    return True


def acquire(force=False, quiet=False):
    """从官方源重建 aidl/aosp（destructive：清空后重写）。"""
    kind, src_root = _pick_acquire_source()
    if not quiet:
        print(f"[deps] AIDL 获取源: {kind} {src_root}")
    if os.path.isdir(AOSP_DIR):
        shutil.rmtree(AOSP_DIR)
    os.makedirs(AOSP_DIR, exist_ok=True)
    missing = []
    for pkg, rel in PACKAGES.items():
        if not _copy_pkg_to_aosp(kind, src_root, pkg, rel):
            missing.append(pkg)
    if missing:
        raise RuntimeError(f"AIDL 包缺失: {missing}")
    if not quiet:
        print(f"[deps] aidl/aosp 已重建（{len(PACKAGES)} 包，tag {AOSP_TAG}）")


def prepare(force=False, quiet=False):
    """确保 aidl/aosp 就绪。已就绪（且非 --force）直接复用；否则获取并展开。"""
    if not force and _aosp_ready():
        if not quiet:
            print("[deps] aidl/aosp 已就绪，复用仓库源")
        return
    acquire(force=force, quiet=quiet)


def import_cache(src):
    """把官方 aidl_api 快照导入下载缓存（幂等：缺失才复制；--force/重导入见 env）。"""
    src = os.path.expanduser(src)
    kind, root = _inspect(src)
    if kind != "aidl_api":
        raise RuntimeError(f"缓存导入仅支持 aidl_api 形态快照: {src}")
    os.makedirs(CACHE_DIR, exist_ok=True)
    imported = 0
    for pkg in PACKAGES:
        h = os.path.join(CACHE_DIR, pkg, str(AIDL_VERSION), ".hash")
        if os.path.isfile(h) and not os.environ.get("CDH_AIDL_REIMPORT"):
            continue
        d = os.path.join(CACHE_DIR, pkg)
        if os.path.isdir(d):
            shutil.rmtree(d)
        shutil.copytree(os.path.join(root, pkg), d)
        imported += 1
    print(f"[deps] 已导入 {imported} 包官方快照 -> {CACHE_DIR}")
    return imported


def prepare_mtk(force=False, quiet=False):
    """确保 aidl/mtk 目录存在（MTK 补充接口存放处；接口补全时用）。"""
    os.makedirs(MTK_DIR, exist_ok=True)
    for _pkg, rel in MTK_PACKAGES.items():
        os.makedirs(os.path.join(MTK_DIR, rel), exist_ok=True)
    if not quiet:
        print(f"[deps] aidl/mtk 就绪（{len(MTK_PACKAGES)} 包，接口随库维护）")


def check():
    """只校验 aidl/aosp 就绪，不获取不写。"""
    problems = []
    for pkg, rel in PACKAGES.items():
        h = os.path.join(AOSP_DIR, rel, ".hash")
        if not (os.path.isfile(h) and os.path.getsize(h) > 0):
            problems.append(f"{pkg}: 缺 {h}")
    if problems:
        print("[deps] aidl/aosp 未就绪：")
        for p in problems:
            print("  -", p)
        return 1
    print("[deps] aidl/aosp 就绪 (8 包)")
    return 0


def main():
    ap = argparse.ArgumentParser(description="radio HAL AIDL 官方依赖准备")
    ap.add_argument("--prepare", action="store_true",
                    help="aidl/aosp 缺失时从官方源获取并展开")
    ap.add_argument("--force", action="store_true", help="强制重新获取并重建 aidl/aosp")
    ap.add_argument("--import", dest="import_src", metavar="PATH",
                    help="把官方 aidl_api 快照导入下载缓存（out/tmp/radio-ndk-work）")
    args = ap.parse_args()
    try:
        if args.import_src:
            import_cache(args.import_src)
            return 0
        if args.prepare or args.force:
            prepare(force=args.force)
            prepare_mtk(force=args.force)
            return 0
        return check()
    except RuntimeError as e:
        print(f"[deps] {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())