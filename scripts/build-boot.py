#!/usr/bin/env python3
"""oriole-16 boot.img 打包（以 lineage-23.2 UTM 底包 boot_a.img 为基座）。

ramdisk 注入 first_stage fstab.gs101：init 按 ro.hardware（=gs101，由 pack-roms 的
grub cmdline 与 build-vendor-boot.py 对齐真机）拼出 /first_stage_ramdisk/fstab.gs101；
vendor_boot fragment 内的 fstab 副本已被 build-vendor-boot.py 删除，boot ramdisk 是
一阶段 fstab 的唯一来源（内容取自 build/vendor/patch/etc/fstab.gs101，与二阶段
/vendor/etc/fstab.gs101 同源，单一来源避免两处漂移）。

kernel 段：优先用 CDH 编译产物 out/kernel/Image；未编译时沿用基底自带 kernel
（lineage 底包 kernel 即 virtio + CDH 补丁版，已验证可引导到 init），只换 ramdisk。

引导链：GRUB 以 android_bootimg_fs 把 boot 分区当文件系统挂载，读 ${boot_partition}/
kernel 与 ${boot_partition}/ramdisk.img，故 boot.img 必须是合法 Android boot 镜像，
ramdisk 为 legacy-LZ4(newc cpio)。

用法（可在任何目录）：
  python3 build/scripts/build-boot.py
  python3 build/scripts/build-boot.py --kernel-image <Image>
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from buildenv import (  # noqa: E402
    BOOT_DIST, KERNEL_OUT, LINEAGE_BOOT_IMG, VENDOR_PATCH_DIR, env, lz4_tool,
)

# boot 基底：lineage 底包 boot_a.img（CDH_LINEAGE_DIR 优先，退回 boot.img）
LINEAGE_DIR = env("CDH_LINEAGE_DIR", LINEAGE_BOOT_IMG.rsplit("/", 1)[0])
_BASE_BOOT = LINEAGE_BOOT_IMG if os.path.isfile(LINEAGE_BOOT_IMG) \
    else os.path.join(LINEAGE_DIR, "boot.img")

# Android boot header 字段偏移（magic 后：kernel_size/ramdisk_size/os_version/
# header_size/page_size/header_version；底包 page_size 字段为 0，按 4096 对齐）
BOOT_MAGIC = b"ANDROID!"
BOOT_KERNEL_SIZE_OFF = 8
BOOT_RAMDISK_SIZE_OFF = 12
BOOT_OS_VERSION_OFF = 16
BOOT_HEADER_SIZE_OFF = 20
BOOT_PAGE_SIZE = 4096

MAX_BOOT_PARTITION_BYTES = 256 * 1024 * 1024

# 注入布局对齐 cloud-droid-hub cmd/gsifix boot_firststage_fstab.go：同一份 fstab
# 同时落到根/system/etc/first_stage_ramdisk/first_stage_ramdisk/system/etc 四处。
_FSTAB_NAMES = ("fstab.gs101",)
_FSTAB_DUMP_PATHS = (
    "{name}",
    "system/etc/{name}",
    "first_stage_ramdisk/{name}",
    "first_stage_ramdisk/system/etc/{name}",
)
_FSTAB_DIR_PATHS = (
    "first_stage_ramdisk",
    "first_stage_ramdisk/system",
    "first_stage_ramdisk/system/etc",
    "system",
    "system/etc",
)

_FSTAB_SRC = os.path.join(VENDOR_PATCH_DIR, "etc", _FSTAB_NAMES[0])


def firststage_fstab():
    with open(_FSTAB_SRC, "rb") as f:
        return f.read()


def _file_ok(p):
    return os.path.isfile(p) and os.path.getsize(p) > 0


def _align_up(value, align):
    return (value + align - 1) // align * align


def _parse_boot_header(data):
    """解析 Android boot header，返回 kernel/ramdisk 段位置。"""
    if len(data) < 64 or data[:8] != BOOT_MAGIC:
        raise ValueError("非 Android boot 镜像 (magic 不匹配)")
    kernel_size = struct.unpack_from("<I", data, BOOT_KERNEL_SIZE_OFF)[0]
    ramdisk_size = struct.unpack_from("<I", data, BOOT_RAMDISK_SIZE_OFF)[0]
    header_size = struct.unpack_from("<I", data, BOOT_HEADER_SIZE_OFF)[0]
    if kernel_size <= 0 or header_size <= 0:
        raise ValueError("boot header 参数异常")
    kernel_offset = _align_up(header_size, BOOT_PAGE_SIZE)
    ramdisk_offset = _align_up(kernel_offset + kernel_size, BOOT_PAGE_SIZE)
    ramdisk_end = ramdisk_offset + ramdisk_size
    if ramdisk_end > len(data):
        raise ValueError(f"boot ramdisk 越界: {ramdisk_end} > {len(data)}")
    return {
        "kernel_size": kernel_size,
        "ramdisk_size": ramdisk_size,
        "header_size": header_size,
        "kernel_offset": kernel_offset,
        "ramdisk_offset": ramdisk_offset,
        "ramdisk_end": ramdisk_end,
    }


def _rebuild_boot(boot_data, kernel, ramdisk):
    """替换 kernel 与 ramdisk 段并重算尺寸/偏移（基底其余字段原样保留）。"""
    if not kernel:
        raise ValueError("boot kernel 为空")
    header = _parse_boot_header(boot_data)
    new_ramdisk_offset = _align_up(header["kernel_offset"] + len(kernel), BOOT_PAGE_SIZE)
    if new_ramdisk_offset + len(ramdisk) > len(boot_data):
        raise ValueError(f"boot kernel+ramdisk 超过分区大小: "
                         f"{new_ramdisk_offset + len(ramdisk)} > {len(boot_data)}")
    rebuilt = bytearray(boot_data)
    struct.pack_into("<I", rebuilt, BOOT_KERNEL_SIZE_OFF, len(kernel))
    struct.pack_into("<I", rebuilt, BOOT_RAMDISK_SIZE_OFF, len(ramdisk))
    rebuilt[header["kernel_offset"]:] = b"\x00" * (len(rebuilt) - header["kernel_offset"])
    rebuilt[header["kernel_offset"]:header["kernel_offset"] + len(kernel)] = kernel
    rebuilt[new_ramdisk_offset:new_ramdisk_offset + len(ramdisk)] = ramdisk
    return bytes(rebuilt)


def _atomic_write(path, data):
    """临时文件写入后原子替换。"""
    tmp = path + ".new.{pid}.{rand}".format(pid=os.getpid(), rand=os.urandom(4).hex())
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


# ---------- first_stage fstab 注入（镜像 cmd/gsifix boot_firststage_fstab.go） ----------

def _align4(v):
    return (v + 3) & ~3


def _newc_hex(value):
    return b"%08x" % value


def _read_newc_entries(data):
    """解析 newc cpio，返回 [(name, header, name_raw, payload)]。"""
    entries = []
    off = 0
    n = len(data)
    while off + 110 <= n:
        hdr = data[off:off + 110]
        if hdr[:6] not in (b"070701", b"070702"):
            raise ValueError("cpio magic 错误: %r" % hdr[:6])
        size = int(hdr[54:62], 16)
        namesize = int(hdr[94:102], 16)
        if namesize == 0:
            raise ValueError("cpio namesize 为 0")
        name_start = off + 110
        name_end = name_start + namesize
        if name_end > n:
            raise ValueError("cpio name 越界")
        name_raw = data[name_start:name_end]
        name = name_raw.rstrip(b"\x00").decode("utf-8", errors="replace")
        data_start = _align4(name_end)
        data_end = data_start + size
        payload = data[data_start:data_end]
        entries.append([name, hdr, name_raw, payload])
        off = _align4(data_end)
        if name == "TRAILER!!!":
            break
    return entries


def _patch_fstab_cpio(archive):
    """把 fstab.gs101 注入 newc ramdisk（同名项先删后加），返回 (新 cpio, 新增项)。"""
    data = firststage_fstab()
    targets = {}
    for name in _FSTAB_NAMES:
        for path in _FSTAB_DUMP_PATHS:
            targets[path.format(name=name)] = data
    for d in _FSTAB_DIR_PATHS:
        targets.setdefault(d, None)

    entries = _read_newc_entries(archive)
    trailer = None
    body = []
    for entry in entries:
        if entry[0] == "TRAILER!!!":
            trailer = entry
            continue
        if entry[0] in targets:
            continue
        body.append(entry)
    if trailer is None:
        raise ValueError("cpio 缺少 TRAILER!!!")

    existing = {e[0] for e in body}
    inode = {"v": 100000}
    added = []
    raw_out = bytearray()

    # newc 对齐是"绝对偏移 mod 4"（110 非 4 倍数），须按 raw_out 累计长度对齐。
    def pad4():
        nonlocal raw_out
        raw_out += b"\x00" * ((4 - len(raw_out) % 4) % 4)

    for _name, hdr, name_raw, payload in body:
        raw_out += hdr + name_raw
        pad4()
        raw_out += payload
        pad4()

    def append_entry(name, mode, payload):
        nonlocal raw_out
        inode["v"] += 1
        meta = (
            b"070701"            # magic
            + _newc_hex(inode["v"])     # ino
            + _newc_hex(mode)           # mode
            + _newc_hex(0)              # uid
            + _newc_hex(0)              # gid
            + _newc_hex(1)              # nlink
            + _newc_hex(0)              # mtime
            + _newc_hex(len(payload))   # filesize
            + _newc_hex(0)              # devmajor
            + _newc_hex(0)              # devminor
            + _newc_hex(0)              # rdevmajor
            + _newc_hex(0)              # rdevminor
            + _newc_hex(len(name) + 1)  # namesize
            + _newc_hex(0)              # check
        )
        raw_out += meta
        raw_out += name.encode("latin1") + b"\x00"
        pad4()
        raw_out += payload
        pad4()

    for d in _FSTAB_DIR_PATHS:
        if d in existing:
            continue
        append_entry(d, 0o040755, b"")
        added.append(d)
    for name in _FSTAB_NAMES:
        for path in _FSTAB_DUMP_PATHS:
            p = path.format(name=name)
            append_entry(p, 0o100644, data)
            added.append(p)
    raw_out += trailer[1] + trailer[2]
    pad4()
    raw_out += trailer[3]
    pad4()
    return bytes(raw_out), added


def _inject_fstab_lz4(ramdisk_lz4):
    """legacy-LZ4(newc cpio) ramdisk 解压 -> 注入 fstab -> 重打包 legacy-LZ4。"""
    lz4 = lz4_tool()
    work = tempfile.mkdtemp(prefix="cdh-boot-fstab.")
    try:
        in_lz4 = os.path.join(work, "rd.in.lz4")
        cpio = os.path.join(work, "rd.cpio")
        out_lz4 = os.path.join(work, "rd.out.lz4")
        with open(in_lz4, "wb") as f:
            f.write(ramdisk_lz4)
        subprocess.run([lz4, "-d", "-f", in_lz4, cpio], check=True, capture_output=True)
        patched, added = _patch_fstab_cpio(open(cpio, "rb").read())
        subprocess.run([lz4, "-12", "-l", "-f", "-", out_lz4],
                       input=patched, check=True, capture_output=True)
        return open(out_lz4, "rb").read(), added
    finally:
        shutil.rmtree(work, ignore_errors=True)


def build_boot(kernel_path):
    """基底 boot_a.img 换 ramdisk（注入 fstab.gs101），可选换 kernel，写 BOOT_DIST。"""
    if not os.path.isfile(_BASE_BOOT):
        print(f"[err] 缺少 lineage boot 基底: {_BASE_BOOT}（先 unpack lineage 底包）")
        return 1
    if not os.path.isfile(_FSTAB_SRC):
        print(f"[err] 缺少 first_stage fstab: {_FSTAB_SRC}")
        return 1
    boot_data = open(_BASE_BOOT, "rb").read()
    if len(boot_data) > MAX_BOOT_PARTITION_BYTES:
        print(f"[err] boot.img 超过分区上限: {len(boot_data)} B")
        return 1
    header = _parse_boot_header(boot_data)
    if not header["ramdisk_size"]:
        print(f"[err] 基底 boot 无内嵌 ramdisk（fstab 无处注入）: {_BASE_BOOT}")
        return 1

    if _file_ok(kernel_path):
        kernel = open(kernel_path, "rb").read()
        kernel_desc = f"CDH 编译 Image ({len(kernel)} B)"
    else:
        kernel = boot_data[header["kernel_offset"]:header["ramdisk_offset"]]
        kernel_desc = (f"沿用基底 kernel ({len(kernel)} B)"
                       f"，缺 {kernel_path}（kernel 阶段未产出）")

    ramdisk, added = _inject_fstab_lz4(
        boot_data[header["ramdisk_offset"]:header["ramdisk_end"]])
    rebuilt = _rebuild_boot(boot_data, kernel, ramdisk)
    os.makedirs(os.path.dirname(BOOT_DIST), exist_ok=True)
    _atomic_write(BOOT_DIST, rebuilt)
    os_version = struct.unpack_from("<I", rebuilt, BOOT_OS_VERSION_OFF)[0]
    print(f"[ok] boot.img -> {BOOT_DIST}")
    print(f"    基底 {os.path.basename(_BASE_BOOT)}: kernel {kernel_desc}")
    print(f"    ramdisk {header['ramdisk_size']} B -> {len(ramdisk)} B"
          f"（注入 {_FSTAB_NAMES[0]} 共 {len(added)} 项）")
    print(f"    os_version=0x{os_version:x}（保留基底值）")
    return 0


def main():
    ap = argparse.ArgumentParser(description="oriole-16 boot.img 打包")
    ap.add_argument("--kernel-image", default=None,
                    help="打包用的 Image 路径（默认 out/kernel/Image）")
    args = ap.parse_args()
    kernel = args.kernel_image or os.path.join(KERNEL_OUT, "Image")
    sys.exit(build_boot(kernel))


if __name__ == "__main__":
    main()
