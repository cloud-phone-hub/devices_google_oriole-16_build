"""官方 gold vendor 属性上下文对齐（/dev/__properties__ 类型命名空间）。

目标：/dev/__properties__ 下的区域文件集合由各分区 *_property_contexts 表与策略里
声明的属性类型共同决定。本机 /system、/system_ext 与 gold 一致（system_ext md5 实测
相同），仅 vendor 侧为 lineage 版本：lineage vendor 只贡献 13 个属性类型，gold vendor
贡献 283 个（MTK/MIUI 的各类 *_prop，含 3 个带连字符的 ctl 类型）。本模块把 vendor
侧对齐到真机：

  1. 上下文表替换（预会话落盘）：/vendor/etc/selinux/vendor_property_contexts 全量
     替换为 gold 版——不留任何 lineage 独有前缀，否则云机属性区会多出真机没有的
     文件（实测 2026-09-27 多出 11 个），与"完全对齐真机"目标冲突；
  2. 策略声明补齐（CIL 追加段，最终由 sepolicy 链统一追加）：为 gold 上下文引用、
     而本策略未声明的属性类型补 (type X)/(roletype object_r X)，按 gold 的
     vendor_property_type / vendor_restricted_property_type 归属镜像成员关系，
     并复刻 gold 中**源域在本策略存在**的授权语句（property_service set / 属性区
     file 读）——源域不存在的（如 MTK 测试域 cittest_app）跳过，保持与真机同源语义。

不改动 file/hwservice/service contexts 与整体策略文件：整包替换实测会让本 vendor
内容（CDH HAL/daemon 路径）被 gold 的 file_contexts 错标、keystore2 服务注册不出
（见 gold_selinux 模块「实测结论」）。

时机：上下文表必须在 sepolicy 链**收集命令之前真实落盘**（该链读取镜像当前内容后
统一追加，同会话命令未执行）；故由 vendor_builder 单独一次 debugfs 会话执行本模块
的 prop_ctx_cmds，与 gold_selinux 同机制。
"""

import os
import re
import sys

from buildenv import ROM_ROOT_DIR, TMP_DIR
from debugfs import path_exists

PROP_CTX_SRC = os.path.join(ROM_ROOT_DIR, "vendor_a", "etc", "selinux",
                            "vendor_property_contexts")
GOLD_VENDOR_CIL = os.path.join(ROM_ROOT_DIR, "vendor_a", "etc", "selinux",
                               "vendor_sepolicy.cil")
IMG_PROP_CTX = "/etc/selinux/vendor_property_contexts"
PROP_CTX_LABEL = "u:object_r:vendor_configs_file:s0"
MARKER = ";; cdh note13-5G-15: gold vendor prop types"
# plat 对 init 的属性区文件权限集（plat cil 实测，用于新增类型的显式授权）
INIT_PROP_PERMS = ("read write create getattr setattr relabelto append map "
                   "unlink rename open")

# 注意：类型名可含连字符（gold MTK 属性类型，如 vendor_mtk_ctl_ril-proxy_prop）——
# 字符集漏掉 '-' 会静默丢弃这些类型 → 不声明 → init 建属性区 setxattr 报
# `op=setxattr invalid_context` → "Failed to initialize property area" → InitFatalReboot
# （实测 2026-09-27，guest 连续重启 63 次，全部只有这 3 个类型报 invalid_context）。
_RE_TYPE = re.compile(r"u:object_r:([A-Za-z_0-9-]+):s0")
_RE_ALLOW_FULL = re.compile(
    r"^\(allow\s+(\S+)\s+(\S+)\s+\((\S+)\s+\(([^)]*)\)\)\)$")


def _rules(text):
    """上下文表有效规则行（去注释/空行）。"""
    return [ln for ln in text.splitlines()
            if ln.strip() and not ln.strip().startswith("#")]


def gold_types():
    """gold vendor_property_contexts 引用的属性类型名（忽略注释行，sorted）。

    注释行不参与编译属性表 —— 若不剔除，被注释规则引用的类型（如
    vendor_cct_prop）会被当成"应存在"的类型，校验时报假缺失（实测 2026-09-27）。
    """
    with open(PROP_CTX_SRC, encoding="utf-8", errors="replace") as f:
        text = f.read()
    return sorted({m.group(1) for m in _RE_TYPE.finditer("\n".join(_rules(text)))})


