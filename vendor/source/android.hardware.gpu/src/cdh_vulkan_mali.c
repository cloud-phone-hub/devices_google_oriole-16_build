// note13-5G-15 Vulkan HAL interposer：产物 lib64/hw/vulkan.mali.so。
//
// 顶替原理：Android 的 libvulkan.so 按 ro.hardware.vulkan 后缀 dlopen
// vulkan.<suffix>.so，dlsym "HMI" 拿到 hwvulkan_module_t，再调
// methods->open(module, "vk0", &device) 得到 hwvulkan_device_t，之后**所有**入口
// 都从该结构的三个函数指针走。所以 interposer 只需：
//   1) 自己实现 open()，在里面转手打开基座 venus（vulkan.virtio.so）；
//   2) 把 venus device 的三个函数指针原样抄过来，只把 GetInstanceProcAddr
//      换成自己的包装；
//   3) 包装里命中要改写的名字就返回覆写实现，其余全部转发。
// 基座 venus 是 Mesa Venus over virtio-gpu，宿主 QEMU 已带 venus=on。
//
// 覆写面刻意收窄到 deviceName 一项：真机 vkGetPhysicalDeviceProperties 的
// vendorID/deviceID/apiVersion/driverVersion 尚未采集（真机离线），编造这些值会与
// deviceName 自相矛盾，反而更容易被识别。宁可只对齐已实测的那一项。
//
// 失败姿态：本库只在 ro.hardware.vulkan=mali 时被加载；venus 打不开就 open() 失败，
// loader 退回「无 Vulkan HAL」——即改动前的现状，不会让图形栈崩掉。

#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <android/log.h>

#include "cdh_gpu_targets.inc"
#include "cdh_hwvulkan_abi.h"

#define CDH_LOG_TAG "cdh-gpu"
#define CDH_VK_BASE_SONAME CDH_ID_VK_BASE_SONAME

static cdh_hwvulkan_device_t cdh_dev;          // 我们对外暴露的 device
static cdh_hwvulkan_device_t *cdh_base_dev;    // venus 的 device
static int (*cdh_base_close)(cdh_hw_device_t *);
static PFN_vkGetInstanceProcAddr cdh_base_gipa;
static PFN_vkGetPhysicalDeviceProperties cdh_real_gppd;
static PFN_vkGetPhysicalDeviceProperties2 cdh_real_gppd2;

// deviceName 是 char[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]，就地覆写。
// 用 sizeof 而非宏名取值：该宏在 Vulkan 1.4 头里由 ..._NAME 改名为 ..._NAME_SIZE，
// 写死宏名会随 NDK 版本断掉。
static void cdh_stamp(VkPhysicalDeviceProperties *p) {
    if (!p) {
        return;
    }
    const size_t cap = sizeof(p->deviceName);
    strncpy(p->deviceName, CDH_ID_VK_DEVICE_NAME, cap - 1);
    p->deviceName[cap - 1] = '\0';
}

static void VKAPI_CALL cdh_gppd(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceProperties *pProperties) {
    if (!cdh_real_gppd) {
        return;
    }
    cdh_real_gppd(physicalDevice, pProperties);
    cdh_stamp(pProperties);
}

// VkPhysicalDeviceProperties2 的首个成员就是 VkPhysicalDeviceProperties，
// 扩展版复用同一套 stamp。
static void VKAPI_CALL cdh_gppd2(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceProperties2 *pProperties) {
    if (!cdh_real_gppd2) {
        return;
    }
    cdh_real_gppd2(physicalDevice, pProperties);
    if (pProperties) {
        cdh_stamp(&pProperties->properties);
    }
}

static PFN_vkVoidFunction VKAPI_CALL cdh_gipa(VkInstance instance,
                                              const char *pName) {
    if (!cdh_base_gipa) {
        return NULL;
    }
    if (pName) {
        if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0) {
            cdh_real_gppd = (PFN_vkGetPhysicalDeviceProperties)cdh_base_gipa(
                instance, pName);
            return cdh_real_gppd ? (PFN_vkVoidFunction)cdh_gppd : NULL;
        }
        if (strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0) {
            cdh_real_gppd2 = (PFN_vkGetPhysicalDeviceProperties2)cdh_base_gipa(
                instance, pName);
            return cdh_real_gppd2 ? (PFN_vkVoidFunction)cdh_gppd2 : NULL;
        }
    }
    return cdh_base_gipa(instance, pName);
}

