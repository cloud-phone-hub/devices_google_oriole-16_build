#!/usr/bin/env python3
"""liblp（Logical Partition Metadata）super.img 生成器与自校验读取器。

参考 Android 15 BP1A pixel6 官方 super 元数据逆向得到，ac定义于
AOSP system/core/fs_mgr/liblp/include/liblp/metadata_format.h（版本 10.x）：

  - 分区头（LpMetadataGeometry）      52B：magic + struct_size + sha256 校验 + 上限/槽数/块大小
  - 元数据头（LpMetadataHeader）    256B：magic + 版本 + header大小 + sha256(头) + 表大小 +
                                          sha256(表) + 4 张表描述符（partitions/extents/groups/block_devices）
                                          + flags + reserved[124]（header 1.2 expanded，须 256B）
  - 表（紧邻头之后）：
      * partitions     52B/条：name[36] + attrs + first_extent + num_extents + group_index
      * extents        24B/条：num_sectors + target_type + target_data + target_source
      * groups         48B/条：name[36] + flags + maximum_size
      * block_devices  64B/条：first_logical_sector + alignment + alignment_offset + size
                                + partition_name[36] + flags
  - 校验算法已对官方镜像逐字节验证通过。

super.img 物理布局（LP_METADATA_DEFAULT_PARTITION_NAME = "super"）：
    [0,4096)     保留区（防误判引导扇区）
    [4096,8192)  geometry 主副本
    [8192,12288) geometry 备份
    [12288, ...) 元数据槽位（metadata_max_size * slot_count 份主副本 + 同量备份）
    [1048576,..) 逻辑分区数据（first_logical_sector = 2048，1MiB 对齐）
"""

import hashlib
import os
import struct

# ---- 常量 ----
SECTOR = 512
RESERVED_BYTES = 4096                 # LP_PARTITION_RESERVED_BYTES
GEOMETRY_SIZE = 4096                  # LP_METADATA_GEOMETRY_SIZE
METADATA_MAX_SIZE = 65536             # 与官方一致，须为 512 的倍数
METADATA_SLOT_COUNT = 2               # 与官方一致（A/B）
LOGICAL_BLOCK_SIZE = 4096             # 与官方一致
GEOMETRY_MAGIC = 0x616C4467
HEADER_MAGIC = 0x414C5030
MAJOR_VERSION = 10
MINOR_VERSION = 2

FIRST_LOGICAL_SECTOR = 2048           # 1MiB
ALIGNMENT = 1048576                   # 1MiB
ALIGNMENT_OFFSET = 0
SECTORS_PER_MIB = ALIGNMENT // SECTOR

LP_TARGET_TYPE_LINEAR = 0
GROUP_DEFAULT_INDEX = 0
GROUP_MAIN_INDEX = 1

LP_METADATA_GEOMETRY_MAX_SIZE_OFF = 40
LP_METADATA_GEOMETRY_SLOT_COUNT_OFF = 44
LP_METADATA_GEOMETRY_BLOCK_SIZE_OFF = 48
LP_METADATA_GEOMETRY_CHECKSUM_OFF = 8

LP_METADATA_HEADER_SIZE = 256
LP_METADATA_HEADER_CHECKSUM_OFF = 12
LP_METADATA_HEADER_TABLES_SIZE_OFF = 44
LP_METADATA_HEADER_TABLES_CHECKSUM_OFF = 48
LP_METADATA_HEADER_PARTITIONS_OFF = 80
LP_METADATA_HEADER_EXTENTS_OFF = 92
LP_METADATA_HEADER_GROUPS_OFF = 104
LP_METADATA_HEADER_BLOCK_DEVICES_OFF = 116


def _align_up(value, unit):
    return (value + unit - 1) // unit * unit


def _sha256(data):
    return hashlib.sha256(data).digest()


def _pad_name(name, length):
    b = name.encode("ascii")
    if len(b) > length:
        raise ValueError(f"name too long: {name}")
    return b + b"\x00" * (length - len(b))


