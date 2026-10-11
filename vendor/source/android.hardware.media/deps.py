#!/usr/bin/env python3
"""android.hardware.media 依赖准备（deps）。

链接期 ABI 库研磨（note13 自己的来源，禁止借用 pixel6 镜像提取件）：
- system 侧：lineage-22.0 super 的 system_a.img /system/lib64（debugfs dump）
- 假登记 store 链接闭包仅 3 库（libc++/libbinder_ndk/libnativewindow）；其余
  传递依赖（liblog/libdl/libm/libc 等）由 NDK sysroot 的 -l 系列覆盖
组装到 <out>/dist/vendor-compile-dep/media/system/lib64，幂等。

v4l2 真实 HAL 路线移除后不再需要 swcodec APEX 库、AOSP 平台源、HIDL 头与
libyuv（原 --platform 段已删除）。

用法：
  python3 .../android.hardware.media/deps.py --check      # 只校验 ABI 库齐全
  python3 .../android.hardware.media/deps.py --abi        # 研磨 ABI 库（幂等）
"""

import argparse
import os
import subprocess
import sys

# 本文件位于 build/vendor/source/android.hardware.media/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import DIST_DIR, LINEAGE_SUPER_DIR  # noqa: E402
from debugfs import DEBUGFS, path_exists  # noqa: E402

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))

# ---- ABI 库来源（note13 自己的 lineage-22.0 super）----
SYSTEM_A_IMG = os.path.join(LINEAGE_SUPER_DIR, "system_a.img")
# 组装目录
ABI_ROOT = os.path.join(DIST_DIR, "vendor-compile-dep", "media")
ABI_SYS = os.path.join(ABI_ROOT, "system", "lib64")

# system 侧链接库（假登记 store 链接闭包；-llog/-ldl/-lm/-lc 走 NDK sysroot）
SYSTEM_LIBS = [
    "libc++.so", "libbinder_ndk.so", "libnativewindow.so",
]


def _dump_system_lib(name, dst):
    """system_a.img /system/lib64/<name> -> dst（debugfs dump）。"""
    if os.path.isfile(dst) and os.path.getsize(dst) > 0:
        return
    if not os.path.isfile(SYSTEM_A_IMG):
        print(f"[err] 依赖研磨: 缺 lineage system_a.img：{SYSTEM_A_IMG}", file=sys.stderr)
        sys.exit(1)
    if not path_exists(SYSTEM_A_IMG, f"/system/lib64/{name}"):
        print(f"[err] 依赖研磨: system_a.img 内找不到 /system/lib64/{name}",
              file=sys.stderr)
        sys.exit(1)
    res = subprocess.run([DEBUGFS, "-R", f"dump -p /system/lib64/{name} {dst}",
                          SYSTEM_A_IMG],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if res.returncode != 0 or not os.path.isfile(dst) or os.path.getsize(dst) == 0:
        print(f"[err] 依赖研磨: dump /system/lib64/{name} 失败", file=sys.stderr)
        sys.exit(1)
    print(f"[ok] 依赖研磨: {name} <- lineage super system_a.img /system/lib64")


def prepare_abi(dry_run=False):
    """研磨 system 链接期 ABI 库到 out/dist/vendor-compile-dep/media/。"""
    os.makedirs(ABI_SYS, exist_ok=True)
    sys_missing = [l for l in SYSTEM_LIBS
                   if not os.path.isfile(os.path.join(ABI_SYS, l))]
    if dry_run:
        print(f"[..] (dry) system ABI 将补齐 {len(sys_missing)} 个库 -> {ABI_SYS}")
        return ABI_ROOT
    for name in SYSTEM_LIBS:
        _dump_system_lib(name, os.path.join(ABI_SYS, name))
    print(f"[ok] ABI 库就绪: system({len(os.listdir(ABI_SYS))}) -> {ABI_ROOT}")
    return ABI_ROOT


def check():
    problems = []
    for lib in SYSTEM_LIBS:
        if not os.path.isfile(os.path.join(ABI_SYS, lib)):
            problems.append(os.path.relpath(os.path.join(ABI_SYS, lib), _DEVICE_ROOT))
    if problems:
        print("[deps] ABI 库缺失：")
        for p in problems:
            print("  -", p)
        return 1
    print(f"[deps] ABI 库齐全: system={len(os.listdir(ABI_SYS))}")
    return 0


def main():
    ap = argparse.ArgumentParser(description="media HAL 依赖准备")
    ap.add_argument("--abi", action="store_true", help="研磨链接期 ABI 库")
    ap.add_argument("--check", action="store_true", help="只校验 ABI 库")
    args = ap.parse_args()
    if args.check:
        return check()
    prepare_abi()
    return 0


if __name__ == "__main__":
    sys.exit(main())
