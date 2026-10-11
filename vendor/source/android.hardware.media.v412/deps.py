#!/usr/bin/env python3
"""android.hardware.media.v412 依赖准备（deps）。

本模块负责两类依赖：
1. 链接期 ABI 库研磨，来源均为 note13 自己的 lineage-22.0 super（禁止借用 pixel6）：
   - system 侧：super/system_a.img /system/lib64（debugfs dump）
   - swcodec APEX 侧：super/swcodec-work/apex_payload.img /lib64（debugfs dump）
2. 构建期三方依赖（AOSP 官方源，git 拉取/下载，全部走代码库而非借像素 6 产物）：
   - aidl/aosp：官方 AIDL 展开源（编译 -I 根，随库固化，含 .hash，无需联网）
   - platform-source：15 个 AOSP 仓（tag android-15.0.0_r36，--depth=1 --filter=blob:none），
     .platform-complete 标记缓存
   - hidl-gen 宿主工具（darwin-x86，gitiles 下载 + sha256 校验）
   - 20 个 HIDL 包生成 219 头（期望数量校验）

组装目录：<out>/dist/vendor-compile-dep/media-v412/{system,apex}/lib64（幂等）
构建期缓存：<out>/tmp/media-v412-work/（platform-source / hidl-tool / hidl-gen）

本模块是 v4l2 真实硬件编解码 HAL（c2.v4l2.*），链接闭包比假登记 store 大得多：
service PIE 需要 system 的 HIDL/基础库（libhidlbase/libstagefright_*/libchrome 等）
与 APEX 的 codec2 库（libcodec2_vndk/libcodec2_aidl 等）；plugin 共享库
libc2plugin_store.so 需要 libcodec2_vndk；libyuv 为自编静态库（见 build.py）。

除链接期 ABI 之外，本模块还定义**运行期落盘清单** VENDOR_LIBS：vendor 进程的
linker namespace 看不到非 LLNDK 的 system 库，服务/plugin 的 NEEDED 闭包中没有
LLNDK 覆盖也用不上 pristine vendor 的部分，必须随模块拷进 /vendor/lib64。

用法：
  python3 .../android.hardware.media.v412/deps.py            # 校验依赖齐全
  python3 .../android.hardware.media.v412/deps.py --prepare  # 缺失时获取
  python3 .../android.hardware.media.v412/deps.py --force    # 强制重建
  python3 .../android.hardware.media.v412/deps.py --abi      # 研磨 ABI 库（幂等）
  python3 .../android.hardware.media.v412/deps.py --check    # 只校验
"""

import argparse
import base64
import hashlib
import os
import shutil
import subprocess
import sys

# 本文件位于 build/vendor/source/android.hardware.media.v412/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import DIST_DIR, LINEAGE_SUPER_DIR, OUT_DIR  # noqa: E402
from debugfs import DEBUGFS, path_exists  # noqa: E402

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))

# ---- 构建期缓存目录（中间产物，不进源码树）----
WORK_DIR = os.path.join(OUT_DIR, "tmp", "media-v412-work")
AIDL_DIR = os.path.join(MODULE_DIR, "aidl")
# 仓库内维护的官方 AIDL 展开源（编译 -I 根，随库固化）
AOSP_DIR = os.path.join(AIDL_DIR, "aosp")

# ---- ABI 库来源（note13 自己的 lineage-22.0 super）----
SYSTEM_A_IMG = os.path.join(LINEAGE_SUPER_DIR, "system_a.img")
SWCODEC_IMG = os.path.join(LINEAGE_SUPER_DIR, "swcodec-work", "apex_payload.img")
# 组装目录
ABI_ROOT = os.path.join(DIST_DIR, "vendor-compile-dep", "media-v412")
ABI_SYS = os.path.join(ABI_ROOT, "system", "lib64")
ABI_APEX = os.path.join(ABI_ROOT, "apex", "media-swcodec", "lib64")

