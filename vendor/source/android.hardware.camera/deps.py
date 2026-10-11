#!/usr/bin/env python3
"""camera HAL 官方依赖准备（deps）。

camera 的接口定义 / camera_metadata 实现由 AOSP 提供，属外部官方依赖：
  * aidl/android15-release/ —— 仓库内构建产物（编译 -I 根，内容 gitignored）：
    7 包 aidl_api 展开（各自 .hash）：
      android.hardware.camera.common@1 / camera.metadata@3 / camera.device@3 /
      camera.provider@3 / graphics.common@5 / common@2 / common.fmq@1。
  * src/camera-metadata/ —— camera_metadata C 实现展开（构建产物，gitignored）：
    - AOSP 官方文件（同 pixel6 ensureCamMeta 在线拉取）：
        system/camera_metadata.h|camera_metadata_tags.h|camera_vendor_tags.h
        camera_metadata_hidden.h（media/include|private/include）
        cutils/compiler.h（libcutils）  log/*.h（liblog，整目录）
        camera_metadata.c|camera_metadata_tag_info.c（media/src）
    - 自研生成文件：camera_metadata_log_shim.c（__android_log_error_write shim）
  * out/tmp/camera-ndk-work/ —— 下载缓存（中间产物，不进源码树）：
      aidl-cache-api/       官方 aidl_api 快照（--import 导入）
      camera-metadata-cache/ camera_metadata 官方文件（--import-meta 导入）

获取策略（prepare 判定）：
  AIDL：aidl/android15-release 7 包已就绪（.hash 非空）且非 --force 直接复用；
        否则 CDH_AIDL_AOSP（本地目录）→ 下载缓存 → 报缺。
  camera_metadata：src/camera-metadata 关键文件就绪且非 --force 直接复用；
        否则 CDH_CAMERA_METADATA（本地展开目录）→ 下载缓存 → 在线 gitiles 拉取。

用法：
  python3 build/vendor/source/android.hardware.camera/deps.py            # 检查就绪
  python3 build/vendor/source/android.hardware.camera/deps.py --prepare  # 缺失时获取
  python3 build/vendor/source/android.hardware.camera/deps.py --force    # 强制重建
  python3 build/vendor/source/android.hardware.camera/deps.py --import <AIDL 快照目录>
  python3 build/vendor/source/android.hardware.camera/deps.py --import-meta <camera_metadata 目录>
"""

import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import urllib.request

# 本文件位于 build/vendor/source/android.hardware.camera/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))

from buildenv import OUT_DIR  # noqa: E402

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
AIDL_DIR = os.path.join(MODULE_DIR, "aidl")
AOSP_DIR = os.path.join(AIDL_DIR, "android15-release")  # 编译 -I 根（构建产物）
CAM_META_DIR = os.path.join(MODULE_DIR, "src", "camera-metadata")  # 展开目标
CACHE_DIR = os.path.join(OUT_DIR, "tmp", "camera-ndk-work", "aidl-cache-api")
CAM_META_CACHE = os.path.join(OUT_DIR, "tmp", "camera-ndk-work",
                              "camera-metadata-cache")

AOSP_TAG = "android-15.0.0_r36"
GITILES = "https://android.googlesource.com"

# <aidl 包名> -> (<接口模块相对路径>, <AIDL 版本>)
# 展开目标形态：<AOSP_DIR>/<rel>/aidl/aidl_api/<pkg>/<ver>/android/... + .hash
PACKAGES = {
    "android.hardware.common": ("common", 2),
    "android.hardware.common.fmq": ("common/fmq", 1),
    "android.hardware.graphics.common": ("graphics/common", 5),
    "android.hardware.camera.common": ("camera/common", 1),
    "android.hardware.camera.metadata": ("camera/metadata", 3),
    "android.hardware.camera.device": ("camera/device", 3),
    "android.hardware.camera.provider": ("camera/provider", 3),
}


def _expanded_dir(root, pkg, rel):
    """展开形态目录：<root>/<rel>/aidl/aidl_api/<pkg>/<ver>。"""
    return os.path.join(root, rel, "aidl", "aidl_api", pkg,
                        str(PACKAGES[pkg][1]))


def _hash_path(root, pkg, rel, kind):
    """某形态下该包的 .hash 路径。kind='aidl_api' 或 'flat'（扩展形态）。"""
    if kind == "aidl_api":
        return os.path.join(root, pkg, str(PACKAGES[pkg][1]), ".hash")
    return os.path.join(_expanded_dir(root, pkg, rel), ".hash")