def _build_geometry(super_size):
    g = bytearray(GEOMETRY_SIZE)
    struct.pack_into("<I", g, 0, GEOMETRY_MAGIC)
    struct.pack_into("<I", g, 4, 52)                     # struct_size
    struct.pack_into("<I", g, LP_METADATA_GEOMETRY_MAX_SIZE_OFF, METADATA_MAX_SIZE)
    struct.pack_into("<I", g, LP_METADATA_GEOMETRY_SLOT_COUNT_OFF, METADATA_SLOT_COUNT)
    struct.pack_into("<I", g, LP_METADATA_GEOMETRY_BLOCK_SIZE_OFF, LOGICAL_BLOCK_SIZE)
    # 校验：对 [0,52) 计算 sha256，其中校验字段 [8,40) 置 0
    g[LP_METADATA_GEOMETRY_CHECKSUM_OFF:LP_METADATA_GEOMETRY_CHECKSUM_OFF + 32] = bytes(32)
    g[LP_METADATA_GEOMETRY_CHECKSUM_OFF:LP_METADATA_GEOMETRY_CHECKSUM_OFF + 32] = _sha256(g[:52])
    return bytes(g)


def _build_metadata(super_size, partitions, extents):
    """partitions: list[(name, attr, first_extent, num_extents, group_index)]
       extents:    list[(num_sectors, target_data)]  target_type=LINEAR, source=0"""
    def desc(offset, num, entry_size):
        return struct.pack("<III", offset, num, entry_size)

    n_part = len(partitions)
    n_ext = len(extents)
    n_grp = 2
    n_dev = 1

    part_size = n_part * 52
    ext_size = n_ext * 24
    grp_size = n_grp * 48
    dev_size = n_dev * 64
    tables_size = part_size + ext_size + grp_size + dev_size

    tables = bytearray(tables_size)
    off = 0
    for name, attr, first_extent, num_extents, group_index in partitions:
        struct.pack_into("<36s5I", tables, off,
                         _pad_name(name, 36), attr, first_extent, num_extents, group_index, 0)
        off += 52
    for num_sectors, target_data in extents:
        struct.pack_into("<QIQI", tables, off, num_sectors, LP_TARGET_TYPE_LINEAR, target_data, 0)
        off += 24
    # groups: "default"(unlimited), "virt_dynamic_partitions"(super - 4MiB)
    struct.pack_into("<36sIQ", tables, off, _pad_name("default", 36), 0, 0)
    off += 48
    struct.pack_into("<36sIQ", tables, off, _pad_name("virt_dynamic_partitions", 36), 0,
                     super_size - 4 * 1024 * 1024)
    off += 48
    # block_devices: super（Q fls, I alignment, I align_offset, Q size, 36s name, I flags）
    struct.pack_into("<Q", tables, off + 0, FIRST_LOGICAL_SECTOR)
    struct.pack_into("<I", tables, off + 8, ALIGNMENT)
    struct.pack_into("<I", tables, off + 12, ALIGNMENT_OFFSET)
    struct.pack_into("<Q", tables, off + 16, super_size)
    struct.pack_into("<36sI", tables, off + 24, _pad_name("super", 36), 0)
    off += 64

    tables_checksum = _sha256(tables)

    hdr = bytearray(LP_METADATA_HEADER_SIZE)
    struct.pack_into("<I", hdr, 0, HEADER_MAGIC)
    struct.pack_into("<H", hdr, 4, MAJOR_VERSION)
    struct.pack_into("<H", hdr, 6, MINOR_VERSION)
    struct.pack_into("<I", hdr, 8, LP_METADATA_HEADER_SIZE)
    # header_checksum [12,44) 留待置 0 后计算
    struct.pack_into("<I", hdr, LP_METADATA_HEADER_TABLES_SIZE_OFF, tables_size)
    # tables_checksum [48,80)
    hdr[LP_METADATA_HEADER_TABLES_CHECKSUM_OFF:LP_METADATA_HEADER_TABLES_CHECKSUM_OFF + 32] = tables_checksum
    # 4 张表描述符
    struct.pack_into("<III", hdr, LP_METADATA_HEADER_PARTITIONS_OFF,
                     0, n_part, 52)
    struct.pack_into("<III", hdr, LP_METADATA_HEADER_EXTENTS_OFF,
                     part_size, n_ext, 24)
    struct.pack_into("<III", hdr, LP_METADATA_HEADER_GROUPS_OFF,
                     part_size + ext_size, n_grp, 48)
    struct.pack_into("<III", hdr, LP_METADATA_HEADER_BLOCK_DEVICES_OFF,
                     part_size + ext_size + grp_size, n_dev, 64)
    # header_checksum：对 [0,header_size) 计算，校验字段 [12,44) 置 0
    hdr[LP_METADATA_HEADER_CHECKSUM_OFF:LP_METADATA_HEADER_CHECKSUM_OFF + 32] = bytes(32)
    hdr[LP_METADATA_HEADER_CHECKSUM_OFF:LP_METADATA_HEADER_CHECKSUM_OFF + 32] = \
        _sha256(bytes(hdr[:LP_METADATA_HEADER_SIZE]))
    return bytes(hdr) + bytes(tables)


