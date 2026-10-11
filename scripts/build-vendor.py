#!/usr/bin/env python3
"""oriole-16 vendor.img 重打包——薄入口（参考 note13-5G-15 移植）。

具体实现见 build/vendor/build/vendor_builder.py；此处仅解析参数并委托。
  1. 以 lineage vendor_a.img 为基座（virtio 内核匹配）
  2. debugfs 注入 build/vendor/patch + 基线 SELinux 标签 + build.prop
  3. e2fsck -fy 重打包 -> out/dist/rom/vendor.img

用法：
  python3 build/build-vendor.py
  python3 build/build-vendor.py --dry-run   # 只打印将执行的动作与命令
"""

import argparse
import os
import sys

# 本文件位于 <设备>/build/scripts/ 下，vendor 驱动在 <设备>/build/vendor/build/
_SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
_BUILD_DIR = os.path.dirname(_SCRIPTS_DIR)
VENDOR_BUILD_DIR = os.path.join(_BUILD_DIR, "vendor", "build")
sys.path.insert(0, VENDOR_BUILD_DIR)

from vendor_builder import build_vendor  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description="oriole-16 vendor.img 重打包")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build_vendor(dry_run=args.dry_run))


if __name__ == "__main__":
    main()