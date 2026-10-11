#!/usr/bin/env python3
"""解压 oriole-16（Pixel 6 / Android 16）所需 ROM 到各自目录。

两个任务（按需可选）：
  * factory 官方固件：从外层固件取出内层 image-*.zip，把官方镜像平铺解到
    roms/image-oriole-<版本>/（system/vendor/product 等 *.img，CDH_FACTORY_DIR）
  * LineageOS UTM 虚机：从 zip 里直接取出 vda.img，解析其 GPT 分区表并按偏移切出
    各分区镜像 -> lineage-rom/lineage-23/<分区名>.img（如 super.img、boot.img），
    并进一步用内置 lpunpack 拆出 super.img 内 system/vendor/product 等逻辑分区；

用法（可在任何目录）：
  python3 build/unpack-roms.py                # 两个任务都跑
  python3 build/unpack-roms.py factory        # 只解 factory
  python3 build/unpack-roms.py lineage        # 只处理 lineage
  python3 build/unpack-roms.py lineage --list # 只打印 lineage 分区表
"""

import glob
import os
import shutil
import struct
import subprocess
import sys
import zipfile

from buildenv import env

SECTOR = 512
TMP_DIR = "/Users/cuimingqiang/workspace/cloud-phone-hub-ws/out/tmp/unpack-roms"
GPT_SIGNATURE = b"EFI PART"

# Android 动态分区 (super) metadata 常量，来源 AOSP liblp/metadata_format.h
LP_METADATA_GEOMETRY_MAGIC = 0x616c4467  # "gDla"
LP_METADATA_HEADER_MAGIC = 0x414C5030    # "0PLA"
LP_METADATA_GEOMETRY_SIZE = 4096
LP_PARTITION_RESERVED_BYTES = 4096
LP_SECTOR_SIZE = 512
LP_PARTITION_ATTR_SLOT_SUFFIXED = 1 << 1
LP_PARTITION_ATTR_DISABLED = 1 << 3
SPARSE_MAGIC = 0xed26ff3a
# 定位 metadata slot0：保留区(4096) + geometry(4096) + geometry 备份(4096)
METADATA_OFFSET = LP_PARTITION_RESERVED_BYTES + LP_METADATA_GEOMETRY_SIZE * 2

# 官方固件 zip 与解压目录、Lineage 底包 zip 与解压目录：
# main.py --build-id 选中 manifest.json 条目后经 CDH_* 环境变量注入，
# 独立运行时回退当前唯一 ROM（与 build/baselines/<build-id>.json 同源）。
FACTORY_ZIP = env(
    "CDH_FACTORY_ZIP",
    "/Users/cuimingqiang/workspace/cloud-phone-hub-ws/devices/google/oriole-16/roms/"
    "oriole-cp1a.260405.005-factory-8081a8ba.zip",
)
# 官方镜像平铺目录（system/vendor/product 等 *.img 直接落此，下游基线件来源）
FACTORY_DIR = env(
    "CDH_FACTORY_DIR",
    "/Users/cuimingqiang/workspace/cloud-phone-hub-ws/devices/google/oriole-16/roms/"
    "image-oriole-cp1a.260405.005",
)

LINEAGE_ZIP = env(
    "CDH_LINEAGE_ZIP",
    "/Users/cuimingqiang/workspace/cloud-phone-hub-ws/lineage-rom/"
    "UTM-VM-lineage-23.2-20260922-UNOFFICIAL-virtio_arm64only.zip",
)
LINEAGE_VDA_MEMBER = "LineageOS_on_arm64.utm/Data/vda.img"  # zip 内的成员路径
LINEAGE_OUT_DIR = env(
    "CDH_LINEAGE_DIR", "/Users/cuimingqiang/workspace/cloud-phone-hub-ws/lineage-rom/lineage-23"
)


class Partition:
    def __init__(self, name, first_lba, last_lba, type_guid):
        self.name = name
        self.first_lba = first_lba
        self.last_lba = last_lba
        self.type_guid = type_guid

    @property
    def size_bytes(self):
        return (self.last_lba - self.first_lba + 1) * SECTOR


# ---------------- factory ----------------

def unpack_zip(zip_path, out_dir):
    """把 zip_path 解压到 out_dir（带 zip-slip 防护），返回是否成功。"""
    if not os.path.isfile(zip_path):
        print(f"[skip] 不存在: {zip_path}")
        return False
    os.makedirs(out_dir, exist_ok=True)
    real_dest = os.path.abspath(out_dir)
    with zipfile.ZipFile(zip_path) as zf:
        members = zf.infolist()
        for i, m in enumerate(members, 1):
            target = os.path.join(real_dest, m.filename)
            if not os.path.abspath(target).startswith(real_dest + os.sep):
                print(f"[warn] 跳过非法路径: {m.filename}")
                continue
            zf.extract(m, out_dir)
            if i == 1 or i == len(members) or i % 500 == 0:
                print(f"  [{i}/{len(members)}] {m.filename}")
    print(f"[ok] 解压 {os.path.basename(zip_path)} -> {out_dir}")
    return True


