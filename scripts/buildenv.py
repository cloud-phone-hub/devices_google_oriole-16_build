"""oriole-16（Pixel 6 / Android 16）构建环境：固件选择 + 宿主工具链定位 + 设备事实/路径。

参考 note13-5G-15 的 buildenv 移植而来（vendor/vendor_dlkm 阶段需要其中的 e2fs/qemu
工具定位、lineage 基座路径、vendor 注入树等）。本文件位于 <设备>/build/scripts/ 下，
故 _DEV_ROOT 为向上 3 级、PROJECT_ROOT 再向上 3 级。

各脚本统一从这里读取：
  - 构建选择：CDH_BUILD_ID（build-id）决定固件与基线件；
  - 路径/命名：设备根、out/dist 布局、lineage 底包、版本目录等，可被 CDH_* 覆盖；
  - 宿主工具：debugfs / e2fsck / resize2fs / qemu-img / baselines。

工具解析优先级（_tool / any_tool）：env 直指 > brew 常用位置 > PATH。
"""

import glob
import json
import os
import re
import shutil

# 宿主工具常用安装位置（macOS brew e2fsprogs / Android SDK emulator）
_E2FSPROGS_SBIN = "/opt/homebrew/opt/e2fsprogs/sbin"
_QEMU_CANDIDATES = (
    os.path.expanduser("~/Library/Android/sdk/emulator/qemu-img"),
    "/opt/homebrew/bin/qemu-img",
    "/usr/local/bin/qemu-img",
)


def env(name, default):
    """读取环境变量，未设置或为空时回退 default。"""
    return os.environ.get(name) or default


def _tool(name):
    """定位 e2fsprogs 工具；env 覆盖 > brew sbin > PATH，缺失时抛异常。"""
    cand = os.environ.get("E2FSPROGS_" + name.upper())
    if cand and os.path.isfile(cand):
        return cand
    brew = os.path.join(_E2FSPROGS_SBIN, name)
    if os.path.isfile(brew):
        return brew
    found = shutil.which(name)
    if found:
        return found
    raise FileNotFoundError(
        f"缺少 e2fsprogs 工具: {name}（brew install e2fsprogs）")


def e2fs_tools():
    """返回 (debugfs, e2fsck, resize2fs) 绝对路径元组。"""
    return _tool("debugfs"), _tool("e2fsck"), _tool("resize2fs")


def _qemu_img():
    """定位 qemu-img（QEMU_IMG 环境变量优先，其次常见安装位置，最后 PATH）。"""
    cand = os.environ.get("QEMU_IMG")
    if cand and os.path.isfile(cand):
        return cand
    for p in _QEMU_CANDIDATES:
        if os.path.isfile(p):
            return p
    found = shutil.which("qemu-img")
    if found:
        return found
    raise FileNotFoundError("缺少 qemu-img（可用 QEMU_IMG 环境变量指定）")


def any_tool(names, suffixes=()):
    """依次在 PATH 与常见位置查找任一命令，返回首个可执行绝对路径或 None。"""
    for name in names:
        found = shutil.which(name)
        if found:
            return found
        for suf in suffixes:
            cand = os.path.join(suf, name)
            if os.path.isfile(cand):
                return cand
    return None


_LZ4_BIN = "/opt/homebrew/opt/lz4/bin"


def lz4_tool():
    """定位 lz4 命令（vendor_boot fragment 编解码）。"""
    brew = os.path.join(_LZ4_BIN, "lz4")
    if os.path.isfile(brew):
        return brew
    found = shutil.which("lz4")
    if found:
        return found
    raise FileNotFoundError("缺少 lz4（brew install lz4）")


# =====================================================================
# 设备事实与路径（单一数据源，env 优先，其次 manifest.json 推导）
# =====================================================================