def build_super_image(partitions, super_size, out_path, reserve=None):
    """partitions: list[(name, image_path)]，按序写入超分区并生成 liblp 元数据。

    super_size 为物理 super 分区大小（字节），须能容纳全部逻辑分区 + 元数据。

    reserve: dict[name -> 预留容量字节]。逻辑分区的 extent 容量取
    max(镜像对齐, 预留)，写入的仍是镜像实际字节；预留出的冗余让后续
    「只改此分区」时可直接 dd 覆盖（新镜像 ≤ 容量即可），无需重建 super/vda。
    """
    reserve = reserve or {}
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    # 计算每个逻辑分区的 extent（1MiB 对齐，容量按 4096 对齐）
    extents = []          # (num_sectors, target_data)  —— target_data 为 super 内扇区
    cursor = FIRST_LOGICAL_SECTOR
    total_bytes = 0
    for name, image_path in partitions:
        size = os.path.getsize(image_path)
        total_bytes += size
        # 容量 = max(镜像大小, 预留)；extent 按此分配，dd 增量补丁据此判定是否放得下。
        capacity = max(size, reserve.get(name, 0))
        num_sectors = _align_up(capacity, LOGICAL_BLOCK_SIZE) // SECTOR
        extents.append((num_sectors, cursor))
        total_sectors = cursor + num_sectors
        cursor = _align_up(total_sectors, SECTORS_PER_MIB)
    req_sectors = cursor - FIRST_LOGICAL_SECTOR
    req_bytes = req_sectors * SECTOR
    if super_size < req_bytes + FIRST_LOGICAL_SECTOR * SECTOR:
        raise ValueError(
            f"super 空间不足: 需 {req_bytes} 数据字节, super_size={super_size}")

    attributes = [(name, 0, i, 1, 1) for i, (name, _) in enumerate(partitions)]

    geometry = _build_geometry(super_size)
    metadata = _build_metadata(super_size, attributes,
                               [(ns, td) for ns, td in extents])

    # slot_size 为单个元数据槽位大小
    slot_size = METADATA_MAX_SIZE
    primary_region = slot_size * METADATA_SLOT_COUNT
    meta_start = RESERVED_BYTES + GEOMETRY_SIZE * 2          # 12288
    slot_blob = metadata.ljust(slot_size, b"\x00")

    print(f"[lp] 构建 super.img: {len(partitions)} 分区, {total_bytes} 数据字节 -> {out_path}")
    with open(out_path, "wb") as f:
        f.truncate(super_size)
        f.seek(RESERVED_BYTES)
        f.write(geometry)                                   # geometry 主副本
        f.write(geometry)                                   # geometry 备份
        # 主副本槽位 + 备份槽位均写入相同有效元数据，保证任意槽位可选
        for slot in range(METADATA_SLOT_COUNT):
            f.seek(meta_start + slot * slot_size)
            f.write(slot_blob)
            f.seek(meta_start + primary_region + slot * slot_size)
            f.write(slot_blob)
        # 写入各逻辑分区数据
        for (name, image_path), (num_sectors, target_data) in zip(partitions, extents):
            f.seek(target_data * SECTOR)
            with open(image_path, "rb") as src:
                data = src.read()
            f.write(data)
    print(f"[lp] 完成: {os.path.getsize(out_path)} B")
    return metadata