# ---- system 侧链接库（service PIE）----
# 对齐 pixel6 codec2_v4l2 的 linkService system 段（build/main.go）：
#   libbinder_ndk/libc++/libnativewindow 之外的 HIDL/基础/阶段特效库。
SYSTEM_LIBS = [
    "android.hardware.graphics.common@1.0.so",
    "android.hardware.media.c2@1.0.so",
    "android.hardware.media.c2@1.1.so",
    "android.hardware.media.c2@1.2.so",
    "libavservices_minijail.so",
    "libbase.so",
    "libbinder_ndk.so",
    "libc++.so",
    "libchrome.so",
    "libcutils.so",
    "libhidlbase.so",
    "liblog.so",
    "libnativewindow.so",
    "libsfplugin_ccodec_utils.so",
    "libstagefright_bufferqueue_helper.so",
    "libstagefright_foundation.so",
    "libui.so",
    "libutils.so",
]

# ---- apex（media-swcodec）侧链接库（service + plugin）----
# 对齐 linkService apex 段 + plugin 的 -lcodec2_vndk。
# 注意：这些只是**链接期**输入（-l 全路径收编），并不代表都要落盘 —— 链接器带
# --as-needed，未被引用符号的库（libcodec2_hidl@1.x / libcodec2_soft_common）不会
# 写进 NEEDED，因而运行期不会被加载，无需随模块落盘（落盘清单见 VENDOR_LIBS）。
APEX_LIBS = [
    "libcodec2_vndk.so",
    "libcodec2_hidl@1.0.so",
    "libcodec2_hidl@1.1.so",
    "libcodec2_hidl@1.2.so",
    "libcodec2_soft_common.so",
    "libcodec2_aidl.so",
    "android.hardware.media.c2-V1-ndk.so",
    "android.hardware.media.bufferpool2-V2-ndk.so",
    "android.hardware.common-V2-ndk.so",
]