# 本文件位于 <设备>/build/scripts/ 下：向上 3 级为设备根，再向上 3 级为项目根
_SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
_BUILD_DIR = os.path.dirname(_SCRIPTS_DIR)          # <设备>/build（manifest/baselines/vendor 所在）
_DEV_ROOT = os.path.dirname(_BUILD_DIR)
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_DEV_ROOT)))

ROMS_DIR = os.path.join(_DEV_ROOT, "roms")          # 官方固件输入（非源码）
OUT_DIR = os.path.join(_DEV_ROOT, "out")            # 设备编译产物
BASELINES_DIR = os.path.join(_BUILD_DIR, "baselines")


def _manifest():
    """读取 build/manifest.json（固件/构建声明），缺失或解析失败返回 {}。"""
    try:
        with open(os.path.join(_BUILD_DIR, "manifest.json"), encoding="utf-8") as f:
            return json.load(f) or {}
    except (OSError, ValueError):
        return {}


def _manifest_build():
    builds = (_manifest().get("build") or [])
    return builds[0] if builds else {}


# ---- 构建选择：CDH_BUILD_ID 优先，其次 manifest.json 首个 build ----
BUILD_ID = env("CDH_BUILD_ID", _manifest_build().get("build-id") or "")

# Android 版本（主/主.次，取自 manifest build 的 android-version，如 16.0.0）
_ANDROID_VERSION = _manifest_build().get("android-version") or ""
OS = _ANDROID_VERSION.split(".")[0] if _ANDROID_VERSION else "16"
_ANDROID_MINOR = ".".join(_ANDROID_VERSION.split(".")[:2]) if _ANDROID_VERSION else "16.0"

# 固件版本号（oriole-cp1a.260405.005，取自 manifest build 的 version；CDH_VERSION 可覆盖）
VERSION = env("CDH_VERSION", _manifest_build().get("version") or "")

# 版本目录：out/device/oriole/16/<版本小写>（与 pack-roms.py 的 IMAGE_DIR 一致）
VERSION_DIR = os.path.join(PROJECT_ROOT, "out", "device",
                           "oriole", OS, VERSION.lower())


def baselines_file():
    """定位 build/baselines/<build-id>.json（基线件清单）。

    优先 CDH_BASELINES_JSON；其次 CDH_BUILD_ID 对应文件；否则取 baselines/ 目录
    内唯一 JSON（单 ROM 设备）。无法确定时返回 None。
    """
    p = os.environ.get("CDH_BASELINES_JSON")
    if p:
        return p if os.path.isfile(p) else None
    build_id = os.environ.get("CDH_BUILD_ID")
    if build_id:
        p = os.path.join(BASELINES_DIR, f"{build_id}.json")
        return p if os.path.isfile(p) else None
    entries = sorted(glob.glob(os.path.join(BASELINES_DIR, "*.json")))
    return entries[0] if len(entries) == 1 else None


def load_baselines():
    """读取基线清单 JSON；文件缺失或解析失败时返回 None。"""
    p = baselines_file()
    if not p:
        return None
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


# ---- 官方固件输入（Pixel factory：roms/<factory-dir>/ 平铺镜像）----
# ROM_ROOT_DIR = 官方镜像平铺目录（CDH_FACTORY_DIR / manifest factory-dir）
ROM_ROOT_DIR = env(
    "CDH_FACTORY_DIR",
    os.path.join(ROMS_DIR, _manifest_build().get("factory-dir") or ""))
ROM_ZIP = env(
    "CDH_FACTORY_ZIP",
    os.path.join(ROMS_DIR, _manifest_build().get("factory-zip") or ""))

# 关键镜像（官方固件平铺在 ROM_ROOT_DIR）
SUPER_IMG = os.path.join(ROM_ROOT_DIR, "super.img")
BOOT_IMG = os.path.join(ROM_ROOT_DIR, "boot.img")
VENDOR_BOOT_IMG = os.path.join(ROM_ROOT_DIR, "vendor_boot.img")
VBMETA_IMG = os.path.join(ROM_ROOT_DIR, "vbmeta.img")
SUPER_OUT = ROM_ROOT_DIR

