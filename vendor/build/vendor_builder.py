#!/usr/bin/env python3
"""note13-5G-15 vendor.img 重打包编排（由 build-vendor.py 薄入口调用）。

参考 pioneer-phone 的 patch_vendor.sh + 00_egl_props.sh：
  1. 用 lineage 底包 vendor_a.img 作基座（virtio 内核匹配，真机厂商 vendor 是
     MTK 内核模块/驱动，不可直接用于 virtio 云主机）；
  2. 一次 debugfs 会话完成注入树（inject）+ build.prop 渲染修正（props）；
  3. e2fsck -fy 重打包，产物输出 out/dist/rom/vendor.img。

分层（参考 pixel6-15 system/build）：debugfs 工具层 + props/selinux/inject
规则模块；本模块仅编排流程。入口 build-vendor.py 仅做参数解析后委托
build_vendor(dry_run=...)。
"""

import math
import os
import re
import shutil
import sys

from buildenv import (
    LINEAGE_SUPER_DIR, ROM_DIST, TMP_DIR, VENDOR_DIST, VENDOR_PATCH_DIR,
    VENDOR_STAGE_DIR, e2fs_tools,
)
from debugfs import DEBUGFS, E2FSCK, cat, run, run_command_file
import deps
import fstab
import gold_props
import gold_selinux
import inject
import props
import prune
import sepolicy
import source_build

RESIZE2FS = e2fs_tools()[2]

# lineage 底包 vendor 逻辑分区（virtio 内核匹配）作基座
VENDOR_BASE = os.path.join(LINEAGE_SUPER_DIR, "vendor_a.img")

WORK_DIR = os.path.join(TMP_DIR, "vendor")

# 本阶段实际执行的 source 子构建（见 build_vendor 内 0b 的口径说明）
SOURCE_BUILDS_STAGE1 = ("daemon", "android.hardware.wifi")

# ueventd 视频节点规则：v4l2 HAL（android.hardware.media.v412）以 media 用户运行，
# 而内核 cdh_media.ko（virtio-media 驱动）创建的 /dev/video* 默认 0600 root:root，
# DAC 直接拒绝 open（早于 SELinux）。对齐 pixel6-15 samsung 链路：属主 media:camera。
# /dev/video* 必须允许 media 组读写，否则 enumerateSupportedPixelformats 全空、
# V4L2Decoder open("/dev/video0") 返回 EACCES（实测 errno=13）。
UEVENTD_VIDEO_RULE = "/dev/video*          0660    media   camera"
UEVENTD_VIDEO_MARKER = b"# cdh note13: virtio-media v4l2 video nodes"
UEVENTD_VIDEO_LABEL = "u:object_r:vendor_configs_file:s0"

# FRP 块设备：PersistentDataBlockService 按 ro.frp.pst=/dev/block/by-name/frp 打开
# FRP 分区（本盘 vda12，见 pack-roms.VDA_PARTITIONS；标签 frp_block_device 见
# sepolicy.FRP_FC_LINES）。ueventd 建的块节点默认 0600 root:root，system_server
# (uid 1000) 打开会 EACCES：
#   java.io.FileNotFoundException: /dev/block/by-name/frp: open failed:
#   EACCES (Permission denied) → pdb init 超时 → system_server FATAL。
# 真机 FRP 分区即 root:system 0660 可读写，此处对齐（popsicle-16 同法实测）。
UEVENTD_FRP_RULE = "/dev/block/vda12      0660    root    system"
UEVENTD_FRP_MARKER = b"# cdh oriole-16: FRP block device (pdb)"

# wifi 宿主控制面节点：自研 IWifi HAL（android.hardware.wifi-service.cdh-note13）以
# `user wifi` 运行，而 virtio_console 建的 /dev/hvc9 默认 0600 root:root → HAL 打开即
# EACCES（实测 logcat：CdhWifiHal: host channel /dev/hvc9 open failed: Permission denied），
# host 的 AP 档位（scan_profile 帧）永远进不了客机 → 扫描列表恒空。
# SELinux 侧早已放行（hal_wifi_default 对 cdh_wifi_channel 有 chr_file open/read/write），
# 卡在 DAC，故给 wifi:wifi 0660。
UEVENTD_WIFI_RULE = "/dev/hvc9            0660    wifi    wifi"
UEVENTD_WIFI_MARKER = b"# cdh oriole-16: wifi host channel /dev/hvc9"