def _aosp_ready():
    """aidl/android15-release 是否就绪：7 包 .hash 均存在且非空。"""
    return all(
        (lambda p: os.path.isfile(_hash_path(AOSP_DIR, p, PACKAGES[p][0],
                                             "flat"))
         and os.path.getsize(_hash_path(AOSP_DIR, p, PACKAGES[p][0],
                                        "flat")) > 0)(pkg)
        for pkg in PACKAGES)


def _inspect(path):
    """判断目录形态：'aidl_api'（<pkg>/<ver>/android/...）或 'flat'（<rel>/aidl/...）。"""
    if not os.path.isdir(path):
        raise RuntimeError(f"AIDL 源不存在: {path}")
    if all(os.path.isfile(_hash_path(path, pkg, rel, "aidl_api"))
           for pkg, rel in ((p, PACKAGES[p][0]) for p in PACKAGES)):
        return "aidl_api", path
    if all(os.path.isfile(_hash_path(path, pkg, rel, "flat"))
           for pkg, rel in ((p, PACKAGES[p][0]) for p in PACKAGES)):
        return "flat", path
    raise RuntimeError(
        f"AIDL 源形态不识别（缺 aidl_api 或 flat 的 .hash）: {path}")


def _pick_acquire_source():
    env = os.environ.get("CDH_AIDL_AOSP", "").strip()
    if env:
        return _inspect(os.path.expanduser(env))
    if os.path.isdir(CACHE_DIR):
        return _inspect(CACHE_DIR)
    raise RuntimeError(
        "无法获取官方 AIDL：请设置 CDH_AIDL_AOSP，或先运行 "
        "`deps.py --import <官方 aidl_api 快照目录>` 导入下载缓存")


def _copy_pkg(kind, src_root, pkg, rel):
    """把某包从源复制到展开目标 <AOSP_DIR>/<rel>/aidl/aidl_api/<pkg>/<ver>。"""
    version = PACKAGES[pkg][1]
    src = (os.path.join(src_root, pkg, str(version)) if kind == "aidl_api"
           else _expanded_dir(src_root, pkg, rel))
    if not os.path.isdir(src):
        return False
    dst = _expanded_dir(AOSP_DIR, pkg, rel)
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(src):
        s = os.path.join(src, name)
        d = os.path.join(dst, name)
        if os.path.isdir(s):
            shutil.copytree(s, d, dirs_exist_ok=True)
        else:
            shutil.copy2(s, d)
    return True


def acquire(force=False, quiet=False):
    """从官方源重建 aidl/android15-release（destructive：清空后重写）。"""
    kind, src_root = _pick_acquire_source()
    if not quiet:
        print(f"[deps] AIDL 获取源: {kind} {src_root}")
    if os.path.isdir(AOSP_DIR):
        shutil.rmtree(AOSP_DIR)
    os.makedirs(AOSP_DIR, exist_ok=True)
    missing = [pkg for pkg, (rel, _ver) in PACKAGES.items()
               if not _copy_pkg(kind, src_root, pkg, rel)]
    if missing:
        raise RuntimeError(f"AIDL 包缺失: {missing}")
    if not quiet:
        print(f"[deps] aidl/android15-release 已重建（{len(PACKAGES)} 包，"
              f"tag {AOSP_TAG}）")


def prepare(force=False, quiet=False):
    """确保 aidl/android15-release 就绪。已就绪（且非 --force）直接复用；否则获取。"""
    if not force and _aosp_ready():
        if not quiet:
            print("[deps] aidl/android15-release 已就绪，复用仓库展开")
        return
    acquire(force=force, quiet=quiet)


def import_cache(src):
    """把官方 aidl_api / flat 形态快照导入下载缓存（幂等：缺失才复制）。"""
    src = os.path.expanduser(src)
    kind, root = _inspect(src)
    os.makedirs(CACHE_DIR, exist_ok=True)
    imported = 0
    for pkg, (rel, ver) in PACKAGES.items():
        h = _hash_path(CACHE_DIR, pkg, rel, "aidl_api")
        if os.path.isfile(h) and not os.environ.get("CDH_AIDL_REIMPORT"):
            continue
        # 目标统一 aidl_api 形态：<cache>/<pkg>/<ver>/android/... + .hash
        s = (os.path.join(root, pkg, str(ver)) if kind == "aidl_api"
             else _expanded_dir(root, pkg, rel))
        if not os.path.isdir(s):
            continue
        d = os.path.join(CACHE_DIR, pkg, str(ver))
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d, exist_ok=True)
        for name in os.listdir(s):
            ss = os.path.join(s, name)
            dd = os.path.join(d, name)
            if os.path.isdir(ss):
                shutil.copytree(ss, dd, dirs_exist_ok=True)
            else:
                shutil.copy2(ss, dd)
        imported += 1
    print(f"[deps] 已导入 {imported} 包官方快照 -> {CACHE_DIR}")
    return imported