# ---- lineage 底包（UTM 虚机，vda -> GPT 分区）----
LINEAGE_ZIP = env("CDH_LINEAGE_ZIP", "")
if not LINEAGE_ZIP and _manifest_build().get("lineage-zip"):
    _cand = os.path.join(PROJECT_ROOT, "lineage-rom", _manifest_build()["lineage-zip"])
    if os.path.isfile(_cand):
        LINEAGE_ZIP = _cand
LINEAGE_VDA_MEMBER = "LineageOS_on_arm64.utm/Data/vda.img"  # lineage-23.2 起为 raw
LINEAGE_OUT_DIR = env(
    "CDH_LINEAGE_DIR",
    os.path.join(PROJECT_ROOT, "lineage-rom",
                 _manifest_build().get("lineage-dir") or "lineage-23"))
# lineage 底包 super 逻辑分区（vendor/vendor_dlkm/odm 基座，virtio 内核匹配）
LINEAGE_SUPER_DIR = os.path.join(LINEAGE_OUT_DIR, "super")
# lineage 底包 boot 链（UTM vda 平铺产物；boot_a 有内嵌 ramdisk，是 fstab 注入载体）
LINEAGE_BOOT_IMG = os.path.join(LINEAGE_OUT_DIR, "boot_a.img")
LINEAGE_VENDOR_BOOT_IMG = os.path.join(LINEAGE_OUT_DIR, "vendor_boot_a.img")

# ---- 输出布局 ----
DIST_DIR = os.path.join(OUT_DIR, "dist")
ROM_DIST = os.path.join(DIST_DIR, "rom")        # 分发镜像（boot/vendor/super/...）
TMP_DIR = os.path.join(OUT_DIR, "tmp")          # 调试临时（不用 /tmp）
KERNEL_OUT = os.path.join(OUT_DIR, "kernel")    # 内核编译产物（Image/modules/config）
KERNEL_MODULES_DIR = os.path.join(DIST_DIR, "kernel", "modules")  # 模块归集
VENDOR_OUT = os.path.join(OUT_DIR, "vendor", "bin", "hw")         # HAL 产物
KERNEL_SRC = os.path.join(_DEV_ROOT, "kernel", "source")          # 内核源码

# ---- vendor patch 注入树（note13 式：debugfs 注入的第二棵树）----
VENDOR_PATCH_DIR = os.path.join(_BUILD_DIR, "vendor", "patch")
# vendor 注入的第二棵树：source 子模块落盘的 /vendor 内容（out/dist/vendor，
# 相对路径即镜像内 /vendor 路径）。
VENDOR_STAGE_DIR = os.path.join(DIST_DIR, "vendor")

# ---- 分发/最终产物 ----
VENDOR_DIST = os.path.join(ROM_DIST, "vendor.img")                # build-vendor 产物
VENDOR_DLKM_DIST = os.path.join(ROM_DIST, "vendor_dlkm.img")      # build-vendor_dlkm 产物
SUPER_DIST = os.path.join(ROM_DIST, "super.img")                  # pack_super 产物
BOOT_DIST = os.path.join(ROM_DIST, "boot.img")                    # build-boot 产物
VENDOR_BOOT_DIST = os.path.join(ROM_DIST, "vendor_boot.img")      # build-vendor-boot 产物
PATCH_DIST = os.path.join(ROM_DIST, "patch.img")                  # vdc 载入的打包镜像
VERSION_VDA = os.path.join(VERSION_DIR, "vda.img")                # pack-roms 产物
VERSION_DATA = os.path.join(VERSION_DIR, "data.qcow2")            # pack-roms 数据盘

# ---- 构建阶段（与 build/main.py 的阶段名保持一致）----
STAGES = ["unpack", "kernel", "boot", "vendor_boot",
          "vendor", "vendor_dlkm", "system", "super", "pack"]