# ---- 需随模块落盘到 /vendor/lib64 的库：{库名: 研磨来源} ----
# 来源取值 system（super/system_a.img 的 /system/lib64）或 apex（swcodec
# apex_payload.img 的 /lib64），表示从哪个 ABI 目录拷进产物。
#
# 判定依据：
#   1. vendor 进程的 linker namespace 只含 /vendor/lib64 与 LLNDK 白名单。本镜像
#      /system/etc/llndk.libraries.txt 仅 25 条（libc/liblog/libbinder_ndk/
#      libnativewindow/libsync/libvndksupport/...），下面这些库一条都不在名单里；
#      非 LLNDK 的 system 库对 vendor 进程不可见，pristine vendor 里也没有，
#      因此必须随模块落盘，否则运行时 "library not found"。
#   2. 清单 = 「服务 PIE + libc2plugin_store.so」的 ELF NEEDED 闭包定点求解结果，
#      共 41 个（含全部传递依赖）。链接期 --as-needed 已剔除未被引用的库
#      （libcodec2_hidl@1.x / libcodec2_soft_common / libstagefright_bufferqueue_helper
#      只在 -l 列表里、不产生 NEEDED），故不落盘。
#   3. 来源按「链接期实际消费的那份文件」定：链接命令的 -L 顺序是 abi_apex 先于
#      abi_sys，且 codec2 系库以全路径取自 abi_apex，所以 libcodec2_vndk.so /
#      libcodec2_aidl.so 取 APEX 副本（system 同名文件与 APEX 副本不 bit 相同，
#      取错会引入 ABI 漂移）。
APEX_VENDOR_LIBS = [
    "libcodec2_vndk.so",   # 链接期取自 abi_apex（plugin 的 -lcodec2_vndk 亦先命中 apex）
    "libcodec2_aidl.so",   # /system/lib64 无同名库，只能来自 APEX
]
SYSTEM_VENDOR_LIBS = [
    # 服务 / plugin 的直接非 LLNDK 依赖
    "android.hardware.media.c2-V1-ndk.so",
    "android.hardware.media.bufferpool2-V2-ndk.so",
    "android.hardware.common-V2-ndk.so",
    "android.hardware.common.fmq-V1-ndk.so",
    "android.hardware.graphics.common-V5-ndk.so",
    "android.hardware.graphics.allocator-V2-ndk.so",
    "libbinder.so",
    "libbase.so",
    "libcutils.so",
    "libutils.so",
    "libui.so",
    "libhidlbase.so",
    "libc++.so",
    "libfmq.so",
    "libion.so",
    "libcodec2.so",
    "libcodec2_hal_common.so",
    "libchrome.so",
    "libevent.so",
    "libdmabufheap.so",
    "libgralloctypes.so",
    "libaconfig_storage_read_api_cc.so",
    "server_configurable_flags.so",
    "libsfplugin_ccodec_utils.so",
    "libstagefright_foundation.so",
    "libstagefright_bufferpool@2.0.1.so",
    "libstagefright_aidl_bufferpool2.so",
    # 上述库的传递依赖（graphics buffer 池与 HAL 接口）
    "android.hardware.graphics.allocator@2.0.so",
    "android.hardware.graphics.allocator@3.0.so",
    "android.hardware.graphics.allocator@4.0.so",
    "android.hardware.graphics.bufferqueue@2.0.so",
    "android.hardware.graphics.common@1.0.so",
    "android.hardware.graphics.common@1.1.so",
    "android.hardware.graphics.common@1.2.so",
    "android.hardware.graphics.mapper@2.0.so",
    "android.hardware.graphics.mapper@2.1.so",
    "android.hardware.graphics.mapper@3.0.so",
    "android.hardware.graphics.mapper@4.0.so",
    "android.hardware.media.bufferpool@2.0.so",
]
VENDOR_LIBS = {**{n: "apex" for n in APEX_VENDOR_LIBS},
               **{n: "system" for n in SYSTEM_VENDOR_LIBS}}

# system 侧 ABI 研磨清单：链接用 SYSTEM_LIBS + 需落盘的非 LLNDK system 库
SYSTEM_DUMP_LIBS = sorted(set(SYSTEM_LIBS) | set(SYSTEM_VENDOR_LIBS))

# ---- 构建期三方依赖（AOSP 官方源）----
AOSP_TAG = "android-15.0.0_r36"
PLATFORM_REPOS = [
    "external/googletest", "external/libchrome", "external/libcxx",
    "external/libcxxabi", "external/libyuv",
    "frameworks/av", "frameworks/native", "hardware/interfaces",
    "hardware/libhardware", "system/core", "system/libbase", "system/libfmq",
    "system/libhidl", "system/libhwbinder", "system/logging",
]
HIDL_PACKAGES = [
    "android.hidl.base@1.0", "android.hidl.manager@1.0", "android.hidl.manager@1.1",
    "android.hidl.manager@1.2", "android.hidl.memory@1.0", "android.hidl.memory.token@1.0",
    "android.hidl.safe_union@1.0", "android.hidl.token@1.0",
    "android.hardware.graphics.bufferqueue@1.0",
    "android.hardware.graphics.bufferqueue@2.0",
    "android.hardware.graphics.common@1.0", "android.hardware.graphics.common@1.1",
    "android.hardware.graphics.common@1.2",
    "android.hardware.media@1.0", "android.hardware.media.bufferpool@1.0",
    "android.hardware.media.bufferpool@2.0",
    "android.hardware.media.c2@1.0", "android.hardware.media.c2@1.1",
    "android.hardware.media.c2@1.2", "android.hardware.media.omx@1.0",
]
HIDL_EXPECT_FILES = 219
# hidl-gen 宿主工具（darwin 宿主；sha256 校验）
HIDL_TOOL_DIR = "darwin-x86"
HIDL_TOOL_FILES = [
    ("bin/hidl-gen",
     "67f2a93691879ee827901f44e1b4217fd457b79fe7b0cd39feb4d75f2d32a8d9"),
    ("lib64/libc++.dylib",
     "581770d70a3632cb9945c7ca825ce7d538ae0c0540ef1ce085c8b624758eef6b"),
]