def prop_ctx_cmds(work_img):
    """预会话：vendor_property_contexts 全量替换为 gold 版（与真机完全一致）。

    不保留任何 lineage 独有前缀规则：保留会让云机多出真机没有的属性区文件
    （实测 2026-09-27：多出 11 个，如 vendor_mesa_prop / vendor_graphics_prop /
    vendor_hwcomposer_prop / vendor_audio_prop 等），与"完全对齐真机"目标冲突。
    真机侧属性区文件集合 = 各分区（含 system/system_ext/product/odm）上下文表所
    引用类型的并集，纯 gold 表即真机口径。
    """
    if not os.path.isfile(PROP_CTX_SRC):
        print(f"[err] 官方 vendor_property_contexts 缺失: {PROP_CTX_SRC}")
        sys.exit(1)
    with open(PROP_CTX_SRC, encoding="utf-8", errors="replace") as f:
        gold_text = f.read()
    tmp_dir = os.path.join(TMP_DIR, "vendor")
    os.makedirs(tmp_dir, exist_ok=True)
    tmp = os.path.join(tmp_dir, "vendor_property_contexts.gold")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(gold_text if gold_text.endswith("\n") else gold_text + "\n")
    print(f"[ok] vendor_property_contexts 全量替换为 gold 版"
          f"（{len(_rules(gold_text))} 条规则，不留 lineage 独有前缀）")
    cmds = []
    if path_exists(work_img, IMG_PROP_CTX):
        cmds.append(f"rm {IMG_PROP_CTX}")
    cmds += [
        f"write {tmp} {IMG_PROP_CTX}",
        f"sif {IMG_PROP_CTX} mode 0x81a4",
        f"sif {IMG_PROP_CTX} uid 0",
        f"sif {IMG_PROP_CTX} gid 0",
        f"ea_set {IMG_PROP_CTX} security.selinux {PROP_CTX_LABEL}",
    ]
    return cmds


def _attr_members(text, attr):
    m = re.search(r"\(typeattributeset %s \(([^)]*)\)\)" % re.escape(attr), text)
    return set(m.group(1).split()) if m else set()


def prop_types_cil(vers, known):
    """gold 属性类型的声明 + 归属 + 授权 CIL 段。

    known = 本策略已声明的类型/属性名全集（plat cil + plat_pub_versioned + vendor
    cil）：用于跳过已声明类型与过滤源域不存在的授权语句。vers = plat 策略版本
    （如 202404，gold 授权语句里的域影子名一致，直接原样复刻）。
    """
    new = gold_types()
    gold_cil = open(GOLD_VENDOR_CIL, encoding="utf-8", errors="replace").read()
    lines = [f"{MARKER}\n"]
    declared = [t for t in new if t not in known]
    for t in declared:
        lines.append(f"(type {t})\n")
        lines.append(f"(roletype object_r {t})\n")
    for attr in ("vendor_property_type", "vendor_restricted_property_type"):
        if attr not in known:
            lines.append(f"(typeattribute {attr})\n")
        members = [t for t in new if t in _attr_members(gold_cil, attr)]
        if members:
            lines.append(f"(typeattributeset {attr} ({' '.join(members)}))\n")
    allowed, skipped = 0, 0
    merged_allows = {}          # (src, tgt, cls) -> set(perms)，gold 文件内重复/拆分的
    for ln in gold_cil.splitlines():     # 语句合并去重（否则 validate_cil 判重复/拆分冗余）
        m = _RE_ALLOW_FULL.match(ln.strip())
        if not m:
            continue
        src, tgt, cls, perms = m.group(1), m.group(2), m.group(3), m.group(4).split()
        if tgt not in new:
            continue
        if src not in known:
            skipped += 1
            continue
        merged_allows.setdefault((src, tgt, cls), set()).update(perms)
        allowed += 1
    # init 建/重标属性区文件：plat 的 init 授权以 **property_type 属性**为目标
    # （plat cil: `(allow init property_type (file (read write create getattr setattr
    # relabelto append map unlink rename open)))`），新增类型不在该属性内 → 区域初始化
    # 时 relabelto 被拒 → init: Failed to initialize property area → InitFatalReboot
    # （实测 2026-09-27）。按 plat 同权限集逐类型显式授权（并入合并表，不动属性归属）。
    for t in new:
        merged_allows.setdefault((f"init_{vers}", t, "file"),
                                 set()).update(INIT_PROP_PERMS.split())
    # 属性区文件需与 tmpfs 关联（plat: `(allow property_type tmpfs (filesystem
    # (associate)))`）；新增类型不在 property_type 属性内 → init 建区后 associate 被拒
    # → init: Failed to initialize property area → InitFatalReboot（实测 2026-09-27）。
    # 逐类型显式授权；不并入 property_type 属性——该属性上有多条 neverallow
    # （如 neverallow domain property_type (file (ioctl lock))），并入会炸 neverallow。
    for t in new:
        merged_allows.setdefault((t, "tmpfs", "filesystem"),
                                 set()).add("associate")
    # 属性 set：本机 /vendor/build.prop 等由 init 直接加载（avc 实测 scontext=
    # u:r:init:s0，无 vendor_init 过渡），lineage 类型时代 init 可设；映射到 gold
    # 类型后 init 无 set 授权 → 属性设不进去（实测 2026-09-27：拒绝数 36 vs 基线
    # 22，新增 18 条全是映射到 gold MTK 类型的属性）。逐类型补 init set。
    # neverallow 安全：vendor 侧 neverallow 主体 base_typeattr_234_202404 不含
    # init（plat 的 base_typeattr_223 同样不含），且无属性级 neverallow。
    for t in new:
        merged_allows.setdefault((f"init_{vers}", t, "property_service"),
                                 set()).add("set")
    for (src, tgt, cls), perms in sorted(merged_allows.items()):
        lines.append(f"(allow {src} {tgt} ({cls} ({' '.join(sorted(perms))})))\n")
    print(f"[ok] gold 属性类型对齐 CIL（plat vers {vers}）：新增类型 "
          f"{len(declared)} 个，复刻授权 {len(merged_allows)} 条"
          f"（gold 原句 {allowed} 条已合并去重；源域不存在的跳过 {skipped} 条）")
    return "".join(lines)