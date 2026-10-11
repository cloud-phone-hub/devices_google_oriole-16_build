"""vendor 注入树 -> debugfs 命令清单。

注入两棵树，相对路径即镜像内 /vendor 路径：
  1. build/vendor/patch（seasoning）：bin/logcat-dump、etc/init/*.rc、
     etc/permissions/*.xml 等，默认文件模式 0644。
  2. out/dist/vendor（source 子模块落盘）：bin/daemon 等编译产物，可执行文件
     模式 0755。

SELinux 标签统一按基线 vendor.selinux-labels 回填（键 = 注入树内相对路径），
缺失即报错退出。staging 树缺失/为空时静默跳过（产物可未编译）。
"""

import os
import sys

from buildenv import VENDOR_PATCH_DIR, VENDOR_STAGE_DIR
from debugfs import path_exists
from selinux import patch_labels

# 常规数据文件（patch 树）
MODE_FILE = 0o100644      # 0x81a4
# 可执行文件（staging 树），如 /vendor/bin/daemon
MODE_EXEC = 0o100755      # 0x8175
# patch 树内需可执行位的脚本（init service 直接 exec，如 bin/netmgr_setup）
PATCH_EXEC = {"bin/netmgr_setup"}
# staging 树内需要常规数据模式（0644）的文件：staging 默认按可执行产物打 0755，
# 但 source/android.hardware.gpu 落盘的是 interposer 与回填的共享库，带可执行位会
# 与官方/基座原模式不一致（且 same_process_hal_file 的 .so 本就不该带可执行位）。
STAGE_FILE = {
    "lib64/egl/libEGL_meow.so",
    "lib64/egl/libGLESv1_CM_meow.so",
    "lib64/egl/libGLESv2_meow.so",
    "lib64/hw/vulkan.mali.so",
    # 官方 gold vendor 回填件（文件扫描面）
    "lib64/egl/libGLES_mali.so",
    "lib64/egl/libMEOW_data.so",
    "lib64/egl/libMEOW_gift.so",
    "lib64/egl/libMEOW_qt.so",
    "lib64/egl/libMEOW_trace.so",
    "lib64/libOpenCL.so",
    # rc 是配置文件：真机 /vendor/etc/init/*.rc 为 0644，staging 默认 0755 会偏
    # （chipinfo_init.rc 由 source/android.hardware.board 落盘）
    "etc/init/chipinfo_init.rc",
    "etc/init/android.hardware.wifi-service.cdh-note13.rc",
}


def _inject_tree(work_img, tree_dir, labels, cmds, mode):
    """注入单个注入树，返回注入文件数；标签缺失即报错退出。"""
    if not os.path.isdir(tree_dir):
        return 0
    n = 0
    for root, _dirs, files in os.walk(tree_dir):
        rel = os.path.relpath(root, tree_dir)
        img_dir = "/" if rel == "." else "/" + rel.replace(os.sep, "/")
        if not path_exists(work_img, img_dir):
            cmds.append(f"mkdir {img_dir}")
        for name in files:
            if name == ".DS_Store":
                continue
            host = os.path.join(root, name)
            img = img_dir + "/" + name
            rel_key = os.path.relpath(host, tree_dir).replace(os.sep, "/")
            label = labels.get(rel_key)
            if not label:
                print(f"[err] 基线清单缺少 SELinux 标签: {rel_key}")
                sys.exit(1)
            file_mode = MODE_EXEC if rel_key in PATCH_EXEC else mode
            if mode == MODE_EXEC and rel_key in STAGE_FILE:
                file_mode = MODE_FILE
            if path_exists(work_img, img):
                cmds.append(f"rm {img}")
            cmds += [
                f"write {host} {img}",
                f"sif {img} mode {file_mode:#x}",
                f"sif {img} uid 0",
                f"sif {img} gid 0",
                f"ea_set {img} security.selinux {label}",
            ]
            n += 1
    return n


def inject_cmds(work_img):
    """walk 注入树，产出注入 debugfs 命令（mkdir/rm/write/sif/ea_set）。"""
    labels = patch_labels()
    cmds = []
    n = 0
    n += _inject_tree(work_img, VENDOR_PATCH_DIR, labels, cmds, MODE_FILE)
    n += _inject_tree(work_img, VENDOR_STAGE_DIR, labels, cmds, MODE_EXEC)
    print(f"[..] 注入树: {n} 个文件 -> {VENDOR_PATCH_DIR} + {VENDOR_STAGE_DIR}")
    return cmds