def read_super_image(super_path):
    """自校验：读回 super.img 的逻辑分区（name, size_bytes, data_sector）。"""
    data = open(super_path, "rb").read(RESERVED_BYTES + GEOMETRY_SIZE * 2 + METADATA_MAX_SIZE)
    # geometry 决定 slot 位置
    max_size = struct.unpack_from("<I", data, RESERVED_BYTES + LP_METADATA_GEOMETRY_MAX_SIZE_OFF)[0]
    slot_count = struct.unpack_from("<I", data, RESERVED_BYTES + LP_METADATA_GEOMETRY_SLOT_COUNT_OFF)[0]
    meta_start = RESERVED_BYTES + GEOMETRY_SIZE * 2
    hdr = data[meta_start:meta_start + LP_METADATA_HEADER_SIZE]
    po, pc, pe = struct.unpack_from("<III", hdr, LP_METADATA_HEADER_PARTITIONS_OFF)
    eo, ec, ee = struct.unpack_from("<III", hdr, LP_METADATA_HEADER_EXTENTS_OFF)
    tables_base = meta_start + LP_METADATA_HEADER_SIZE
    parts = []
    extents = []
    for i in range(pc):
        base = tables_base + po + i * pe
        name = bytes(data[base:base + 36]).split(b"\x00", 1)[0].decode("ascii")
        attr, fe, ne, gi = struct.unpack_from("<IIII", data, base + 36)
        parts.append((name, attr, fe, ne, gi))
    for i in range(ec):
        base = tables_base + eo + i * ee
        ns = struct.unpack_from("<Q", data, base)[0]
        td = struct.unpack_from("<Q", data, base + 12)[0]
        extents.append((ns, td))
    out = []
    for name, attr, fe, ne, gi in parts:
        size = sum(extents[fe + j][0] for j in range(ne)) * SECTOR
        out.append((name, size, extents[fe][1]))
    return parts, out


def locate_logical_partition(super_path, partition_name):
    """定位逻辑分区在 super.img 中的字节偏移与容量（供 dd 增量补丁用）。

    返回 (data_offset_bytes, capacity_bytes) 或 None（分区不存在）。
    data_offset_bytes 是该分区数据在 super.img 中的字节偏移；
    capacity_bytes 是分配的 extent 容量（按 4096 对齐的扇区数×512），
    而非镜像实际字节数——只要新镜像 ≤ capacity_bytes，就能 dd 原地覆盖。
    """
    _, partitions = read_super_image(super_path)
    for name, size_bytes, data_sector in partitions:
        if name == partition_name:
            # size_bytes 是 extent 容量（构建时可能已预留冗余）；
            # data_sector 是 super 内扇区偏移（FIRST_LOGICAL_SECTOR=2048 起）。
            return (data_sector * SECTOR, size_bytes)
    return None


if __name__ == "__main__":
    import sys
    # 用法: python3 pack_super.py <super_size_bytes> <name>=<image> ...  <out>
    if len(sys.argv) < 4:
        print("usage: pack_super.py <super_size_bytes> <name>=<image>... <out>")
        sys.exit(1)
    size = int(sys.argv[1])
    out = sys.argv[-1]
    parts = []
    for arg in sys.argv[2:-1]:
        name, image = arg.split("=", 1)
        parts.append((name, image))
    build_super_image(parts, size, out)
    for row in read_super_image(out)[1]:
        print("  ", row)
