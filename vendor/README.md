# note13-5G-15 vendor 移植说明

场景：**以厂商（MIUI/MTK gold）vendor 为底座**时，把 lineage（virtio）vendor 中“云机运行必需”的内容移植过去，并让系统加载我们指定的 vendor 内容。

> 现状架构（推荐保持）：官方 MIUI system + **lineage virtio vendor 为底座** + 官方身份基线（build.prop）。本 README 记录的是反向（官方 vendor 为底座）的移植清单与加载方案演进，供决策/实现参考。
>
> **结论先行**：分区/文件系统层的方案（独立分区 / overlay / qcow2 差异）都会暴露挂载特征，无法满足“应用验证 vendor 为厂商”的要求；EROFS 也无原地编辑能力。**最终方向 = 官方 vendor 保持原样 + 内核层挂载伪装（patch 集）**，见 [二-2.4](#24-最终方向内核层挂载伪装patch-集)。

---

## 一、必须从 lineage vendor 移植的文件（完整路径）

> 以下全部路径均为 **vendor 分区内路径**（挂载点为 `/vendor`）。数据来自 `lineage-rom/lineage-22.0/super/vendor_a.img`（ext4，virtio 内核匹配）。

### 1. HAL 服务二进制 `/vendor/bin/hw/`
```
/vendor/bin/hw/android.hardware.bluetooth-service.default
/vendor/bin/hw/android.hardware.cas@1.2-service
/vendor/bin/hw/android.hardware.gatekeeper@1.0-service.software
/vendor/bin/hw/android.hardware.graphics.allocator-service.minigbm
/vendor/bin/hw/android.hardware.graphics.composer@2.4-service
/vendor/bin/hw/hostapd
/vendor/bin/hw/wpa_supplicant
```
作用：virtio 图形（minigbm 分配器 / composer@2.4）、软件 gatekeeper、蓝牙 default（走 virtio-serial 对接宿主 netsimd）、CAS、WiFi（hostapd/wpa_supplicant）。

### 2. HAL 实现库 `/vendor/lib64/hw/`
```
/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl.minigbm.so
/vendor/lib64/hw/android.hidl.memory@1.0-impl.so
/vendor/lib64/hw/gralloc.default.so
/vendor/lib64/hw/hwcomposer.drm.so
/vendor/lib64/hw/mapper.minigbm.so
/vendor/lib64/hw/vulkan.pastel.so
/vendor/lib64/hw/vulkan.virtio.so
```
作用：DRM hwcomposer + minigbm gralloc + virtio/pastel Vulkan——virtio-gpu 上渲染的核心。

### 3. 图形 EGL 库 `/vendor/lib64/egl/`
```
/vendor/lib64/egl/libEGL_mesa.so
/vendor/lib64/egl/libGLESv2_mesa.so
```
作用：mesa 软渲染 GL（无硬件 GPU 时 SurfaceFlinger/app 依赖）。

### 4. init 脚本 `/vendor/etc/init/`
```
/vendor/etc/init/allocator.rc
/vendor/etc/init/android.hardware.cas@1.2-service.rc
/vendor/etc/init/android.hardware.gatekeeper@1.0-service.software.rc
/vendor/etc/init/android.hardware.graphics.composer@2.4-service.rc
/vendor/etc/init/android.hardware.security.keymint-service.rc
/vendor/etc/init/android.hardware.wifi.supplicant-service.rc
/vendor/etc/init/boringssl_self_test.rc
/vendor/etc/init/setup_wifi.rc
/vendor/etc/init/vndservicemanager.rc
/vendor/etc/init/hw/init.low_performance.rc
/vendor/etc/init/hw/init.virtio.rc      # ← 核心：by-name symlink、mount_all、GPU 检测、seriallogging
```
作用：拉起上述 HAL 服务；`init.virtio.rc` 负责 `/dev/block/vda*` → by-name symlink、`mount_all --early/--late`、按 `ro.boot.gpu` 选择图形后端、`seriallogging` 到 `/dev/hvc1`。

### 5. VINTF 清单 `/vendor/etc/vintf/`
```
/vendor/etc/vintf/manifest.xml
/vendor/etc/vintf/compatibility_matrix.xml
```
作用：声明 `android.hardware.graphics.composer@2.4`（HIDL）与 `sepolicy version 202404`；官方 manifest 声明的是 MTK HAL 全集，必须替换否则 framework 按官方清单找 MTK HAL 全部失败。

### 6. 音频策略 `/vendor/etc/`
```
/vendor/etc/audio_policy_configuration.xml
/vendor/etc/audio_policy_volumes.xml
/vendor/etc/primary_audio_policy_configuration.xml
/vendor/etc/audio_effects_config.xml
/vendor/etc/aidl/le_audio/...            # LE Audio AIDL（需按需确认）
```
配套库：`/vendor/lib64/libtinyalsav2.so`、`/vendor/lib64/libaudioutils.so`、`/vendor/lib64/libeffects.so`、`/vendor/lib64/libspeexresampler.so`、`/vendor/lib64/soundfx/{libaudiopreprocessing,libdownmix,libeffectproxy,libldnhncr,libvisualizer}.so`
> 注：本 virtio vendor 的 manifest **未声明 audio HAL 服务**，音频走 policy + tinyalsa；集成时需实测确认。

### 7. SELinux `/vendor/etc/selinux/`
```
/vendor/etc/selinux/vendor_sepolicy.cil
/vendor/etc/selinux/plat_pub_versioned.cil
/vendor/etc/selinux/vendor_property_contexts
/vendor/etc/selinux/vendor_hwservice_contexts
/vendor/etc/selinux/vendor_keystore2_key_contexts
/vendor/etc/selinux/vndservice_contexts
```
作用：virtio 域（`pioneer`、graphics/minigbm/vulkan/keymint.sw/gatekeeper.sw/bluetooth.default/wifi 等）与对应 contexts。
> ⚠️ 不能只换 contexts 不换策略（官方 contexts 引用 278 个 MTK 类型在本策略缺失）。若 vendor_sepolicy.cil 换成本文件，需确认能对本项目 plat（MIUI system 的 plat_sepolicy.cil）编译通过。

### 8. fstab `/vendor/etc/`
```
/vendor/etc/fstab.virtio
/vendor/etc/fstab.virtio.gsi.sda
/vendor/etc/fstab.virtio.gsi.vdc
```
作用：virtio 磁盘（vda 单盘 / GSI 场景）分区挂载定义。

### 9. ueventd `/vendor/etc/ueventd.rc`
作用：virtio 设备节点（vport/virtio-ports、dri 节点等）的权限；官方 ueventd.rc 只有 MTK 节点。

### 10. WiFi 配置 `/vendor/etc/wifi/`
```
/vendor/etc/wifi/p2p_supplicant.conf
/vendor/etc/wifi/wpa_supplicant_overlay.conf
```

### 11. 辅助二进制 `/vendor/bin/`
```
/vendor/bin/setup_wifi          # init.virtio.rc / setup_wifi.rc 调用
/vendor/bin/virtgpu_detect      # GPU 后端探测
/vendor/bin/tablet2multitouch   # 触控适配
/vendor/bin/vndservice
/vendor/bin/modprobe            # vkms/virtio 模块加载（配合 /vendor/lib/modules）
```

### 12. HAL 依赖库 `/vendor/lib64/`
除上述外，至少还包括（完整闭包需集成时 ldd 解析）：
```
/vendor/lib64/libminigbm_gralloc.so
/vendor/lib64/libgralloctypes.so
/vendor/lib64/libhwc2onfbadapter.so
/vendor/lib64/libcuttlefish_utils_result.so
/vendor/lib64/libgatekeeper.so
/vendor/lib64/libkeymaster_portable.so
/vendor/lib64/lib_android_keymaster_keymint_utils.so
/vendor/lib64/libkeystore-engine-wifi-hidl.so
/vendor/lib64/libnl.so
/vendor/lib64/libprotobuf-cpp-full-21.12.so
/vendor/lib64/libprotobuf-cpp-lite-21.12.so
/vendor/lib64/libstagefright_foundation.so
/vendor/lib64/android.hardware.graphics.*.so（composer@2.1~2.4/mapper@2.0~4.0/allocator@2.0~4.0）
/vendor/lib64/android.hardware.gatekeeper@1.0.so
/vendor/lib64/android.hardware.security.keymint-V3-ndk.so
/vendor/lib64/android.hardware.bluetooth-V1-ndk.so
/vendor/lib64/android.hardware.wifi.*.so（common/hostapd/supplicant）
/vendor/lib64/android.hidl.memory*.so / android.hidl.memory.token@1.0.so
/vendor/lib64/libhidlmemory.so / libfmq.so / libbinder.so / libutils.so / libc++.so / libcutils.so / libprocessgroup.so
/vendor/lib64/cuttlefish_net.so
```

### 13. build.prop `/vendor/build.prop`
> lineage virtio vendor **无此文件**，需由本项目生成（`build/vendor/build/props.py` 已处理）：`ro.hardware.egl/vulkan=mesa`、`ro.hardware.hwcomposer=drm`、`ro.hardware.gralloc=minigbm`、`ro.zygote=zygote64` 等。

### 14. 内核模块（不在此 vendor 内）
`/vendor/lib/modules` 是链接，实际模块在 **vendor_dlkm** 分区——必须用 lineage virtio 版（官方是 MTK 模块，virtio 内核不认），与本 vendor 底座无关。

### 不需要移植
`/vendor/odm`（空）、`/vendor/apex`、`/vendor/pci.ids`、`/vendor/usr`、`/vendor/etc/{NOTICE.xml.gz,passwd,res,media_profiles_V1_0.xml,excluded-input-devices.xml,fs_config_files,build_flags.json}`（按需）。

---

## 二、加载方案：从「分区/文件系统层」到「内核层」的演进与结论

### 2.1 问题定义：应用验证 vendor 的“挂载特征”

实测当前运行实例（`/proc/mounts`）：
```
/dev/block/dm-3 /vendor ext4 ro,seclabel,noatime 0 0
ro.boot.verifiedbootstate = orange
ro.build.fingerprint = VirtIO/lineage_virtio_arm64only/...
```
即今天挂载特征**已不像官方**（官方应为 `/dev/block/mapper/vendor_a ... erofs ro`）。应用可观测的面：

| 观测面 | 载体 |
|---|---|
| 源设备 / fstype / 挂载 flags | `/proc/mounts`、`/proc/self/mountinfo`、`getfsstat/statfs` |
| superblock UUID | `mountinfo`、`statfs` |
| verifiedboot 状态 | `ro.boot.verifiedbootstate`、`ro.boot.flash.locked`（来自内核 cmdline / vbmeta） |
| 内容指纹 | `/vendor/build.prop` → `ro.build.fingerprint` |

### 2.2 分区/文件系统层方案（A/B/C）与各自暴露面

前提术语：厂商 `vendor_a.img`（gold_images 内原始 EROFS 文件）保持**零改动**。

| 方案 | 做法 | /proc/mounts 暴露 | 结论 |
|---|---|---|---|
| A 独立分区 | super 新增 `cdh_vendor_a` + fstab 改挂 `/vendor` | 源设备名 ≠ 官方 `vendor_a` ❌ | 设备路径校验不过 |
| B overlayfs | 官方 lower + 我们的 upper，挂 /vendor | `overlay on /vendor type overlay (lowerdir=…)` ❌ | 类型全暴露 |
| C qcow2 差异 | 宿主层块级 overlay（复用 daemon 机制） | guest 侧同 B，`overlay` 类型 ❌ | 类型全暴露 |
| 追加分区+重定向 `/vendor/etc/init` | vendor_patch.img + bind/直接挂到 `/vendor/etc/init` | 多 N 条 vendor_patch 挂载 ❌；且只重定向 init 不够（HAL bin/lib、VINTF manifest、sepolicy 仍需可达） | 等价“手动 bind 版 overlay”，暴露面更多 |

**结论：分区/文件系统层没有一条路能同时满足“官方挂载特征”与“加载我们的内容”**——挂载源与 fstype 由真实挂载决定，无法通过布局/挂载方式伪造。

### 2.3 EROFS 重建语义（dump→mkfs 实验结论）

若走“官方 EROFS 全量重建”（提取 → 改 → `mkfs.erofs`）：

| 项 | 结论（受控实验，odm_dlkm_a.img） |
|---|---|
| mtime | ✅ 可保：mkfs.erofs 默认按源树 mtime 写入（`touch -t` → 重建后原样保留）；`--preserve-mtime` 显式保 |
| inode（NID） | ❌ 必然重排：仅加一个文件重建，其余文件 inode 全部 +1（实测 45/48/51 → 46/49/52）；EROFS 无保留原 inode 的选项 |
| 加文件影响其他文件 | 物理布局（inode 元数据/数据块顺序）变，但**每个文件内容字节不变** |
| 原地编辑 | ❌ **不存在 debugfs 等价物**：EROFS 是只读压缩格式，无空闲块管理，增删必重排。工具链只有 mkfs/dump/fsck/erofsfuse（只读挂载） |

> 影响面：inode/布局变化对“应用验证 vendor”无实质影响（应用不查 inode）；真正会让整镜像校验失败的仍是**内容变化本身**（AVB/dm-verity 哈希），与 inode 无关，只能靠 vbmeta 关校验（当前 `orange` 说明本就不强制）。

### 2.4 最终方向：内核层挂载伪装（patch 集）

**为什么是内核**：`/proc/mounts`、`mountinfo`、`statfs`、文件属性全是内核输出的。改内核即可把这些观测面做成对应用完全透明，分区/文件系统层怎么加载都无所谓。

> 前提：先接通内核编译管线（`build-kernel.py` 骨架 → 编出可引导的 virtio GKI 6.6 内核，模块归集 `out/dist/kernel/modules` 自动接入 vendor_dlkm 注入路径）。当前 guest 内核为 lineage virtio GKI `6.6.66-4k-g8231513ea014`。

**P0 —— 核心（必须）**

| # | 文件/函数 | 行为 | 触发 |
|---|---|---|---|
| P0-1 隐藏挂载 | `fs/namespace.c`：`show_mountinfo()` / `show_vfsmnt()` / `show_mountstats()` | 按路径前缀或源设备匹配，命中即 `SEQ_SKIP` | cmdline `cdh_mount_hide=/mi_ext,/dev/block/vdc` |
| P0-2 改写 `/vendor` 行 | 同上两函数 | 挂载点==`/vendor` 时打印官方字样：源 `mapper/vendor_a`、fstype `erofs`、flags `ro,seclabel,noatime`；mountinfo 的 major:minor/root 取官方真实值（构建期 lpdump 提取） | cmdline `cdh_mount_spoof=/vendor:/dev/block/mapper/vendor_a:erofs:ro` |
| P0-3 statfs 类型 | `fs/overlayfs/super.c`：`ovl_statfs()` | 强制报 lower（erofs）的 `f_type`（0xE0F5E1E2），防上层 ext4 泄漏 | 同 P0-2 开关 |

**P1 —— 可选**

| # | 文件/函数 | 行为 |
|---|---|---|
| P1-1 inode 假映射 | `fs/erofs/`：`erofs_iget()`/`erofs_readdir()` | 按构建期生成的映射表把新 inode 翻译回官方值（默认不做——应用不查 inode） |
| P1-2 /proc/version 伪装 | `kernel/version.c`：`init_uts_ns` | 改写 release/version 串为官方口径（若应用做内核版本白名单） |

**无需内核 patch**

| 项 | 处理 |
|---|---|
| `ro.boot.verifiedbootstate` 等 | QEMU 启动参数追加 `androidboot.verifiedbootstate=green androidboot.flash.locked=1 …`（cmdline 由我们控制，`/proc/cmdline` 与 init 派生属性天然一致） |
| mtime | `mkfs.erofs --preserve-mtime` |
| SELinux | 非内核问题：注入文件按 `vendor_file`/`vendor_configs_file` 打 label（复用现有 selinux.py） |

**约束与验证**
- Patch 全部走 cmdline 开关，**默认关**（不传 `cdh_*` 时与原生内核行为一致）；不引入导出符号变化 → **不破 GKI KMI**；模块 vermagic 须与重建内核匹配（自然接入 vendor_dlkm 注入）。
- 验证清单：挂 vendor_patch/overlay 后 `cat /proc/mounts` 只见官方一行；`df -T /vendor`、`stat -f /vendor` 报 erofs + 官方 UUID；virtio HAL 正常运行（只是“看不见”）；默认关回归一致。
- 兜不住：无 TEE 的硬件级 attestation（Play Integrity 强校验，virtio 无 TEE）；官方 vbmeta 签名链（不锁 boot 时本就不强制，现 orange）。

### 2.5 note13-5G-15 实际落地：vdc 挂载点选 /mi_ext

当前 note13 采用「官方 super.img 字节级零改动 + lineage virtio 内容独立 vdc 分区」：

- 官方 system/vendor **原样挂载**（erofs，inode/内容不变，真机身份视角）。
- lineage vendor/vendor_dlkm 修补 → patch.img，QEMU 以 `vdc` 挂载。
- **挂载点选官方 system_a 已存在的空目录 `/mi_ext`**：Android init 在 switch root
  （rootfs=/system）后才挂载该分区，挂载点必须已存在于官方 system_a（erofs 只读）。
  - `/patch`：官方无此目录 → `failed to realpath(/patch)` → InitFatalReboot。
  - `/debug_ramdisk`：目录存在、第一阶段可用，但 init **第二阶段**会
    `mount("tmpfs","/debug_ramdisk",...)`（system/core/init/setup_mounts.cpp）遮蔽它，
    第二阶段重定向读全部 ENOENT（`Failed to read /vendor/etc/selinux/plat_sepolicy_vers.txt`）。
  - `/mi_ext`：官方 super 中 mi_ext_a 为 0MB（无内容），MIUI 的 system/product/system_ext
    init 脚本均不 mount/mkdir 它 → 安全。
  - init 会挂 tmpfs 的目录（须避开）：`/mnt`、`/second_stage_resources`、`/apex`、
    `/bootstrap-apex`、`/dev`、`/linkerconfig`、`/debug_ramdisk`。
- 内核 `getname_flags` 重定向：`/vendor -> /mi_ext/vendor`（白名单精确匹配，
  见 kernel/source/fs/namei.c CONFIG_CDH_PATH_REDIRECT）。
- **ODM 策略屏蔽**：官方 `/odm/etc` 是指向 `/vendor/odm/etc` 的符号链接，官方 vendor 的
  `odm_sepolicy.cil` 引用 MTK 专属类型（thermal_data_file/teei_data_file 等），与我们实际
  加载的 lineage vendor 策略不兼容 → secilc `Failed to resolve allow statement at
  /odm/etc/selinux/odm_sepolicy.cil:1` → init 重启。内核白名单加一条把该文件指到不存在的
  `/mi_ext/odm/...`（ENOENT → init 跳过 odm 策略）；其余 odm 内容保持官方原样。
- **patch.img 根目录标签**：vdc 挂载后 `/mi_ext` 根 inode 必须带 `u:object_r:vendor_file:s0`
  （`build-patch.py` 的 `ROOT_META`），否则 mount 根 unlabeled，各域 traverse
  `/mi_ext/vendor/...` 被 SELinux 拒 → 连锁表现为 linker 找不到库、VINTF Permission denied。
- **update_verifier 屏蔽**：MIUI `init.rc` 的 `on zygote-start: exec_start update_verifier`
  会阻塞动作队列，而该服务需要 A/B bootctrl HAL（lineage virtio vendor 不提供），取不到即
  `sys.powerctl=reboot` → ~7.5s 引导死循环。内核白名单把 `/system/bin/update_verifier` 指到
  不存在路径（exec ENOENT → init 跳过）。
- 引导链：vendor_boot 的 platform fragment 内含 lineage 原版 fstab.virtio（含 odm 挂载），
  initrd 拼接时晚于 boot.img ramdisk 会覆盖我们注入的 fstab → 由 build-vendor-boot.py
  删掉 fragment 内 fstab 副本，boot.img 的 fstab 成为唯一来源（删 odm、vdc 挂 `/mi_ext`）。
- 可见性：`vdc /mi_ext` 挂载条目仍出现在 `/proc/mounts`（云机特征），
  由 P0-1 挂载隐藏后续接入（cmdline 隐藏目标 `/mi_ext`）。

**2.4 最终方向（挂载伪装 patch 集）与此处落地（getname_flags 重定向）的关系**：
P0-1 隐藏挂载 / P0-2 改写 /vendor 行是「彻底伪装」的后续增强；本落地先保证
「官方 super 零改动 + 系统功能可用」，挂载表伪装留作 P0-1/P0-2。

---

## 三、落地对照（当前项目）

- 现状：`pack_super.py` 用 `dd` 把 lineage vendor 写入 super 的 `vendor_a` 分区（改的是 super 内副本，官方 `vendor_a.img` 文件未动）——分区内容已替换，挂载特征（ext4）与官方（erofs）不符，正是 2.1 的问题。
- 已具备：`debugfs` 注入工具层、`vendor_builder.py` 编排、`sepolicy.py`（pioneer 域 + `validate_cil`）、`props.py`（build.prop 渲染）、`vendor_dlkm` 基线对齐。
- 缺口：
  1. 内核编译管线（`build-kernel.py` 骨架；源码 `kernel/source` 未就位）——挂载伪装 patch 的前提；
  2. 挂载伪装 patch 集（P0-1/P0-2/P0-3 → P1 按需）挂进 GKI 6.6 源码；
  3. QEMU 启动参数追加 `androidboot.*`（daemon `-boot` 接入点）；
  4. （若走 EROFS 重建）官方 vendor 提取能力（宿主 erofs-utils 1.9.2 无 extract，需新版或逐文件 dump）。