def _find_inner_image_zip():
    """定位内层 image-*.zip：优先 roms/ 下已解出的外层目录，否则解外层 zip 到临时目录。

    返回 (内层 zip 路径或 None, 需清理的临时目录或 None)。
    """
    parent = os.path.dirname(FACTORY_ZIP)
    hits = sorted(glob.glob(os.path.join(parent, "*", "image-*.zip")))
    if hits:
        return hits[0], None
    tmp_root = os.path.join(TMP_DIR, "factory")
    shutil.rmtree(tmp_root, ignore_errors=True)
    unpack_zip(FACTORY_ZIP, tmp_root)
    hits = sorted(glob.glob(os.path.join(tmp_root, "**", "image-*.zip"), recursive=True))
    return (hits[0] if hits else None), tmp_root


def unpack_factory():
    """把内层 image-*.zip 的官方镜像平铺解到 roms/image-oriole-<版本>/（CDH_FACTORY_DIR）。

    契约：CDH_FACTORY_DIR 即平铺镜像目录（下游 vendor/vendor_dlkm 基线件来源），
    故此处直接平铺落盘，不再嵌套内层 zip 名目录。
    """
    if glob.glob(os.path.join(FACTORY_DIR, "*.img")):
        print(f"[skip] factory 已解出: {FACTORY_DIR}")
        return
    if not os.path.isfile(FACTORY_ZIP):
        print(f"[skip] 不存在: {FACTORY_ZIP}")
        return
    inner, tmp_root = _find_inner_image_zip()
    if not inner:
        print("[warn] 未找到内层 image-*.zip")
        return
    try:
        print(f"  内层 {os.path.basename(inner)} -> {os.path.relpath(FACTORY_DIR)}")
        unpack_zip(inner, FACTORY_DIR)
    finally:
        if tmp_root:
            shutil.rmtree(tmp_root, ignore_errors=True)


# ---------------- lineage ----------------

def unpack_vda_from_zip(zip_path, member, tmp_vda):
    """把 zip 内的 vda.img 解压到临时文件，返回是否成功。"""
    if not os.path.isfile(zip_path):
        print(f"[skip] 不存在: {zip_path}")
        return False
    os.makedirs(os.path.dirname(tmp_vda), exist_ok=True)
    with zipfile.ZipFile(zip_path) as zf:
        if member not in zf.namelist():
            raise SystemExit(f"[err] zip 中未找到成员 {member}")
        print(f"  提取 {member} -> 临时 {tmp_vda}")
        with zf.open(member) as src, open(tmp_vda, "wb") as dst:
            _copy_stream(src, dst)
    return True


def read_partitions(img_path):
    with open(img_path, "rb") as f:
        hdr = f.read(SECTOR * 2)[SECTOR:SECTOR * 2]
        if hdr[0:8] != GPT_SIGNATURE:
            raise SystemExit(f"[err] 不是合法 GPT 磁盘镜像: {img_path}")
        part_lba, num_entries, entry_size = struct.unpack("<QII", hdr[72:72 + 16])
        if num_entries == 0 or entry_size == 0 or entry_size > 4096:
            raise SystemExit("[err] GPT 分区表字段异常")
        f.seek(part_lba * SECTOR)
        entries = f.read(num_entries * entry_size)
    parts = []
    for i in range(num_entries):
        e = entries[i * entry_size:(i + 1) * entry_size]
        if len(e) < entry_size:
            break
        type_guid, _uniq, first, last = struct.unpack("<16s16sQQ", e[:48])
        if first == 0 and last == 0:
            continue
        name = e[56:56 + 72].decode("utf-16-le", "ignore").rstrip("\x00 \t")
        parts.append(Partition(name or f"p{i+1}", first, last, type_guid.hex()))
    return parts


