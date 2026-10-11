/* note13-5G-15 云机 SoC 身份对齐：官方 /vendor/bin/chipinfo 的等价实现。
 *
 * 真机链路（gold vendor）：
 *   /vendor/bin/chipinfo（脚本，SELinux 域 chipinfo，由 chipinfo_init.rc 在 early-init
 *   exec_background 拉起）读 MTK sysfs /proc/device-tree 派生 SoC 身份，写：
 *     ro.vendor.soc.manufacturer / ro.vendor.soc.model
 *     ro.vendor.soc.model.external_name / ro.vendor.soc.model.part_name
 *     ro.vendor.soc.model_ready=1
 *   随后 chipinfo_init.rc 在 post-fs-data 由这些值派生 ro.soc.model / ro.soc.manufacturer。
 *
 * 云机为什么不能照抄脚本：virtio 内核没有 /sys/devices/soc0 与 device-tree 节点，
 * 且属性类型 vendor_mtk_soc_prop 属 vendor_restricted_property_type，plat 的
 * neverallow（主体 coredomain）禁止 init/vendor_init 直接 set —— 只能由独立的
 * 非 coredomain 供应商域（即 gold 的 chipinfo）写入。故本程序保留同名同路径同域，
 * 只把「读 sysfs」换成硬编真机实测值（身份常量，不随构建/机型漂移）。
 *
 * 值来源：device-checker properties.native 实读真机（note13-5G-15, MT6833）。
 * 落位 /vendor/bin/chipinfo（标签 chipinfo_exec），无解释器依赖。
 */

#include <stdio.h>
#include <sys/system_properties.h>

static const struct {
    const char *key;
    const char *value;
} kProps[] = {
    { "ro.vendor.soc.manufacturer",         "Mediatek"     },
    { "ro.vendor.soc.model",                "MT6833"       },
    { "ro.vendor.soc.model.external_name",  "MT6833V/PNZA" },
    { "ro.vendor.soc.model.part_name",      "MT6833V/PNZA" },
    { "ro.vendor.soc.model_ready",          "1"            },
};

int main(void) {
    int fail = 0;
    for (unsigned i = 0; i < sizeof(kProps) / sizeof(kProps[0]); i++) {
        int rc = __system_property_set(kProps[i].key, kProps[i].value);
        if (rc != 0) {
            /* 只写 stderr（无 logd 依赖）：init 会把子进程输出落到 serial console */
            fprintf(stderr, "chipinfo: set %s=%s failed (rc=%d)\n",
                    kProps[i].key, kProps[i].value, rc);
            fail++;
        }
    }
    if (fail == 0) {
        printf("chipinfo: %zu 项 SoC 身份属性已写入\n",
               sizeof(kProps) / sizeof(kProps[0]));
    }
    return fail ? 1 : 0;
}