def ueventd_video_cmds(work_img, tmp_dir=None):
    """读回 /etc/ueventd.rc，追加所需规则（video / FRP / wifi，逐条幂等），返回 debugfs 命令。

    与 fstab 同机制：基于工作镜像的「原始」内容拼接，命令序里只有最后一次 write
    生效——故所有规则在**一次** write 里补齐（否则后写的会覆盖先写的）。
    本函数须在 sepolicy 链之后、props 链之前调用（互不冲突，均独立文件）。
    """
    tmp_dir = tmp_dir or os.path.join(TMP_DIR, "vendor")
    os.makedirs(tmp_dir, exist_ok=True)
    img = "/etc/ueventd.rc"
    data = cat(work_img, img)
    if data is None:
        print(f"[warn] 镜像内缺 {img}，跳过节点授权")
        return []
    need_video = UEVENTD_VIDEO_MARKER not in data
    need_frp = UEVENTD_FRP_MARKER not in data
    need_wifi = UEVENTD_WIFI_MARKER not in data
    if not need_video and not need_frp and not need_wifi:
        print("[ok] ueventd.rc 已含 video/FRP/wifi 规则，跳过")
        return []
    rule = b""
    if need_video:
        rule += b"\n# " + UEVENTD_VIDEO_MARKER + b"\n" + \
            UEVENTD_VIDEO_RULE.encode() + b"\n"
    if need_frp:
        rule += b"\n" + UEVENTD_FRP_MARKER + b"\n" + \
            UEVENTD_FRP_RULE.encode() + b"\n"
    if need_wifi:
        rule += b"\n" + UEVENTD_WIFI_MARKER + b"\n" + \
            UEVENTD_WIFI_RULE.encode() + b"\n"
    tmp = os.path.join(tmp_dir, "ueventd.rc")
    with open(tmp, "wb") as f:
        f.write(data + rule)
    added = "+".join(n for n, need in (("video", need_video), ("FRP", need_frp),
                                       ("wifi", need_wifi))
                     if need)
    print(f"[ok] ueventd.rc 追加节点规则（{added}，幂等）")
    return [
        f"rm {img}",
        f"write {tmp} {img}",
        f"sif {img} mode 0x81a4",
        f"ea_set {img} security.selinux {UEVENTD_VIDEO_LABEL}",
    ]


def _estimate_new_inodes():
    """统计注入树将占用的新 inode 数（文件 + 子目录，保守计）。"""
    n = 0
    for tree in (VENDOR_PATCH_DIR, VENDOR_STAGE_DIR):
        if not os.path.isdir(tree):
            continue
        for root, dirs, files in os.walk(tree):
            n += len(dirs) + len(files)
    return n


def _ensure_vendor_capacity(work_img, needed_inodes):
    """扩展 vendor 工作镜像，保证注入可用 inode/块。

    lineage 基座空闲 inode 极少（实测 29），而 ext4 的 inode 只随新增 block
    group 增长；注入内容多（如 audio HAL 全家桶 ~55 inode）时 write 会报
    "Could not allocate inode in ext2 filesystem"。按整组补足（+272 inode/组）。
    """
    res = run([DEBUGFS, "-R", "stats", work_img], capture_output=True, text=True)
    out = res.stdout

    def _int(key):
        m = re.search(rf"^{re.escape(key)}:\s+(\d+)", out, re.M)
        return int(m.group(1)) if m else 0

    free_inodes = _int("Free inodes")
    free_blocks = _int("Free blocks")
    block_size = _int("Block size") or 4096
    ipg = _int("Inodes per group") or 1
    bpg = _int("Blocks per group")
    blocks = _int("Block count")

    inode_groups = math.ceil(max(0, needed_inodes - free_inodes) / ipg)
    if inode_groups == 0:
        return
    new_blocks = blocks + inode_groups * bpg
    print(f"[..] vendor inode 不足（空闲 {free_inodes} < 需 {needed_inodes}），"
          f"resize2fs +{inode_groups} 组 -> {new_blocks} 块（空闲块 {free_blocks}）")
    with open(work_img, "rb+") as f:
        f.truncate(new_blocks * block_size)
    res2 = run([RESIZE2FS, work_img], capture_output=True, text=True)
    if res2.returncode not in (0, 1):
        print(res2.stderr)
        sys.exit(1)

