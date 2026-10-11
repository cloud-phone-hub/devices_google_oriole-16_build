#!/usr/bin/env python3
"""note13-5G-15 health HAL 官方 AIDL 依赖准备（deps）。

android.hardware.health 接口由 AOSP hardware/interfaces/health 提供（外部官方依赖）：
  * aidl/aosp/ —— 仓库内固化的「v3 冻结 AIDL 展开源」（12 个 .aidl + .hash，编译
    -I 根）；接口版本 3（hash 3bab6273a5491102b29c9d7a1f0efa749533f46d），与真机
    vintf 声明（android.hardware.health v3 / IHealth/default）一致；
  * 就绪即复用，不联网；缺失时由 --import <官方 aidl_api 快照目录> 重新展开
    （形态 <包名>/<版本>/.hash + android/...）。

用法:
  python3 build/vendor/source/android.hardware.health/deps.py                # 检查
  python3 build/vendor/source/android.hardware.health/deps.py --import <目录>  # 展开
"""

import argparse
import os
import shutil
import sys

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
AOSP_DIR = os.path.join(MODULE_DIR, "aidl", "aosp")

PKG = "android.hardware.health"
REL = "android/hardware/health"
VERSION = 3


def _hash_path(root):
    return os.path.join(root, REL, ".hash")


def _aosp_ready():
    h = _hash_path(AOSP_DIR)
    return os.path.isfile(h) and os.path.getsize(h) > 0


def _import(src_root):
    """从官方 aidl_api 快照目录展开到 aidl/aosp（destructive）。"""
    pkg_root = os.path.join(src_root, PKG, str(VERSION))
    src = os.path.join(pkg_root, REL)
    if not os.path.isdir(src):
        print(f"[err] 快照目录形态不符（缺 {pkg_root}）: {src_root}", file=sys.stderr)
        sys.exit(1)
    if os.path.isdir(AOSP_DIR):
        shutil.rmtree(AOSP_DIR)
    n = 0
    for dirpath, _dirs, files in os.walk(src):
        for name in files:
            if not name.endswith(".aidl"):
                continue
            s = os.path.join(dirpath, name)
            d = os.path.join(AOSP_DIR, os.path.relpath(s, pkg_root))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(s, d)
            n += 1
    shutil.copy2(os.path.join(pkg_root, ".hash"), _hash_path(AOSP_DIR))
    print(f"[deps] aidl/aosp 已展开: {n} 个 .aidl（{PKG}@v{VERSION}）")


def prepare(quiet=False):
    """确保 aidl/aosp 就绪（正常路径：仓库内已固化，直接复用）。"""
    if _aosp_ready():
        if not quiet:
            print("[deps] aidl/aosp 已就绪，复用仓库源")
        return
    env = os.environ.get("CDH_AIDL_AOSP", "").strip()
    if not env:
        raise RuntimeError(
            "aidl/aosp 缺失：请设置 CDH_AIDL_AOSP 或运行 "
            "deps.py --import <官方 aidl_api 快照目录>")
    _import(os.path.expanduser(env))


def main():
    ap = argparse.ArgumentParser(description="health HAL AIDL 依赖准备")
    ap.add_argument("--import", dest="import_dir",
                    help="官方 aidl_api 快照目录（含 <包名>/<版本>/）")
    args = ap.parse_args()
    if args.import_dir:
        _import(os.path.expanduser(args.import_dir))
        return
    prepare()


if __name__ == "__main__":
    main()