def check():
    """只校验 aidl/android15-release 就绪，不获取不写。"""
    problems = []
    for pkg, (rel, _ver) in PACKAGES.items():
        h = _hash_path(AOSP_DIR, pkg, rel, "flat")
        if not (os.path.isfile(h) and os.path.getsize(h) > 0):
            problems.append(f"{pkg}: 缺 {h}")
    if problems:
        print("[deps] aidl/android15-release 未就绪：")
        for p in problems:
            print("  -", p)
        return 1
    print(f"[deps] aidl/android15-release 就绪 ({len(PACKAGES)} 包)")
    return 0


# ---------------------------------------------------------------------------
# camera_metadata 官方材料（AOSP system/media + liblog + libcutils）
# ---------------------------------------------------------------------------

# camera_metadata 必需文件：<展开相对路径, 在线 URL 后缀>；log/ 目录单独整目录拉取
CAM_META_FILES = [
    ("system/camera_metadata.h",
     "media/+/%s/camera/include/system/camera_metadata.h" % AOSP_TAG),
    ("system/camera_metadata_tags.h",
     "media/+/%s/camera/include/system/camera_metadata_tags.h" % AOSP_TAG),
    ("system/camera_vendor_tags.h",
     "media/+/%s/camera/include/system/camera_vendor_tags.h" % AOSP_TAG),
    ("camera_metadata_hidden.h",
     "media/+/%s/camera/private/camera/include/camera_metadata_hidden.h" % AOSP_TAG),
    ("cutils/compiler.h",
     "core/+/%s/libcutils/include/cutils/compiler.h" % AOSP_TAG),
    ("camera_metadata.c",
     "media/+/%s/camera/src/camera_metadata.c" % AOSP_TAG),
    ("camera_metadata_tag_info.c",
     "media/+/%s/camera/src/camera_metadata_tag_info.c" % AOSP_TAG),
]
# 自研生成文件（非官方源，在线拉不到）：__android_log_error_write shim
CAM_META_SHIM = "camera_metadata_log_shim.c"
_CAM_META_SHIM_CONTENT = """#include <stdint.h>
#include <android/log.h>

int __android_log_error_write(int tag, const char *subTag, int32_t uid, const char *data, uint32_t dataLen) {
  (void)uid;
  (void)data;
  (void)dataLen;
  return __android_log_print(ANDROID_LOG_ERROR, "camera_metadata", "snet:%d:%s", tag, subTag ? subTag : "");
}
"""
_LOG_DIR_URL = "%s/platform/system/logging/+/%s/liblog/include/log" % (
    GITILES, AOSP_TAG)


def _curl(url):
    """拉取 gitiles 文本（?format=TEXT 为 base64）。"""
    with urllib.request.urlopen(url, timeout=30) as r:
        body = r.read()
    return base64.b64decode(body).decode("utf-8", "replace")


def _log_headers():
    """liblog include/log 目录的 .h 清单（gitiles JSON）。"""
    url = _LOG_DIR_URL + "/?format=JSON"
    with urllib.request.urlopen(url, timeout=30) as r:
        raw = r.read().decode("utf-8", "replace")
    # gitiles JSON 带 ])}' 前缀
    idx = raw.find("{")
    if idx >= 0:
        raw = raw[idx:]
    doc = json.loads(raw)
    return [e["name"] for e in doc.get("entries", [])
            if e.get("type") == "blob" and e["name"].endswith(".h")]


def _camera_meta_ready():
    """src/camera-metadata 是否就绪：必需官方文件 + 生成文件都存在且非空。"""
    for rel, _url in CAM_META_FILES:
        p = os.path.join(CAM_META_DIR, rel)
        if not (os.path.isfile(p) and os.path.getsize(p) > 0):
            return False
    return os.path.isfile(
        os.path.join(CAM_META_DIR, CAM_META_SHIM))