static int cdh_close(cdh_hw_device_t *device) {
    (void)device;
    if (cdh_base_close && cdh_base_dev) {
        return cdh_base_close(&cdh_base_dev->common);
    }
    return 0;
}

static int cdh_open(const cdh_hw_module_t *module, const char *id,
                    cdh_hw_device_t **device) {
    (void)module;
    if (!id || strcmp(id, CDH_HWVULKAN_DEVICE_0) != 0) {
        return -EINVAL;
    }
    // 先按绝对路径开（/vendor 会被内核重定向到 /mnt/vendor），失败再退回
    // soname 让 linker 按命名空间搜索路径解析。
    void *h = dlopen(CDH_ID_VK_BASE_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        h = dlopen(CDH_VK_BASE_SONAME, RTLD_NOW | RTLD_LOCAL);
    }
    if (!h) {
        const char *e = dlerror();
        __android_log_print(ANDROID_LOG_ERROR, CDH_LOG_TAG,
                            "vulkan: dlopen(%s) 失败: %s", CDH_VK_BASE_SONAME,
                            e ? e : "?");
        return -ENOENT;
    }
    cdh_hwvulkan_module_t *base =
        (cdh_hwvulkan_module_t *)dlsym(h, CDH_HAL_MODULE_INFO_SYM_AS_STR);
    if (!base || !base->common.methods || !base->common.methods->open) {
        __android_log_print(ANDROID_LOG_ERROR, CDH_LOG_TAG,
                            "vulkan: %s 里没有可用的 %s/methods->open",
                            CDH_VK_BASE_SONAME, CDH_HAL_MODULE_INFO_SYM_AS_STR);
        return -ENOENT;
    }
    cdh_hw_device_t *base_common = NULL;
    int rc = base->common.methods->open(&base->common, id, &base_common);
    if (rc != 0 || !base_common) {
        __android_log_print(ANDROID_LOG_ERROR, CDH_LOG_TAG,
                            "vulkan: 基座 open(%s) 失败 rc=%d", id, rc);
        return rc != 0 ? rc : -EIO;
    }
    cdh_base_dev = (cdh_hwvulkan_device_t *)base_common;
    cdh_base_close = cdh_base_dev->common.close;
    cdh_base_gipa = cdh_base_dev->GetInstanceProcAddr;

    // 抄基座 device 的 tag/version（loader 会校验 version），close 换成自己的，
    // module 指向本模块，三个入口只替换 GetInstanceProcAddr。
    cdh_dev.common = cdh_base_dev->common;
    cdh_dev.common.module = (cdh_hw_module_t *)module;
    cdh_dev.common.close = cdh_close;
    cdh_dev.EnumerateInstanceExtensionProperties =
        cdh_base_dev->EnumerateInstanceExtensionProperties;
    cdh_dev.CreateInstance = cdh_base_dev->CreateInstance;
    cdh_dev.GetInstanceProcAddr = cdh_gipa;

    *device = &cdh_dev.common;
    __android_log_print(ANDROID_LOG_INFO, CDH_LOG_TAG,
                        "vulkan: %s 已接管，基座 %s", CDH_ID_VK_DEVICE_NAME,
                        CDH_VK_BASE_SONAME);
    return 0;
}

static cdh_hw_module_methods_t cdh_methods = {
    .open = cdh_open,
};

// 符号名必须是 HMI（loader 按该名 dlsym）。
__attribute__((visibility("default")))
cdh_hwvulkan_module_t HMI = {
    .common =
        {
            .tag = CDH_HARDWARE_MODULE_TAG,
            .module_api_version = CDH_HWVULKAN_MODULE_API_VERSION_0_1,
            .hal_api_version = CDH_HARDWARE_HAL_API_VERSION,
            .id = CDH_HWVULKAN_HARDWARE_MODULE_ID,
            .name = "CDH Mali-G57 MC2 Vulkan HAL (venus forwarder)",
            .author = "cloud-phone-hub",
            .methods = &cdh_methods,
            .dso = NULL,
            .reserved = {0},
        },
};