# ---------------------------------------------------------------------------
# 链接期 ABI 库研磨
# ---------------------------------------------------------------------------

def _dump_img_into(img, img_path, dst):
    """"img 内 /<img_path>/<name> -> dst（debugfs dump，幂等）。"""
    if os.path.isfile(dst) and os.path.getsize(dst) > 0:
        return
    if not os.path.isfile(img):
        print(f"[err] 依赖研磨: 缺镜像 {img}", file=sys.stderr)
        sys.exit(1)
    if not path_exists(img, img_path):
        print(f"[err] 依赖研磨: {img} 内找不到 {img_path}", file=sys.stderr)
        sys.exit(1)
    res = subprocess.run([DEBUGFS, "-R", f"dump -p {img_path} {dst}", img],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if res.returncode != 0 or not os.path.isfile(dst) or os.path.getsize(dst) == 0:
        print(f"[err] 依赖研磨: dump {img_path} 失败", file=sys.stderr)
        sys.exit(1)
    print(f"[ok] 依赖研磨: {os.path.basename(img_path)} <- {os.path.basename(img)}")


def prepare_abi(dry_run=False):
    """研磨链接期 ABI 库到 out/dist/vendor-compile-dep/media-v412/。"""
    os.makedirs(ABI_SYS, exist_ok=True)
    os.makedirs(ABI_APEX, exist_ok=True)
    sys_missing = [l for l in SYSTEM_DUMP_LIBS
                   if not os.path.isfile(os.path.join(ABI_SYS, l))]
    apex_missing = [l for l in APEX_LIBS
                    if not os.path.isfile(os.path.join(ABI_APEX, l))]
    if dry_run:
        print(f"[..] (dry) system ABI 待补齐 {len(sys_missing)} 个、apex ABI 待补齐 "
              f"{len(apex_missing)} 个")
        return ABI_ROOT
    for name in SYSTEM_DUMP_LIBS:
        _dump_img_into(SYSTEM_A_IMG, f"/system/lib64/{name}",
                       os.path.join(ABI_SYS, name))
    for name in APEX_LIBS:
        _dump_img_into(SWCODEC_IMG, f"/lib64/{name}",
                       os.path.join(ABI_APEX, name))
    print(f"[ok] ABI 库就绪: system({len(os.listdir(ABI_SYS))}) "
          f"+ apex({len(os.listdir(ABI_APEX))}) -> {ABI_ROOT}")
    return ABI_ROOT


def check_abi():
    problems = []
    for lib in SYSTEM_DUMP_LIBS:
        if not os.path.isfile(os.path.join(ABI_SYS, lib)):
            problems.append(f"system/lib64/{lib}")
    for lib in APEX_LIBS:
        if not os.path.isfile(os.path.join(ABI_APEX, lib)):
            problems.append(f"apex/media-swcodec/lib64/{lib}")
    if problems:
        print("[deps] ABI 库缺失：")
        for p in problems:
            print("  -", p)
        return False
    print(f"[deps] ABI 库齐全: system={len(os.listdir(ABI_SYS))} "
          f"+ apex={len(os.listdir(ABI_APEX))}")
    return True


# ---------------------------------------------------------------------------
# 构建期三方依赖：aidl/aosp / platform-source / hidl-gen / HIDL 头
# ---------------------------------------------------------------------------

def _aosp_ready():
    """aidl/aosp 是否就绪：5 个 AIDL 包 .hash 均存在且非空。"""
    hashes = [
        os.path.join(AOSP_DIR, "common/aidl/aidl_api/android.hardware.common",
                     "2", ".hash"),
        os.path.join(AOSP_DIR, "common/fmq/aidl/aidl_api/android.hardware.common.fmq",
                     "1", ".hash"),
        os.path.join(AOSP_DIR,
                     "graphics/common/aidl/aidl_api/android.hardware.graphics.common",
                     "5", ".hash"),
        os.path.join(AOSP_DIR,
                     "media/bufferpool/aidl/aidl_api/android.hardware.media.bufferpool2",
                     "2", ".hash"),
        os.path.join(AOSP_DIR, "media/c2/aidl/aidl_api/android.hardware.media.c2",
                     "1", ".hash"),
    ]
    return all(os.path.isfile(h) and os.path.getsize(h) > 0 for h in hashes)


def git_in(cwd, *argv):
    res = subprocess.run(["git", *argv], cwd=cwd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[err] git {' '.join(argv)} 失败 (rc={res.returncode})\n"
              f"  {res.stderr.strip()}", file=sys.stderr)
        sys.exit(1)


def prepare_platform_source(platform_root, dry_run=False):
    marker = os.path.join(platform_root, ".platform-complete")
    if dry_run:
        print("[..] (dry) 确保 platform-source 就位（15 个 AOSP 仓，git 拉取）")
        return
    if os.path.isfile(marker):
        print("[..] platform-source 已缓存（.platform-complete）")
        return
    os.makedirs(platform_root, exist_ok=True)
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
    with open(marker, "w", encoding="ascii") as f:
        f.write(AOSP_TAG + "\n")
    print(f"[ok] platform-source 就位（{len(PLATFORM_REPOS)} 仓）")


def prepare_hidl_gen(tool_root, dry_run=False):
    bin_path = os.path.join(tool_root, HIDL_TOOL_DIR, "bin", "hidl-gen")
    if dry_run:
        print("[..] (dry) 确保 hidl-gen 就位（下载 + sha256 校验）")
        return bin_path
    if os.path.isfile(bin_path):
        return bin_path
    os.makedirs(tool_root, exist_ok=True)
    base = ("https://android.googlesource.com/platform/prebuilts/build-tools/+/"
            + AOSP_TAG + "/" + HIDL_TOOL_DIR + "/")
    for rel_path, sha in HIDL_TOOL_FILES:
        dst = os.path.join(tool_root, HIDL_TOOL_DIR, rel_path.replace("/", os.sep))
        if os.path.isfile(dst):
            continue
        url = base + rel_path + "?format=TEXT"
        res = subprocess.run(["curl", "-fsSL", "--retry", "5",
                              "--retry-delay", "2", "--retry-all-errors", url],
                             capture_output=True)
        if res.returncode != 0:
            print(f"[err] 下载 hidl 工具失败: {url}", file=sys.stderr)
            sys.exit(1)
        data = base64.b64decode("".join(res.stdout.decode("utf8", "replace").split()))
        got = hashlib.sha256(data).hexdigest()
        if got != sha:
            print(f"[err] hidl 工具 sha256 错误 {rel_path}: got={got} want={sha}",
                  file=sys.stderr)
            sys.exit(1)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        if rel_path.startswith("bin/"):
            os.chmod(dst, 0o755)
    return bin_path


def generate_hidl(hidl_gen_bin, platform_root, out_dir, dry_run=False):
    if dry_run:
        print(f"[..] (dry) 生成 {HIDL_EXPECT_FILES} 个 HIDL 包头")
        return
    if os.path.isdir(out_dir) and \
            len(glob_hidl_headers(out_dir)) >= HIDL_EXPECT_FILES:
        print(f"[..] hidl 头已生成（>= {HIDL_EXPECT_FILES}）")
        return
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    argv = [hidl_gen_bin, "-Lc++-headers",
            "-randroid.hidl:" + os.path.join(platform_root, "system", "libhidl", "transport"),
            "-randroid.hardware:" + os.path.join(platform_root, "hardware", "interfaces"),
            "-o", out_dir] + HIDL_PACKAGES
    tool_lib = os.path.join(os.path.dirname(os.path.dirname(hidl_gen_bin)), "lib64")
    subprocess.run(argv, check=True,
                   env={**os.environ, "LANG": "en_US.UTF-8",
                        "LC_ALL": "en_US.UTF-8",
                        "DYLD_LIBRARY_PATH": tool_lib,
                        "LD_LIBRARY_PATH": tool_lib})
    n = len(glob_hidl_headers(out_dir))
    if n != HIDL_EXPECT_FILES:
        print(f"[err] HIDL 生成数量错误: got={n} want={HIDL_EXPECT_FILES}",
              file=sys.stderr)
        sys.exit(1)
    print(f"[ok] hidl 头生成 {n} 个")


def glob_hidl_headers(out_dir):
    import glob
    return glob.glob(os.path.join(out_dir, "**", "*.h"), recursive=True)


def prepare_platform(force=False, quiet=False, dry_run=False):
    """确保 platform-source / hidl-gen / HIDL 头全部就绪。"""
    platform_root = os.path.join(WORK_DIR, "platform-source")
    hidl_tool = os.path.join(WORK_DIR, "hidl-tool")
    gen_hidl = os.path.join(WORK_DIR, "hidl-gen")
    prepare_platform_source(platform_root, dry_run=dry_run)
    hidl_gen_bin = prepare_hidl_gen(hidl_tool, dry_run=dry_run)
    generate_hidl(hidl_gen_bin, platform_root, gen_hidl, dry_run=dry_run)
    if not quiet:
        print("[deps] platform-source / hidl-gen / HIDL 头就绪")
    return platform_root, hidl_gen_bin, gen_hidl


def check_platform():
    problems = []
    platform_root = os.path.join(WORK_DIR, "platform-source")
    marker = os.path.join(platform_root, ".platform-complete")
    if not os.path.isfile(marker):
        problems.append("platform-source 未拉取（缺 .platform-complete）")
    hidl_gen_bin = os.path.join(WORK_DIR, "hidl-tool", HIDL_TOOL_DIR, "bin", "hidl-gen")
    if not os.path.isfile(hidl_gen_bin):
        problems.append("hidl-gen 缺失")
    n = len(glob_hidl_headers(os.path.join(WORK_DIR, "hidl-gen")))
    if n < HIDL_EXPECT_FILES:
        problems.append(f"hidl 头不足（got {n} < {HIDL_EXPECT_FILES}）")
    if problems:
        print("[deps] 构建期三方依赖未就绪：")
        for p in problems:
            print("  -", p)
        return False
    print(f"[deps] 构建期三方依赖齐全（hidl 头 {n}）")
    return True


def check():
    """校验全部依赖（ABI + aosp + platform/hidl），不获取不写。"""
    ok = True
    ok = check_abi() and ok
    if not _aosp_ready():
        print("[deps] aidl/aosp 未就绪：5 个 AIDL 包 .hash 缺失或为空")
        ok = False
    else:
        print("[deps] aidl/aosp 就绪（5 包）")
    ok = check_platform() and ok
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="media-v412 HAL 依赖准备")
    ap.add_argument("--prepare", action="store_true",
                    help="缺失时获取构建期三方依赖 + ABI 库")
    ap.add_argument("--force", action="store_true",
                    help="强制重建构建期三方依赖 + ABI 库")
    ap.add_argument("--abi", action="store_true", help="研磨链接期 ABI 库（幂等）")
    ap.add_argument("--check", action="store_true", help="只校验依赖齐全")
    args = ap.parse_args()
    if args.check:
        return check()
    if args.prepare or args.force or args.abi:
        prepare_abi()
        if args.prepare or args.force:
            prepare_platform(force=args.force)
        return 0
    return check()


if __name__ == "__main__":
    sys.exit(main())