def _fetch_camera_meta_online(dst_cache):
    """从 AOSP gitiles 拉取 camera_metadata 官方文件到缓存目录。"""
    os.makedirs(dst_cache, exist_ok=True)
    for rel, suffix in CAM_META_FILES:
        url = "%s/platform/system/%s?format=TEXT" % (GITILES, suffix)
        dst = os.path.join(dst_cache, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.isfile(dst) and os.path.getsize(dst) > 0:
            continue
        try:
            _curl(url)
        except Exception:
            continue  # 单个失败继续；（缓存已部分就绪时 build 前可再跑一次）
        with open(dst, "w", encoding="utf-8") as f:
            f.write(_curl(url))
    os.makedirs(os.path.join(dst_cache, "log"), exist_ok=True)
    for name in _log_headers():
        dst = os.path.join(dst_cache, "log", name)
        if os.path.isfile(dst) and os.path.getsize(dst) > 0:
            continue
        try:
            _curl("%s/%s?format=TEXT" % (_LOG_DIR_URL, name))
        except Exception:
            continue
        with open(dst, "w", encoding="utf-8") as f:
            f.write(_curl("%s/%s?format=TEXT" % (_LOG_DIR_URL, name)))


def _copy_camera_meta_from(src_root):
    """把源目录（缓存或本地展开）同步到 src/camera-metadata。"""
    if os.path.isdir(CAM_META_DIR):
        shutil.rmtree(CAM_META_DIR)
    missing = []
    for rel, _url in CAM_META_FILES:
        s = os.path.join(src_root, rel)
        if not os.path.isfile(s):
            missing.append(rel)
            continue
        d = os.path.join(CAM_META_DIR, rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(s, d)
    # log/ 整目录复制
    slog = os.path.join(src_root, "log")
    if os.path.isdir(slog):
        shutil.copytree(slog, os.path.join(CAM_META_DIR, "log"))
    else:
        missing.append("log/")
    # 生成 shim
    shim = os.path.join(CAM_META_DIR, CAM_META_SHIM)
    os.makedirs(os.path.dirname(shim), exist_ok=True)
    with open(shim, "w", encoding="utf-8") as f:
        f.write(_CAM_META_SHIM_CONTENT)
    if missing:
        raise RuntimeError("camera_metadata 源缺文件: %s" % missing)


def _pick_camera_meta_source():
    env = os.environ.get("CDH_CAMERA_METADATA", "").strip()
    if env and os.path.isdir(os.path.expanduser(env)):
        return os.path.expanduser(env)
    if os.path.isdir(CAM_META_CACHE):
        return CAM_META_CACHE
    return None


def prepare_camera_meta(force=False, quiet=False):
    """确保 src/camera-metadata 就绪。已就绪（非 --force）复用；否则拉取+展开。"""
    if not force and _camera_meta_ready():
        if not quiet:
            print("[deps] src/camera-metadata 已就绪，复用仓库展开")
        return
    src = _pick_camera_meta_source()
    if src is None:
        # 无本地源：在线拉取到缓存再展开（失败会抛）
        _fetch_camera_meta_online(CAM_META_CACHE)
        src = CAM_META_CACHE
    if not quiet:
        print(f"[deps] camera_metadata 源: {src}")
    _copy_camera_meta_from(src)
    if not quiet:
        print("[deps] src/camera-metadata 已展开")


def import_camera_meta_cache(src):
    """把本地展开目录导入 camera-metadata 缓存。"""
    src = os.path.expanduser(src)
    if not os.path.isdir(src):
        raise RuntimeError(f"camera_metadata 源目录不存在: {src}")
    os.makedirs(CAM_META_CACHE, exist_ok=True)
    copied = 0
    for rel, _url in CAM_META_FILES:
        s = os.path.join(src, rel)
        if not os.path.isfile(s):
            continue
        d = os.path.join(CAM_META_CACHE, rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(s, d)
        copied += 1
    slog = os.path.join(src, "log")
    if os.path.isdir(slog):
        if os.path.isdir(os.path.join(CAM_META_CACHE, "log")):
            shutil.rmtree(os.path.join(CAM_META_CACHE, "log"))
        shutil.copytree(slog, os.path.join(CAM_META_CACHE, "log"))
    print(f"[deps] 已导入 camera_metadata 官方材料 -> {CAM_META_CACHE}")
    return copied


def check_camera_meta():
    if _camera_meta_ready():
        print("[deps] src/camera-metadata 就绪")
        return 0
    print("[deps] src/camera-metadata 未就绪：缺必需文件")
    return 1


def main():
    ap = argparse.ArgumentParser(description="camera HAL 官方依赖准备")
    ap.add_argument("--prepare", action="store_true",
                    help="aidl/android15-release 与 src/camera-metadata 缺失时获取")
    ap.add_argument("--force", action="store_true",
                    help="强制重新获取并重建依赖")
    ap.add_argument("--import", dest="import_src", metavar="PATH",
                    help="把官方 aidl_api 快照导入下载缓存（out/tmp/camera-ndk-work）")
    ap.add_argument("--import-meta", dest="import_meta", metavar="PATH",
                    help="把本地 camera_metadata 展开目录导入下载缓存")
    args = ap.parse_args()
    try:
        if args.import_src:
            import_cache(args.import_src)
            return 0
        if args.import_meta:
            import_camera_meta_cache(args.import_meta)
            return 0
        if args.prepare or args.force:
            prepare(force=args.force)
            prepare_camera_meta(force=args.force)
            return 0
        rc = check()
        rcm = check_camera_meta()
        return 0 if (rc == 0 and rcm == 0) else 1
    except RuntimeError as e:
        print(f"[deps] {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())