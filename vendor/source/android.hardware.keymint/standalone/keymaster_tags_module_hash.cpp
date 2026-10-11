// keymaster_tags_module_hash.cpp —— note13 运行库兼容垫片。
//
// note13 lineage-22.0 的 /vendor/lib64/libkeymaster_messages.so 是较早的 keymaster
// 快照，未导出 `keymaster::TAG_MODULE_HASH`（AOSP 较新版本在 keymaster_tags.cpp 用
// DEFINE_KEYMASTER_TAG(KM_BYTES, TAG_MODULE_HASH) 定义）。而 Remote KeyMint HAL
// （remote_keymint_device.cpp 等）的静态初始化引用了该符号，直接链接 note13 运行库
// 会 undefined symbol。
//
// 这里以完全相同的方式补一份 **强定义**：链接期满足引用；运行期 guest 的旧库未导出
// 该符号，可执行文件自带的定义会被自身引用解析（避免加载期重定位失败）。TypedTag
// 的取值即模板参数（KM_TAG_MODULE_HASH），与枚举语义一致，无需依赖运行库版本。
//
// 适用范围仅 Remote KeyMint HAL 进程；guest 其它使用旧 keymaster 库的进程不受影响。
#include <keymaster/keymaster_tags.h>

namespace keymaster {
TypedTag<KM_BYTES, KM_TAG_MODULE_HASH> TAG_MODULE_HASH;
}  // namespace keymaster