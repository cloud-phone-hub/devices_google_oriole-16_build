"""vendor 文件裁剪：删除与本 vendor 内容不匹配的遗留 rc / 已被取代的基座 rc。

1) lineage virtio vendor 只有 64 位内容（/vendor/bin 仅 boringssl_self_test64），却带了
   /vendor/etc/boringssl_self_test.zygote64_32.rc。该 rc 的触发条件是**系统属性**
   ro.product.cpu.abilist32=*（官方 MIUI /system 上该属性为 armeabi-v7a,armeabi，本机
   必命中），命中后 exec_start boringssl_self_test32_vendor → 执行
   /vendor/bin/boringssl_self_test32；本 vendor 无此二进制 → init 按"加密自检失败"
   处理 → `reboot,boringssl-self-check-failed` → 引导死循环（实测 0.6s 即重启）。
   本 vendor 确无 32 位 crypto 内容，该 32 位自检无从进行，故删除该 rc。

2) lineage 基座的主 init rc 名为 init.virtio.rc（按 ro.hardware=virtio 拼出）。云机
   ro.hardware 已对齐真机 mt6833（build-vendor-boot.py 改 androidboot.hardware），
   同名主 rc 改由 vendor patch 树提供 /etc/init/hw/init.mt6833.rc，基座的
   init.virtio.rc 已无人读取，留着还会与真机 /vendor/etc/init/hw 的内容不符（真机只有
   init.mt6833.rc 系列 MTK rc），故删除。
"""

from debugfs import path_exists

# 要删除的镜像内路径
PRUNE_FILES = (
    "/etc/boringssl_self_test.zygote64_32.rc",
    "/etc/init/hw/init.virtio.rc",
)


def prune_cmds(work_img):
    """产出裁剪 debugfs 命令清单（存在才删）。"""
    cmds = []
    for path in PRUNE_FILES:
        if path_exists(work_img, path):
            cmds.append(f"rm {path}")
            print(f"[..] 裁剪 vendor 文件: {path}")
    return cmds