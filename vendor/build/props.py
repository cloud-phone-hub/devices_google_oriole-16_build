"""vendor build.prop 渲染修正（属性声明收敛到 baselines，避免硬编码漂移）。

对齐 pixel6-15 的 baselines vendor.build-prop-sets / build-prop-unsets：
图形后端切 mesa 软渲染、64 位 only 口径（zygote64 + abilist32 置空）、以及
Remote KeyMint 所需的 ro.vendor.build.security_patch / ro.vendor.boot_security_patch
（host secure_env 要求 vendor/boot patchlevel 有效且与 os 一致，note13 基座
缺 boot、build 是 2024-11-05，需与 clamp 后的 os_patchlevel 202505 对齐）。
键值对全部声明在 baselines JSON，本模块只负责应用。
"""

import json
import os

from buildenv import baselines_file  # noqa: E402
from debugfs import cat, path_exists  # noqa: E402


def _build_prop_edits():
    """读取 baselines vendor.build-prop-sets / build-prop-unsets。

    sets：["key", "value"] 覆盖或追加；unsets："key" 删除所有匹配行。
    """
    with open(baselines_file(), encoding="utf-8") as f:
        doc = json.load(f)
    v = doc.get("vendor", {})
    sets = [tuple(e) for e in v.get("build-prop-sets", []) if len(e) >= 2]
    unsets = list(v.get("build-prop-unsets", []))
    return sets, unsets


def host_content(work_img, work_dir):
    """读回镜像内 /build.prop，应用 baselines 声明的 set/unset 并写为宿主文件。"""
    data = cat(work_img, "/build.prop")
    if data is None:
        lines = []
    else:
        lines = data.decode("utf-8", "replace").splitlines()

    prop_sets, prop_unsets = _build_prop_edits()

    # 1) unset：删除所有以 "key=" 开头的行
    for key in prop_unsets:
        lines = [ln for ln in lines if not ln.startswith(key + "=")]

    # 2) set：已有键覆盖，缺省追加
    for k, v in prop_sets:
        matched = False
        for i, ln in enumerate(lines):
            if ln.startswith(k + "="):
                lines[i] = f"{k}={v}"
                matched = True
                break
        if not matched:
            lines.append(f"{k}={v}")

    tmp = os.path.join(work_dir, "build.prop")
    with open(tmp, "w") as f:
        f.write("\n".join(lines) + ("\n" if lines else ""))
    return tmp


def prop_cmds(work_img, bp_host):
    """build.prop 注入 debugfs 命令清单（重写 + 归一元数据 + SELinux 标签）。"""
    cmds = []
    if path_exists(work_img, "/build.prop"):
        cmds.append("rm /build.prop")
    cmds += [
        f"write {bp_host} /build.prop",
        "sif /build.prop mode 0x81a4",
        "sif /build.prop uid 0",
        "sif /build.prop gid 0",
        "ea_set /build.prop security.selinux u:object_r:vendor_file:s0",
    ]
    return cmds