def export_partitions(img_path, parts, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    exported = 0
    with open(img_path, "rb") as src:
        for p in parts:
            out = os.path.join(out_dir, f"{p.name}.img")
            print(f"  导出 {p.name} ({p.size_bytes/2**30:.2f} GiB) -> {os.path.basename(out)}")
            src.seek(p.first_lba * SECTOR)
            with open(out, "wb") as dst:
                _copy(src, dst, p.size_bytes)
            exported += 1
    return exported


def _read_table(f, hdr, offset, desc):
    """读 metadata 某张表 (off,num,entry) 的原始字节，off 相对 header 末尾。"""
    off, num, entry_size = desc
    header_size = struct.unpack_from("<I", hdr, 8)[0]
    f.seek(METADATA_OFFSET + header_size + off)
    return f.read(num * entry_size), num, entry_size


def unpack_super(super_path, out_dir):
    """Python 版 lpunpack：从 super 动态分区拆出各逻辑分区镜像到 out_dir。

    逻辑分区以 sparse 或 raw 形式存在，sparse 用系统 simg2img 转换。
    """
    if not os.path.isfile(super_path):
        print(f"[skip] 不存在: {super_path}")
        return 0
    os.makedirs(out_dir, exist_ok=True)
    with open(super_path, "rb") as f:
        f.seek(METADATA_OFFSET)
        hdr = f.read(256)
        magic = struct.unpack_from("<I", hdr, 0)[0]
        if magic != LP_METADATA_HEADER_MAGIC:
            raise SystemExit(f"[err] {super_path} 不是合法 super 动态分区镜像")

        pdata, pnum, pesz = _read_table(f, hdr, 0, struct.unpack_from("<III", hdr, 80))
        edata, en, eesz = _read_table(f, hdr, 0, struct.unpack_from("<III", hdr, 92))
        bdata, bn, besz = _read_table(f, hdr, 0, struct.unpack_from("<III", hdr, 116))

        exported = 0
        for i in range(pnum):
            e = pdata[i * pesz:(i + 1) * pesz]
            name = e[0:36].split(b"\x00", 1)[0].decode("ascii", "ignore")
            attrs = struct.unpack_from("<I", e, 36)[0]
            first_ext, num_ext = struct.unpack_from("<II", e, 40)
            if not name or (attrs & LP_PARTITION_ATTR_DISABLED):
                continue
            if (attrs & LP_PARTITION_ATTR_SLOT_SUFFIXED) and not name.endswith(("_a", "_b")):
                name += "_a"  # slot0
            out = os.path.join(out_dir, f"{name}.img")

            # 汇总各 extent，得到文件偏移 → 长度 段，逐段从 super 读出拼成分区镜像
            segs = []
            total = 0
            for k in range(num_ext):
                et = edata[(first_ext + k) * eesz:(first_ext + k + 1) * eesz]
                nsectors, ttype, tdata, tsource = struct.unpack_from("<QIQI", et, 0)
                if ttype != 0:  # LP_TARGET_TYPE_LINEAR
                    continue
                seg = (tdata * LP_SECTOR_SIZE, nsectors * LP_SECTOR_SIZE)
                segs.append(seg)
                total += seg[1]
            print(f"  逻辑分区 {name} ({total/2**30:.2f} GiB, {num_ext} 段) -> {name}.img")
            if not segs:
                continue
            with open(out, "wb") as dst:
                for soff, slen in segs:
                    f.seek(soff)
                    remaining = slen
                    while remaining > 0:
                        buf = f.read(min(16 * 1024 * 1024, remaining))
                        if not buf:
                            raise SystemExit("[err] super 读取越界")
                        dst.write(buf)
                        remaining -= len(buf)
            # sparse -> raw
            if _is_sparse(out):
                print(f"    (sparse) simg2img 转换 {os.path.basename(out)}")
                _unsparse(out)
            exported += 1
    return exported


def _is_sparse(path):
    with open(path, "rb") as f:
        return struct.unpack("<I", f.read(4))[0] == SPARSE_MAGIC


def _unsparse(path):
    """用系统 simg2img 把文件就地由 sparse 转为 raw。"""
    simg2img = shutil.which("simg2img")
    if not simg2img:
        print("    [warn] 本机无 simg2img，保留 sparse 格式。")
        return
    tmp = path + ".raw"
    subprocess.run([simg2img, path, tmp], check=True)
    os.replace(tmp, path)


def unpack_lineage(only_list=False):
    tmp_vda = os.path.join(TMP_DIR, "lineage", "vda.img")
    try:
        if not only_list and glob.glob(os.path.join(LINEAGE_OUT_DIR, "super", "*.img")):
            print(f"[skip] lineage 已解出: {LINEAGE_OUT_DIR}")
            return
        if not unpack_vda_from_zip(LINEAGE_ZIP, LINEAGE_VDA_MEMBER, tmp_vda):
            return
        parts = read_partitions(tmp_vda)
        if only_list:
            print(f"分区表 (lineage):")
            for p in parts:
                print(f"  {p.name:<24} {p.size_bytes/2**30:8.2f} GiB  LBA {p.first_lba}..{p.last_lba}")
            print(f"\n共 {len(parts)} 个分区")
            return
        print(f"解析分区表完成，{len(parts)} 个分区 -> {LINEAGE_OUT_DIR}")
        n = export_partitions(tmp_vda, parts, LINEAGE_OUT_DIR)
        print(f"[ok] lineage 分区已导出 {n} 个到 {LINEAGE_OUT_DIR}")
        super_img = os.path.join(LINEAGE_OUT_DIR, "super.img")
        if os.path.isfile(super_img):
            super_out = os.path.join(LINEAGE_OUT_DIR, "super")
            print(f"进一步解压 super.img (动态分区) -> {super_out}")
            m = unpack_super(super_img, super_out)
            print(f"[ok] super 拆出 {m} 个逻辑分区到 {super_out}")
    finally:
        shutil.rmtree(os.path.dirname(tmp_vda), ignore_errors=True)


def _copy(src, dst, length):
    remaining = length
    chunk = 16 * 1024 * 1024
    while remaining > 0:
        buf = src.read(min(chunk, remaining))
        if not buf:
            raise SystemExit("[err] 镜像读取异常，分区越界")
        dst.write(buf)
        remaining -= len(buf)


def _copy_stream(src, dst):
    chunk = 16 * 1024 * 1024
    while True:
        buf = src.read(chunk)
        if not buf:
            break
        dst.write(buf)


def export_gold_vendor_selinux():
    """把官方 vendor.img 的 /etc/selinux 解到 ROM_ROOT_DIR/vendor_a/etc/selinux/。

    下游 gold_props（属性上下文对齐真机）与 vendor 阶段的 sepolicy 规则读取的是
    **解出的目录路径** `<factory-dir>/vendor_a/etc/selinux/…`（note13/popsicle 约定：
    官方 super 解包后逻辑分区摊平成 <name>_a/ 目录）。Pixel 官方固件没有 super.img，
    逻辑分区是独立的 ext4 镜像，故这里用 debugfs 逐件 dump 补齐该约定。
    官方 vendor 是 ext4（实测 offset 1080 处 0xEF53），macOS 侧 debugfs 可直接读。
    不用 rdump：它会把源目录本身再套一层（<dest>/selinux/…），且要求目标父目录就位。
    """
    from buildenv import ROM_ROOT_DIR, e2fs_tools

    debugfs = e2fs_tools()[0]
    vendor_img = os.path.join(FACTORY_DIR, "vendor.img")
    dest = os.path.join(ROM_ROOT_DIR, "vendor_a", "etc", "selinux")
    if not os.path.isfile(vendor_img):
        print(f"[skip] 官方 vendor.img 不存在: {vendor_img}")
        return
    listing = subprocess.run([debugfs, "-R", "ls -l /etc/selinux", vendor_img],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    names = []
    for raw in listing.stdout.decode("utf8", "replace").splitlines():
        fields = raw.split()
        if len(fields) < 2:
            continue
        name = fields[-1]
        if name in (".", "..") or "(" in name:
            continue
        names.append(name)
    if not names:
        raise SystemExit(f"[err] 官方 vendor 内读不到 /etc/selinux: {vendor_img}")
    os.makedirs(dest, exist_ok=True)
    missing = [n for n in names if not os.path.isfile(os.path.join(dest, n))]
    for name in missing:
        res = subprocess.run([debugfs, "-R", f"dump /etc/selinux/{name} "
                              f"{os.path.join(dest, name)}", vendor_img],
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if res.returncode != 0 or not os.path.isfile(os.path.join(dest, name)):
            print(res.stderr.decode("utf8", "replace"))
            raise SystemExit(f"[err] 官方 vendor selinux 提取失败: /etc/selinux/{name}")
    print(f"[ok] gold vendor selinux -> {dest}（共 {len(names)} 件，本次补 {len(missing)} 件）")


def main():
    argv = sys.argv[1:]
    only_list = "--list" in argv
    keys = [a.lower() for a in argv if a != "--list"]

    if not keys:
        keys = ["factory", "lineage"]

    todo = [t for t in ("factory", "lineage") if t in keys]

    if not todo:
        print("未匹配到任务，可用关键字: factory / lineage")
        return 1

    if "factory" in todo:
        print("=== factory 固件 ===")
        unpack_factory()
        export_gold_vendor_selinux()
    if "lineage" in todo:
        print("=== LineageOS (vda.img -> 分区镜像) ===")
        unpack_lineage(only_list=only_list)
    return 0


if __name__ == "__main__":
    sys.exit(main())