def build_vendor(dry_run=False):
    if not os.path.isfile(VENDOR_BASE):
        print(f"[err] 基座 vendor_a.img 缺失: {VENDOR_BASE}（先 unpack lineage）")
        sys.exit(1)
    if not os.path.isdir(VENDOR_PATCH_DIR):
        print(f"[err] 注入树缺失: {VENDOR_PATCH_DIR}")
        sys.exit(1)

    # 0) 编译依赖准备（source 子构建前置）：keymint 等链接期运行库由
    #    note13 lineage-22.0 super 组装（deps.py），就绪后才进入子构建。
    deps.prepare_runtime_libs(dry_run=dry_run)

    # 0b) 优先编译 source 子模块（daemon 等），产物就绪后再进入镜像重打包。
    #     阶段口径：本盘编**设备无关**的 guest daemon（cph_daemon.rc /
    #     cph_start_adbd.rc 依赖 /vendor/bin/daemon，AdbKeyHVC2 + adbd-over-5555 全靠它，
    #     引导期没它就没有 logcat 通道）+ wifi HAL（虚拟 HAL：只对接内核
    #     mac80211_hwsim 的 scan_profile 与 /dev/hvc9|hvc10，不含任何 MTK 真机口径，
    #     故与机型无关；lineage 底座没有 IWifi HAL，缺它 WifiService 拿不到 chip）。
    #     note13 遗留的其余 14 个 HAL 子项目
    #     （audio/camera/gnss/media/radio/sensors/thermal/vibrator/light/health/
    #     board/gpu/keymint/media.v412）的配置仍是 MTK 真机口径（gpu.json=Mali-G57、
    #     chipinfo=MT6833、camera.json=Redmi 传感器），编出来会注入错二进制，
    #     故本轮不跑；按机型校准后逐个放回该清单。
    source_build.run_source_builds(dry_run=dry_run, only=SOURCE_BUILDS_STAGE1)

    os.makedirs(WORK_DIR, exist_ok=True)
    work_img = os.path.join(WORK_DIR, "vendor.work.img")
    if not dry_run:
        shutil.copy2(VENDOR_BASE, work_img)
    print(f"[ok] 基座副本: {VENDOR_BASE} -> {work_img}")

    # 注入前按注入树占用量扩展 inode/块（lineage 基座空闲 inode 极少；
    # gold /etc/selinux 替换件中镜像内不存在的新文件也占 inode）
    if not dry_run:
        _ensure_vendor_capacity(work_img,
                                _estimate_new_inodes() + gold_selinux.file_count())
    else:
        print(f"[..] (dry) 将校验 vendor inode 容量（注入树需 ~{_estimate_new_inodes()} inode）")

    # 注入树 + build.prop 渲染修正（一次 debugfs 会话）
    if dry_run:
        print(f"[..] (dry) gold /etc/selinux 替换开关={gold_selinux.ENABLED}；"
              f"将注入 vendor/patch 注入树并改 build.prop")
    else:
        # 官方 gold /etc/selinux 替换先真实落盘（单独一次 debugfs 会话）：sepolicy
        # 链在收集命令阶段读取镜像「当前」内容（同会话命令尚未执行）；若与它们
        # 同批，CDH CIL 段会叠在 lineage 策略上并成为最终写入者，替换被覆盖丢弃。
        # 默认关闭（CDH_GOLD_SELINUX=1 启用）：实测 gold 策略与本平台不兼容，
        # 启用后 boot 不完成（见 gold_selinux 模块「实测结论」）。
        if gold_selinux.ENABLED:
            if not run_command_file(work_img,
                                    gold_selinux.replace_cmds(work_img), WORK_DIR):
                print("[err] 官方 gold /etc/selinux 替换失败")
                sys.exit(1)
        else:
            print("[..] 跳过官方 gold /etc/selinux 替换（CDH_GOLD_SELINUX=1 可启用）")
        # vendor 属性上下文对齐真机（/dev/__properties__ 类型命名空间）：同样必须先
        # 真实落盘——sepolicy 链读镜像当前内容后统一追加（keymint 段会再补一条规则）。
        if not run_command_file(work_img, gold_props.prop_ctx_cmds(work_img), WORK_DIR):
            print("[err] 官方 vendor_property_contexts 对齐失败")
            sys.exit(1)
        cmds = inject.inject_cmds(work_img)
        cmds += prune.prune_cmds(work_img)
        cmds += sepolicy.patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.audio_hal_patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.keymint_hal_patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.radio_hal_patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.sensors_hal_patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.thermal_hal_patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.vibrator_hal_patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.lights_hal_patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.gnss_hal_patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.media_hal_patch_commands(work_img, WORK_DIR)
        cmds += sepolicy.wifi_hal_patch_commands(work_img, WORK_DIR)
        cmds += fstab.fstab_cmds(work_img)
        cmds += ueventd_video_cmds(work_img, WORK_DIR)
        bp_host = props.host_content(work_img, WORK_DIR)
        cmds += props.prop_cmds(work_img, bp_host)
        if not run_command_file(work_img, cmds, WORK_DIR):
            print("[err] debugfs 注入失败")
            sys.exit(1)
        print("[ok] debugfs 注入完成")

    # e2fsck 修复（rc 1 = 已修复，视为成功）
    if not dry_run:
        res = run([E2FSCK, "-fy", work_img], capture_output=True)
        if res.returncode not in (0, 1):
            print(res.stderr.decode("utf8", "replace"))
            print(f"[err] e2fsck 失败: {res.returncode}")
            sys.exit(1)
        print("[ok] e2fsck 通过")

    os.makedirs(ROM_DIST, exist_ok=True)
    if not dry_run:
        shutil.copy2(work_img, VENDOR_DIST)
    print(f"[ok] vendor.img -> {VENDOR_DIST}")
    return 0


