# android.hardware.media.v412 —— note13-5G-15 V4L2 硬件编解码 Codec2 store

> **自 pixel6-15 `vendor/codec2_v4l2` 全量移植**（ChromeOS v4l2_codec2 真实硬件编解码 HAL），
> 依赖自包含：ABI 库从 note13 自己 lineage super 研磨，AOSP 平台源/hidl/libyuv 构建期自取，
> 不借用 pixel6 外部子模块。

## 0. 模块定位

真实硬件编解码 `IComponentStore` 服务（`c2.v4l2.*`），源码来自 ChromeOS v4l2_codec2：

- 服务二进制 `android.hardware.media.c2@1.2-service-v4l2-64` 注册 **`IComponentStore/default`**
- plugin 共享库 `libc2plugin_store.so`（`/vendor/lib64/`）
- 组件：`c2.v4l2.avc.decoder` / `c2.v4l2.hevc.decoder` / `c2.v4l2.avc.encoder`

## 1. 实例规划（关键决策：mtk 让出 default）

真机 note13 无 v4l2（MTK 平台），`default` 由 MTK 假登记 store 承担；
云机注入 v4l2 硬件 store 后，`default` 让给 v4l2，MTK store 改注册 **`default1`**：

| store | 实例 | 二进制 | 提供方 |
|---|---|---|---|
| V4L2 硬件 | `default` | `android.hardware.media.c2@1.2-service-v4l2-64` | 本模块 |
| MTK 假登记 | `default1` | `android.hardware.media.c2-mediatek` | android.hardware.media |
| Dolby 假登记 | `default9` | `vendor.dolby.media.c2-default-service-dax` | android.hardware.media |
| 软解 | `software` | media.swcodec APEX | 平台 |

**同步改动（android.hardware.media 模块）**：
- `config/codec2.json`：`serviceNames` / `stores[0].instance` → `default1`
- `src/cdh_codec2_store_service.cpp`：`kDefault1ServiceName`，`instanceForBinary(mtk)` → `default1`
- `vendor/etc/init/android.hardware.media.c2-mediatek.rc`：加 `interface aidl .../default1`
- `vendor/etc/vintf/manifest/manifest_media_c2_default.xml`：`fqname` → `IComponentStore/default1`

`service_contexts` 无需新增：guest 的 `plat_service_contexts` 已含
`default/default1/default2/software` 四条精确实例名映射（与真机同款），
`default9` 已由 android.hardware.media 的 `MEDIA_VSC_LINES` 补齐。

## 2. 依赖（deps.py）

**链接期 ABI 库全部来自 note13 自己的 lineage-22.0 super**（禁止借用 pixel6）：
- system 侧：`super/system_a.img /system/lib64`（HIDL/基础/阶段特效库）
- swcodec APEX 侧：`super/swcodec-work/apex_payload.img /lib64`（libcodec2_*）

组装到 `out/dist/vendor-compile-dep/media-v412/{system,apex}/lib64`。

## 3. 构建期依赖（deps.py，带缓存离线复用）

| 依赖 | 来源 | 缓存标记 |
|---|---|---|
| `aidl/aosp` 展开源 | 随库固化（5 包 + frameworks 类型，各含 .hash） | `.hash` |
| AOSP platform 源 15 仓 | android.googlesource.com git（tag android-15.0.0_r36，depth=1 blob:none） | `.platform-complete` |
| hidl-gen 工具 | prebuilts/build-tools 下载（sha256 校验） | 二进制存在 |
| HIDL 头 20 包（219 个） | hidl-gen 从 platform 源生成 | 数量 >= 219 |
| AIDL NDK 绑定 | 运行时用 `aidl/aosp` 生成 | — |
| libyuv | platform-source/external/libyuv 源码编译 → `libyuv_cdh.a` | — |

## 4. 构建编排（build.py）

三段式（对齐 pixel6 build/main.go）：
1. 编译全部业务源（common/components/v4l2/plugin_store/service）
2a. 链接 `libc2plugin_store.so` 共享库
2b. libyuv 源码 clang 编译 + llvm-ar 归档 `libyuv_cdh.a`
3. 链接服务 PIE（`-pie -Wl,-e,_start -Wl,--dynamic-linker,/system/bin/linker64`），
   复制 legacy 名副本

产物落 `VENDOR_STAGE_DIR`：
```
bin/hw/android.hardware.media.c2@1.2-service-v4l2-64     （+ legacy 副本）
lib64/libc2plugin_store.so
etc/init/android.hardware.media.c2@1.2-service-v4l2-64.rc
etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy
etc/vintf/manifest/android.hardware.media.c2@1.2-service-v4l2.xml
etc/media_codecs_c2.xml   ← **合并**（追加 c2.v4l2.*，保留 MTK 全集）
```

## 5. media_codecs_c2.xml 合并

`/vendor/etc/media_codecs_c2.xml` 与 android.hardware.media 同路径：
- android.hardware.media 注入真机 MTK 全集（c2.mtk.* 22 条）
- 本模块**追加** `c2.v4l2.*` 3 条（`merge_media_codecs_c2`），
  **绝不覆盖**（覆盖会摧毁已对齐真机的 MediaCodecList）

## 6. SELinux / baselines

- 两个 bin → `u:object_r:hal_codec2_default_exec:s0`
- plugin → `u:object_r:same_process_hal_file:s0`
- rc / vintf / seccomp / xml → `u:object_r:vendor_configs_file:s0`
- rc 追加 `seclabel u:r:mediacodec:s0`（基座无 hal_codec2_default 域自动转换，同 mtk/dolby）
- `mediacodec → hal_codec2_service add/find` 已由 android.hardware.media 的 `media_hal_cil` 授权

## 7. 已知限制

- **云机无 `/dev/video*`**：内核有 `CONFIG_VIDEO_DEV=y` 但 virtio 未暴露 video 节点。
  服务能注册、`listComponents` 能列 c2.v4l2.avc/hevc decoder，但：
  - `createComponent` 实际打不开设备（decoder/encoder 均不可实例化）
  - **`c2.v4l2.avc.encoder` 不显示在 MediaCodecList**：无设备 → encoder 的 profile/level
    能力查询失败 → framework 走默认上限 → Java 层能力解析丢弃（同 mtk 曾遇的塌缩，
    见 android.hardware.media README §9）。真机有 video0-3，后续若加 virtio-video 设备
    可解锁完整编解码。
- **vintf 声明**：本模块声明 `default`，与 mtk 的 `default1` 互不冲突。
- **APEX 私有库落盘**：`libcodec2_aidl.so`/`libcodec2_soft_common.so`/`libcodec2_hidl@1.x.so`
  随模块落盘 `/vendor/lib64`（swcodec APEX namespace 隔离，vendor 进程需本模块提供）。
- **`--help` 等二次 exec 报 libchrome 缺失**：init 拉起的主进程通过 sphal namespace 正常
  解析（已实测注册成功）；shell 直接 exec 走不同 namespace 才缺——不影响服务运行。
