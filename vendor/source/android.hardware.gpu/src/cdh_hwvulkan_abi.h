// Android libhardware / hwvulkan 的 ABI 声明（自带，不依赖构建期临时快照）。
//
// 为什么自带：platform-source 快照落在 out/tmp/*/platform-source，是各模块编译时
// 的临时产物，随时可能被清掉；而这套结构是公开 HAL 契约、ABI 稳定，抄一份进来
// 比引一条易断的 include 路径可靠。取值逐条核对自 AOSP
// hardware/libhardware/include/hardware/hardware.h 与
// frameworks/native/vulkan/include/hardware/hwvulkan.h。
//
// 关键契约（容易搞错的一点）：hwvulkan_module_t **只有 common 一个成员**，
// 没有 pfnGetInstanceProcAddr；真正的入口在 methods->open() 返回的
// hwvulkan_device_t 上（EnumerateInstanceExtensionProperties / CreateInstance /
// GetInstanceProcAddr 三个函数指针）。

#ifndef CDH_HWVULKAN_ABI_H
#define CDH_HWVULKAN_ABI_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#define CDH_MAKE_TAG_CONSTANT(A, B, C, D) \
    (((A) << 24) | ((B) << 16) | ((C) << 8) | (D))
#define CDH_MAKE_API_VERSION(maj, min) ((((maj)&0xff) << 8) | ((min)&0xff))
#define CDH_MAKE_API_VERSION_2(maj, min, hdr) \
    ((((maj)&0xff) << 24) | (((min)&0xff) << 16) | ((hdr)&0xffff))

// 'H','W','M','T' / 'H','W','D','T'
#define CDH_HARDWARE_MODULE_TAG CDH_MAKE_TAG_CONSTANT('H', 'W', 'M', 'T')
#define CDH_HARDWARE_DEVICE_TAG CDH_MAKE_TAG_CONSTANT('H', 'W', 'D', 'T')
#define CDH_HARDWARE_HAL_API_VERSION CDH_MAKE_API_VERSION(1, 0)
#define CDH_HWVULKAN_MODULE_API_VERSION_0_1 CDH_MAKE_API_VERSION(0, 1)
#define CDH_HWVULKAN_DEVICE_API_VERSION_0_1 CDH_MAKE_API_VERSION_2(0, 1, 0)
#define CDH_HWVULKAN_HARDWARE_MODULE_ID "vulkan"
#define CDH_HWVULKAN_DEVICE_0 "vk0"
// loader 用这个名字 dlsym 模块信息
#define CDH_HAL_MODULE_INFO_SYM_AS_STR "HMI"

typedef struct cdh_hw_module_t cdh_hw_module_t;
typedef struct cdh_hw_device_t cdh_hw_device_t;

typedef struct cdh_hw_module_methods_t {
    int (*open)(const cdh_hw_module_t *module, const char *id,
                cdh_hw_device_t **device);
} cdh_hw_module_methods_t;

typedef struct cdh_hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    cdh_hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];   // __LP64__ 分支；本模块只出 arm64
} cdh_hw_module_t;

typedef struct cdh_hw_device_t {
    uint32_t tag;
    uint32_t version;
    cdh_hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(cdh_hw_device_t *device);
} cdh_hw_device_t;

typedef struct cdh_hwvulkan_module_t {
    cdh_hw_module_t common;
} cdh_hwvulkan_module_t;

typedef struct cdh_hwvulkan_device_t {
    cdh_hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} cdh_hwvulkan_device_t;

#endif   // CDH_HWVULKAN_ABI_H
