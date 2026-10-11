# Pixel 6（oriole）Android 15 云手机镜像制作手册（cook.md v3）

本文回答两个问题，全部到**逐分区 × 逐文件**粒度：

1. **每个分区的每个文件来自哪里、被添加/删除/修改了什么**（含 mode/uid/gid/
   SELinux 标签/二进制偏移/十六进制前后值/length/sha256）——§2～§6；
2. **本仓 `devices/google/pixel6-15/build/` 如何把这些对齐成一条可执行的制作流程**
   （8 阶段流水线，每阶段：输入→命令→输出→校验→幂等性）——§7。

> 事实来源（代码即证据，本文所有数字均可回溯）：
> - 基线仓 `/Volumes/work/cloud-droid-hub`：`justfile`（Stage A）、
>   `internal/cdhapp/device_run.go`（Stage B 16 步）、
>   `cmd/gsifix/{slim_base,boot,boot_firststage_fstab,patch_device}.go`、
>   `cmd/gsifix/vendor_policy.go`（vendorPolicyCore）、`cmd/gsifix/pixel_props.go`、
>   `devices/pixel6-15/device/{gsi_boot,gsi_properties,gsi_property_bundle,gsi_generated_files,vendor_policy}.go`、
>   `internal/creator/datadisk/run.go`（data.qcow2）。
> - 本仓 `/Volumes/work/cloud-phone-hub`：`build/` 下 9 个脚本
>   （main / unpack-roms / build-kernel / build-boot / build-vendor-boot /
>   build-vendor / build-vendor_dlkm / pack_super / pack-roms）。
>
> 术语约定：**来源**=分区数据从哪个镜像复制；**新增**=镜像内注入的文件；
> **删除**=从基底镜像中移除的文件；**修改**=对已有文件就地改写；
> **不动**=逐字节原样保留。

---

## 1. 三盘模型与总装结构

| 盘 | 文件 | 大小 | 角色 |
|----|------|------|------|
| vda | `base.qcow2`（本仓 `vda.img`） | 5 GiB GPT | 引导盘 + super 动态分区（system/product/system_ext 被 slim 清零，vendor/vendor_dlkm/odm/system_dlkm 留存） |
| vdb | `data.qcow2` | 110 GiB f2fs | 用户数据盘（mkfs 时即带种子目录与 file_contexts，非空盘） |
| vdc | `system.qcow2` | 整盘 | factory 官方 `system.img` 整盘 + GSI 属性/文件补丁 = 系统盘 |

**GSI-from-vdc 设计**：grub 引导参数 `androidboot.fstab_suffix=virtio.gsi.vdc`
→ vold 命中 **vda 内 vendor LP 分区**的 `/etc/fstab.virtio.gsi.vdc` → 该 fstab 把
`/system` 挂到 `/dev/block/vdc`。super 内 system/product/system_ext 虽仍在 LP
表中占位，但内容已被 slim-base 清空（0x00），系统分区实际由 vdc 承载。

**两条生产链**（同一镜像语义的两种实现）：

| | 基线链（cloud-droid-hub） | 本仓链（cloud-phone-hub build/） |
|---|---|---|
| 载体 | 就地在 qcow2/raw 上打补丁（debugfs/mcopy/gsifix） | 先产出各分区裸 img，再 liblp/GPT 从零组装 |
| 阶段 | Stage A（justfile 3 步）→ Stage B（cdh device-prepare 16 步）→ Stage C（patchDevice 5 步） | 8 阶段：unpack→kernel→boot→vendor_boot→vendor→vendor_dlkm→super→pack |
| 产物 | `base.qcow2`/`data.qcow2`/`system.qcow2` | `vda.img`/`data.qcow2`（super 内含完整 7 分区；vdc 不在本仓产出） |

---

## 2. vda：GPT 物理分区布局与逐分区清单

### 2.1 GPT 分区表（usable LBA 34..10485726，128 entries）

| # | 分区 | LBA 起–止 | 大小 | GUID（type） | 来源 | 内容改动 |
|---|------|-----------|------|--------------|------|----------|
| 1 | EFI | 2048–526335 | 256 MiB | `28732ac1-...`（ESP） | lineage `EFI.img` | 仅覆写 `/boot/grub/grub.cfg`（§2.2） |
| 2 | super | 526336–8914943 | 4096 MiB | ANDROID_SYS | lineage super（slim 清 3 分区）+ 官方/vendor 注入 | §3 逐 LP 详表 |
| 3 | misc | 1 MiB | 1 MiB | ANDROID_MISC | lineage 原样 | 前 4096 B BCB 清零（§2.8） |
| 4 | metadata | 32 MiB | 32 MiB | ANDROID_META | lineage 原样 | 非标准 ext4 时整盘重格（§2.9） |
| 5 | cache | 50 MiB | 50 MiB | ANDROID_CACHE | lineage 原样 | 删 `/recovery/command`（§2.10） |
| 6 | boot | 64 MiB | 64 MiB | ANDROID_BOOT | lineage boot.img | 换内核 + 注 20 个 fstab（§2.3） |
| 7 | recovery | 64 MiB | 64 MiB | ANDROID_RECOVERY | lineage 原样 | **不动** |
| 8 | firmware | 128 MiB | 128 MiB | ANDROID_FW | lineage 原样 | **不动** |
| 9 | persist | 16 MiB | 16 MiB | ANDROID_PERSIST | lineage 原样 | **不动** |
| 10 | BIOS | 4 MiB | 4 MiB | `48616821-4964-6f6e-744e-65656445-4649`（"dontNEEDEFI"） | lineage 原样 | **不动** |
| 11 | vendor_boot | 100 MiB | 100 MiB | ANDROID_VB | lineage vendor_boot.img | 剔 3 个隐藏 bootconfig 键（§2.4） |

分区顺序刻意与 lineage 基线一致（**recovery 之后紧跟 firmware**）。GPT 组装细节：
cursor 从 LBA 2048 起、每分区 1 MiB 对齐；分区 GUID 用
`uuid5(NAMESPACE_DNS, 分区名)` 生成（跨构建稳定）；protective MBR 在字节
450 写 0xEE、510–511 写 0x55 0xAA（**小端字节序，先 0xAA 后 0x55 逆序写入**）；
主备两份 GPT 头各带 CRC32。

### 2.2 分区 1（EFI）：grub.cfg

- **来源**：lineage UTM 盘的 EFI.img（含 grub aarch64 EFI + 既有目录结构），整分区复制。
- **修改**（仅 1 个文件）：`/boot/grub/grub.cfg` —— `mcopy -o -i` 覆写为内联常量
  `GRUB_CFG_CONTENT`：
  - banner `CDH_GRUB_BANNER_9518`；
  - `kms_display_resolution=1080x2400`、`ab_enable=false`；
  - `boot_android` 函数内 `android_boot_img_cmdline`：
    `console=ttynull stack_depot_disable=on cgroup_disable=pressure kasan.stacktrace=off
    kvm-arm.mode=protected bootconfig ioremap_guard log_buf_len=4M loop.max_part=7
    printk.devkmsg=on rw vt.global_cursor_default=0
    androidboot.fstab_suffix=virtio.gsi.vdc` + vendor_boot 侧 `console=ttyAMA0`；
  - **默认菜单项 gsifromvdc**：附加 39 项 oriole 硬件身份 cmdline（完整清单见 §2.2.1）。
- 其余文件（grubx64.efi 等）**不动**。

#### 2.2.1 grub 身份 cmdline（39 项，逐字对齐 `gsi_boot.go` `pixel6Android15BootIdentityKernelArgs`）

```
androidboot.hardware=oriole
androidboot.bootloader=slider-15.3-13239612
androidboot.color=RED
androidboot.coo=CN
androidboot.ddr=8GiB,Samsung,LPDDR5,0108
androidboot.devcfg=G950-07197-01
androidboot.pcbcfg=G650-07839-05
androidboot.platform=gs101
androidboot.revision=MP1.0        （platform_revision / hardware_revision 两个键同值）
androidboot.ufs=128GB,SKHynix
androidboot.ddr_info=Samsung
androidboot.ddr_size=8GiB
androidboot.primary_panel_drv=panel-samsung-s6e3fc3
androidboot.hw.soc.id=09b64fbc2110
androidboot.hw.soc.nonsec-ar=6
androidboot.hw.soc.rev=3
androidboot.hw.soc.sec-ar=3
androidboot.avb_version=1.3
androidboot.vbmeta.avb_version=1.3
androidboot.vbmeta.device=PARTUUID=3928c12b-4a24-452e-92f0-b15030a38aeb
androidboot.vbmeta.device_state=locked
androidboot.vbmeta.hash_alg=sha256
androidboot.vbmeta.size=16768
androidboot.vbmeta.digest=fe049d35e450d9faf4b26788864f7b5fd5b857276d5271b067ef0e0268acff6f
androidboot.verifiedbootstate=green
androidboot.flash.locked=1
androidboot.secure_boot=PRODUCTION
androidboot.veritymode=enforcing
androidboot.veritymode.managed=yes
androidboot.force_normal_boot=1
androidboot.load_modules_parallel=true
androidboot.hypervisor.version=kvm-arm-nvhe
androidboot.serialconsole=0
androidboot.dtbo_idx=0
androidboot.dpm_state=0x0000
androidboot.debug_level=0x4948
androidboot.cdt_hwid=0x00020306000100000100000000000000
```

### 2.3 分区 6（boot）：boot.img 重制（build-boot.py 440 行）

**基底**：lineage `boot.img`（Android boot image **v4** header：magic `ANDROID!`@0、
kernel_size@8、ramdisk_size@12、os_version@16、header_size@20、page 4096；内嵌
first-stage ramdisk 非空——EDK II 可作 EFI-stub 直启）。

**修改 1 —— 内核替换**：kernel 段整段替换为本仓自编译 `out/kernel/Image`
（内核 6.6.127-4k-cdh 系，builder 见 §7 阶段 2）；重排时 kernel 段页对齐、
清掉中间空洞、ramdisk 重新居后；boot 分区上限 `MAX_BOOT_PARTITION_BYTES=256MiB`。

**修改 2 —— first_stage ramdisk 注入 20 个 fstab + 5 个目录**：解出内嵌 ramdisk
（`lz4 -d`）→ 解析 newc cpio → 在 TRAILER 之前按**绝对偏移 mod 4 对齐**追加条目
（inode 从 100000 递增；目录 040755、文件 0100644）→ 追加 `TRAILER!!!` →
`lz4 -12 -l` 重压。文件名 × 路径矩阵：

| fstab 名 | 根 | system/etc/ | first_stage_ramdisk/ | first_stage_ramdisk/system/etc/ |
|---|---|---|---|---|
| fstab.virtio | ✔ | ✔ | ✔ | ✔ |
| fstab.virtio.gsi.sda | ✔ | ✔ | ✔ | ✔ |
| fstab.virtio.gsi.vdc | ✔ | ✔ | ✔ | ✔ |
| fstab.oriole | ✔ | ✔ | ✔ | ✔ |
| fstab.gs101 | ✔ | ✔ | ✔ | ✔ |

= **20 个 cpio 文件（0100644）+ 5 个目录（040755）**（已有同名旧条目先剔除再
追加，保证幂等），内容统一为内联常量 `FIRSTSTAGE_FSTAB`：EFI/misc/metadata/
firmware/persist 各 1MiB 级、CD-ROM/USB/SD、share 的 VirtioFS+9P 双写、
`/data` = vdb ext4+f2fs 双写（带 `fileencryption=` `keydirectory=`）、super 7 个
逻辑分区（ext4+erofs 双写）、cache/boot/vendor_boot/recovery 各自挂点。
5 个名字是 vold 在不同 fstab_suffix 下探测的并集，保证任何 suffix 都命中。

**修改 3 —— os_version**：header offset 16 写 `ALIGNED_OS_VERSION=0x1E00019A`
（Android 15）。

**输出**：原子写 `out/dist/rom/boot.img`（先写 `.new.{pid}.{rand}` 临时件再
`os.replace`）。

### 2.4 分区 11（vendor_boot）：隐藏键剔除（build-vendor-boot.py 159 行）

**基底**：lineage `vendor_boot.img`（v4：magic `VNDRBOOT`@0、header_version@8、
page_size@12、ramdisk_size@24、header_size@2096、dtb_size@2100、3 张表
@2112/2116/2120、bootconfig_size@2124）。

**修改**：bootconfig 区域逐行扫描，删除以下 3 个键所在行，区域整体前移并
清零尾部，`bootconfig_size` 重写：

| 被删键 | 原因 |
|---|---|
| `androidboot.console` | 防止内核日志走串口抢占 |
| `androidboot.first_stage_console` | 同上 |
| `androidboot.theme` | 触发 A/B 主题与 virtual AB 冲突 |

**幂等**：无隐藏键时逐字节原样复制（可重复执行）。其余字段（vendor ramdisk、
dtb、表区）**不动**。输出 `out/dist/rom/vendor_boot.img`。

> 实测（2026-09）：lineage 源 bootconfig 仅含 console/first_stage_console 两键
> （theme 缺失，扫描 3 键命中 2）；剔除后产物与基线盘 vendor_boot 分区
> **逐字节一致**（整镜像 sha256 `2791e956…a387`，bootconfig 276→217 B，
> vendor ramdisk 4128290 B / ramdisk table / dtb(0) 各段 sha 与基线盘相同）。

### 2.5 分区 7/8/9/10（recovery / firmware / persist / BIOS）

全部**逐字节不动**（来源 lineage vda.img 对应分区）。recovery 是 lineage 自带
GKI recovery；firmware 满足 vendor 侧 radio/modem 固件路径；BIOS 是 "dontNEEDEFI"
占位类型，保留仅为了 LBA 布局与基线一致。

### 2.6 分区 3（misc）：BCB 清零

前 4096 字节（BCB / bootloader control block）全零——去掉 lineage 遗留的
recovery 一次性行命令，防止首启进 recovery 模式。其余**不动**。

### 2.7 分区 4（metadata）：ext4 保障

读 superblock（offset 1024，s_magic@56 应为 0xEF53）；若非 ext4 则
`mke2fs -t ext4 -b 4096 -I 256 -m 0` 整分区重格（Android SDK platform-tools
内的 mke2fs）。已是 ext4 则**不动**（vold 会自挂 /metadata 用作 checkpoint）。

### 2.8 分区 5（cache）：删 1 个文件

debugfs 删除 `/recovery/command`（lineage 遗留的 recovery 指令文件）。其余
**不动**。

## 3. super.img（分区 2，4096MiB）——7 个 LP 逻辑分区逐文件来源与修改

### 3.0 liblp 元数据结构（pack_super.py 从零组装）

super 镜像 = `liblp`（logical partition）容器：物理上是一块 6GiB 的裸区域，前 12KiB
是元数据，其后按 1MiB 对齐顺序排布 7 个 LP 分区的文件系统镜像。pack_super.py
**不依赖 lpmake**，用纯 Python 写出元数据 blob 再拼接各分区：

| 元数据结构 | 值 | 说明 |
|---|---|---|
| geometry magic | `0x616C4467`（"lLdg"→`\x67\x44\x6C\x61` LE） | 写**两份**：@4096 与 @8192，共 2 个副本 |
| geometry 尺寸 | 4096 B（`GEOMETRY_SIZE=4096`） | 每副本占 1 个 4KiB 扇区 |
| header magic | `0x414C5030`（"ALP0"） | @12288（`meta_start=12288`） |
| header 版本/尺寸 | liblp v10.2，header 256 B（`LP_METADATA_HEADER_SIZE=256`） | |
| metadata 最大尺寸 | 65536 B（`METADATA_MAX_SIZE=65536`） | 含 header+分区表+区段表+组表 |
| 逻辑块大小 | 4096 B（`LOGICAL_BLOCK_SIZE=4096`） | extents 以此计 |
| 首个逻辑扇区 | 2048（`FIRST_LOGICAL_SECTOR=2048`） | = 12288+65536 后向上对齐 4KiB |
| 分区对齐 | 1MiB（`ALIGNMENT=1048576`） | 所有 extents 起始 LBA 对齐 |
| 元数据槽 | 2 槽 × 2 副本 = 4 份 | 主备/AB 双冗余 |
| 分区组 | `default`（unlimited）+ `virt_dynamic_partitions`（= super−4MiB，上限） | 后者是虚拟 A/B 动态分区组 |
| SUPER_SIZE | `6*1024**3` = 6GiB | **基线为 4GiB 装不下 factory 件**，扩到 6GiB |

打包动作（`pack_super.main`）：对 7 个 LP 输入镜像各建一个
`Extent(offset=起始LBA, num_sectors=按 4096 向上取整)` → 组表/分区表/区段表序列化
→ header+几何写盘 → 逐个把分区镜像 4KiB 边界复制进 super 主体 → 输出
`out/dist/super.img`。

### 3.1 七个 LP 分区镜像来源总表（pack-roms.py `SUPER_PARTITIONS`，有序）

| # | LP 分区 | 镜像来源（本仓链） | 处理方式 |
|---|---|---|---|
| 1 | `system_dlkm` | factory `roms/image-oriole-bp1a.250505.005/system_other.img` | **原样**（只重命名用途） |
| 2 | `vendor_dlkm` | `out/dist/rom/vendor_dlkm.img`（build-vendor_dlkm.py 自产） | 重打包（§3.3） |
| 3 | `product` | factory `roms/image-oriole-bp1a.250505.005/product.img` | **原样** |
| 4 | `system` | factory `roms/image-oriole-bp1a.250505.005/system.img` | **原样**（本仓 vdc 体系下 super 内 system 仅作 fallback 启动项 `gsifromsda`） |
| 5 | `system_ext` | factory `roms/image-oriole-bp1a.250505.005/system_ext.img` | **原样** |
| 6 | `odm` | lineage `lineage-22.2/super/odm.img` | **原样**（unpack-roms.py 拆出，无补丁） |
| 7 | `vendor` | `out/dist/rom/vendor.img`（build-vendor.py 自产） | **lineage vendor.img 基座重打包**（§3.2.2） |

> **关键架构差异（§7 展开）**：基线链（cloud-droid-hub）super 内
> system/product/system_ext 来自 lineage GSI 并经 gsiPatchBundle 改写、vendor 来自
> lineage vendor + 16 段脚本补丁；本仓链（cloud-phone-hub build/）super 内
> factory 四件+lineage odm 全部**原样**，仅 vendor / vendor_dlkm 自产，
> GSI 改写整体让渡给 vdc 盘（§4）。

### 3.2 vendor（384MiB 级）——两链同一 lineage 基座、两种做法

#### 3.2.0 基座选择：两条链现共用 lineage vendor，注入深度不同

| | 基线链（cloud-droid-hub device/） | 本仓链（cloud-phone-hub build/） |
|---|---|---|
| 基座镜像 | lineage `vendor.img`（lineage 编译的 oriole vendor） | **同一 lineage `vendor.img`**（`lineage-22.2/super/vendor.img`，virtio_arm64only super 拆出件；官方 factory `vendor.img` 降级为提取源：GPU 临时库 + Widevine 恢复 + 基线对齐官方件） |
| 改造思路 | 16 段脚本把 factory vendor 里需要的配置/固件/HAL「**搬回来**」 | 同一基座，复刻「自研 HAL + goodix ko + Widevine APEX + 官方件回填 + 自生成件」全集（单会话注入，表 A–G）；vintf 声明/合并总 rc/rro 三类刻意不复刻，剩余差异见 §3.2.2.6 |
| 工具 | gsifix 管道（debugfs/sif/ea_set） | build-vendor.py（debugfs 单会话注入 + resize2fs 扩容） |
| 输出 | vda 内 super 的 vendor LP | `out/dist/rom/vendor.img` |

#### 3.2.1 基线链 16 段补丁执行序（vendor_policy.go L97–114 `PatchScript` 拼接顺序）

| 序 | 段名（Go 片段） | 作用一句话 | 详表 |
|---|---|---|---|
| 1 | preAudioPatch | 删 7 个杂 bin + netmgr 四件套 + init.oriole/init.virt rc 改写 + sepolicy −4/+17 + 官方 13 XML 回填 | §3.2.1-a |
| 2 | audioAPEXGuard | audio APEX 三重校验 + 删 mounton + 清旧派生服务 | §3.2.1-b |
| 3 | widevinePolicy | widevine CIL 21 行 + service_contexts 追加 | §3.2.1-c |
| 4 | vendorPostAudio | public.libraries + GPU 6 扫描库 + sensors/vibrator/thermal/nfc 四 HAL + runtime sepolicy 3 条 | §3.2.1-d |
| 5 | widevineAPEX | widevine APEX 逐字节落位 | §3.2.1-e |
| 6 | updateEngine | system 侧 update_engine rc → vendor compat 入口 | §3.2.1-f |
| 7 | GPUServiceVenusPatchScript（pixelvendor 共享段） | gpuservice 身份改 Venus | §3.2.1-g |
| 8 | VulkanRuntimeIdentityPatch | vulkan.virtio.so 设备名改 Mali-G78 | §3.2.1-h |
| 9 | VendorAfterGPUServicePatch（内含共享 HAL 大段） | 删 execmem + hvc2–5 设备面 + 共享 7 bin + Remote KeyMint 替换 + goodix KO + cdh-pixel6-hal.xml/rc | §3.2.1-i |
| 10 | rroPatch（vendor_rro.go） | 3 个 RRO 更名迁移 + 1 删除 | §3.2.1-j |
| 11 | RadioPatch | radio HAL service + 属性/seapp/CIL 双域 | §3.2.1-k |
| 12 | tensorPatch（tensor_patch.go） | vendor /build.prop −set 约 60 键 −unset 38 键 | §3.2.1-l |
| 13 | VendorDLKMPatch | vendor_dlkm 官方 build.prop 回填 | §3.2.1-m |
| 14 | wifiScanPolicy（wifi_scan_policy.go） | cdh_wifi_scan_param CIL | §3.2.1-n |
| 15 | WifiPatch | wifi HAL bin+manifest+rc+build.prop+5 模块索引 | §3.2.1-o |
| 16 | codec2V4L2ServicePatch（codec2_v4l2_service.go） | vendor media_codecs_c2 注入 + system 侧 3 件 | §3.2.1-p |

> 幂等护栏：`MarkerFragments` 共 75 个（`pixel6-15.vendor-init-oriole-virtio-v1` …
> `vendor-radio-hal-service-v8`），任一段已打过 marker 即跳过；
> `VendorExt4MinFreeInodes=128` 保证扩容后空闲 inode 充足；所有官方件回填前先
> 比对 length+sha256，回填后进 `vendor_policy_required_paths`（widevine 还进
> `required_sha256`）终检。

#### 3.2.1-a 段 1 preAudioPatch（模板 L537–1068）

**删除（释放 inode，7 个 bin）**：

| 路径 | 原因 |
|---|---|
| /bin/cuttlefish_sensor_injection | cuttlefish 注入工具，无对应设备 |
| /bin/memeater | cuttlefish 内存压测 |
| /bin/gpioset / /bin/gpiodetect / /bin/gpiofind / /bin/gpioget / /bin/gpioinfo | libgpiod 5 件，无 GPIO 背板 |

**改写（2 个 init rc）**：

| 路径 | 来源 | 修改 | mode/uid/gid/标签 |
|---|---|---|---|
| /etc/init/hw/init.oriole.rc | base vendor `/etc/init/hw/init.virtio.rc` cat 出来 | `__PIXEL6_DROP_SERIAL__` 过滤器删 `service seriallogging` 段（校验残留即 exit 9） | 0x81a4 / 0 / 0 / vendor_configs_file |
| /etc/init/hw/init.virtio.rc | 同文件自身 | sed 删 `setprop ro.cpuvulkan.version` 行（防 GSI 侧 ro.cpuvulkan.version 被运行期打回） | 0x81a4 / 0 / 0 / vendor_configs_file |

**新增（netmgr 四件套，ADB 网络域隔离核心）**：

| 路径 | 内容 | mode/uid/gid/标签 |
|---|---|---|
| /bin/netmgr_setup | 生成的 sh：五模式 `cellular`（写入 10.0.2.15/24、路由 10.0.2.2）/ `setup`（建 netns adb_net_bootstrap、eth0 入域）/ `finalize`（等 vendor.netmgr_keeper pid → 软链 /proc/PID/ns/net → /mnt/run/adb_net）/ `configure`（lo+eth0 up）/ `keep`（死循环保 namespace） | 0x81c0 / 0 / 0 / setup_wifi_exec |
| /etc/init/netmgr.rc | 5 个 service（netmgr_setup/cellular_setup/netmgr_configure/netmgr_finalize/keeper，keeper 带 `enter_namespace net /mnt/run/adb_net_bootstrap`+seclabel u:r:setup_wifi:s0）+ `__PIXEL6_ADB_INIT__` 展开的 adbd 域段 + on post-fs/post-fs-data 触发 | **0x8180 (0600)** / 0 / 0 / vendor_configs_file |
| /bin/adbd_netns_launcher | guest-helper 预编译件 `output/guest-helper/hal/pixel6/android15/cdh-pixel6-adbd-netns-launcher` | 0x81e8 / 0 / **2000** / adbd_exec |
| /bin/adb_auth_loader | guest-helper `output/guest-helper/hal/pixel6/android15/adb_auth_loader`（ADB 公钥授权 helper） | 0x81e8 / 0 / **2000** / adbd_exec |

**sepolicy（内存过滤 ${filtered}，非镜像内文件）**：删 4 条 setup_wifi 旧域规则
（tmpfs/nsfs/proc_filesystems 的 mounton 套件）；增 17 条 = setup_wifi 13 条
（self sys_admin、init_svc_debug_prop 读、tmpfs_202404 目录/lnk/file 六件、
filesystem unmount、proc_filesystems_202404、system_file_202404 execute、
netlink_route_socket、unlabeled_202404 两条）+ adbd 4 条（self sys_admin、
sysfs_202404 mounton、selinuxfs_202404 mount、adbd_exec execute_no_trans）。

**删除（4 个多余 feature XML + 1 过期权限）**：
`/etc/permissions/android.hardware.ethernet.prebuilt.xml`、
`android.hardware.sensor.ambient_temperature.xml`、
`android.hardware.sensor.hinge_angle.xml`、
`android.hardware.sensor.relative_humidity.xml`（真机 PackageManager 清单对齐）；
`android.hardware.hardware_keystore.xml`（被 V3 取代）。

**回填（官方 factory vendor.img → lineage vendor，13 个 XML，全部先校验 length+sha256）**：

| 路径 | length | sha256 | 落位元数据 |
|---|---|---|---|
| /etc/permissions/android.hardware.camera.full.xml | 1045 | 5c2d543329b61b79…e61cf | 0x81a4/0/0/vendor_configs_file + 4×sif 0x495c0780 |
| /etc/permissions/android.hardware.camera.raw.xml | 912 | 18edf1d6ff694c40…2abe06 | 同上 |
| /etc/permissions/android.hardware.hardware_keystore_V3.xml | 765 | 30583fcccca1bb7c…f41805 | 同上 |
| /etc/permissions/android.software.verified_boot.xml | 753 | a9d903b0239520d9…d7890f | 同上 |
| /etc/media_codecs_aosp_c2.xml | 12975 | 9f4f798336c7a899…508c93 | 0x81a4/0/0/vendor_configs_file |
| /etc/media_codecs_c2.xml | 2584 | 9f9807918a1527d4…5124e | 同上 |
| /etc/media_codecs_dolby_c2.xml | 1441 | a46b958a79d40c2e…c428a | 同上 |
| /etc/media_codecs_performance_c2.xml | 17238 | 3dec4644f3212644…d64c2 | 同上 |
| /etc/media_codecs_performance.xml | 697 | 5996ec1eb986d952…8a8596 | 同上 |
| /etc/media_codecs.xml | 3258 | b3783c0efdc7c98e…e850a0 | 同上 |
| /etc/media_profiles_V1_0.xml | 37643 | a8238e51ce62e3a1…032d8 | 同上（校验含 cameraId="3"、2160p） |
| /etc/audio_policy_configuration.xml | 17682 | 68424af89b4482f9…76d87 | 同上（校验 Earpiece/Speaker Safe 等片段） |
| /etc/audio_policy_volumes.xml | 13748 | 63d7b49f0efe8710…893386 | 同上 + 4×sif 0x495c0780（base 双态：6732440a… 原版或 63d7b49f… 已打均接受） |

伴随删除：`/etc/media_profiles_vendor.xml`（避免 CDH-only 录像能力混入）；
`/etc/permissions` 目录本身 4×sif 0x495c0780。

#### 3.2.1-b 段 2 audioAPEXGuard（L1070–1146，只校验不落盘）

三重 sha 链校验 base vendor 的 audio APEX 完整性（不做写入）：
`/apex/com.android.hardware.audio.apex` 207298586→**20729856** B /
54d84e2d260fcbde…3dab9 → `unzip -p apex_payload.img` 20602880 B /
76ea07dd5212cd11…070a9 → payload 内 `/bin/hw/android.hardware.audio.service-aidl.example`
1048936 B / dbd9ddca38ab3d88…c31212。任何一环不符 exit 9。
随后删 base vendor 派生旧件 `/bin/hw/android.hardware.audio.service-aidl.example`
（先校验其 sha = e57718c12b02fd32…8c786，未知内容拒删），并从 ${filtered}
剔除 `(allow init hal_audio_default_exec (file (mounton)))`。

#### 3.2.1-c 段 3 widevinePolicy（L200–245）

- sepolicy ${filtered} 追加 21 行 CIL：`hal_drm_widevine`/`hal_drm_widevine_exec`
  双类型 + domain/halserverdomain/hal_drm(_server)/file_type/exec_type/vendor_file_type
  属性集 + init_202404 transition/execute/siginh/dontaudit + vendor_data_file_202404
  file/dir 全套 + dmabuf_system_heap_device_202404。
- **改写 /etc/selinux/vendor_service_contexts**：cat 出现有内容 → 追加一行
  `android.hardware.drm.IDrmFactory/widevine    u:object_r:hal_drm_service:s0`
  → rm+write+sif 0x81a4/0/0/ea_set vendor_configs_file。

#### 3.2.1-d 段 4 vendorPostAudio（const L1148–1490）

**回填 /etc/public.libraries.txt**（官方 vendor cat，119 B /
080bf9319fb2eef0…a8710，校验含 libOpenCL.so/libOpenCL-pixel.so/libedgetpu_client.google.so/
libedgetpu_util.so/lib_aion_buffer.so/libmetrics_logger.so 六行）：
0x81a4/0/0/vendor_configs_file。伴随删
`/etc/init/cdh-pixel6-sensors-apex-bindmount.rc`。

**新增 GPU 扫描库 6 个（AIDA64 等文件扫描路径对齐，不改 EGL 选择器）**，
全部从官方 vendor.img cat + length/sha256 校验，0x81a4/0/0/
**same_process_hal_file**：

| 路径 | length | sha256 |
|---|---|---|
| /lib/egl/libGLES_mali.so | 36727472 | 4ce968fb25621657…c2ac36 |
| /lib64/egl/libGLES_mali.so | 50687312 | b0b38345bf594a37…af361e |
| /lib/libOpenCL.so | 22152 | 2fea45774bc6f459…bc6f5 |
| /lib64/libOpenCL.so | 35440 | 9350447242f79639…cc8ec |
| /lib/libOpenCL-pixel.so | 3032 | 209d666b1e99f594…3d23 |
| /lib64/libOpenCL-pixel.so | 14360 | 3aa12c5f6e23e12f…b776 |

**新建目录 /lib/egl**（base 无 32 位 egl 目录）：mkdir+sif 0x41ed/0/2000/
same_process_hal_file。**为腾 inode 删 9 个无关件**：/pci.ids、
/etc/NOTICE.xml.gz、/etc/boringssl_self_test.zygote32.rc、
boringssl_self_test.zygote64_32.rc、/lib64/android.hardware.radio@1.0.so、
@1.1.so、libreference-ril.so、libril.so、librilutils.so。

**Sensors HAL 替换**：/bin/hw/android.hardware.sensors-service.example ←
guest-helper `cdh-pixel6-sensors-service`，0x81ed/0/2000/hal_sensors_default_exec
（先删 ${filtered} 的 `(allow init hal_sensors_default_exec (file (mounton)))`）。

**Vibrator/Thermal/NFC 三套**（各自 3 件：bin 0x81ed/0/2000/hal_*_default_exec +
vintf manifest 0x81a4/0/0/vendor_configs_file + init rc 0x81a4/0/0/vendor_configs_file，
全部先 rm 同名旧件）：

| HAL | bin | manifest / rc | service 声明要点 |
|---|---|---|---|
| Vibrator | /bin/hw/cdh-pixel6-vibrator-service | /etc/vintf/manifest/cdh-pixel6-vibrator.xml + /etc/init/cdh-pixel6-vibrator.rc | vendor.cdh_pixel6_vibrator，seclabel hal_vibrator_default，IVibrator+IVibratorManager |
| Thermal | /bin/hw/cdh-pixel6-thermal-service | cdh-pixel6-thermal.xml + cdh-pixel6-thermal.rc | vendor.cdh_pixel6_thermal，seclabel hal_thermal_default，IThermal |
| NFC | /bin/hw/cdh-pixel6-nfc-service | cdh-pixel6-nfc.xml + cdh-pixel6-nfc.rc | vendor.cdh_pixel6_nfc，seclabel hal_nfc_default，INfc |

**runtime sepolicy 增 3 条**：pixelsystemservice_app→hal_power_stats_service
find、pixelsystemservice_app→hal_power_stats_default binder call、
system_server→zygote_tmpfs file write。

#### 3.2.1-e 段 5 widevineAPEX（L247–297）

**新增 /apex/com.google.android.widevine-12141839.apex**：官方 vendor.img cat →
5459968 B / 01b4ff35c88868ea…64ac6，0x81a4/0/0/**vendor_apex_file**。
已存在时先读回比对同一 sha（未知内容拒覆盖 → exit 9），一致则跳过写。
进 `vendor_policy_required_sha256` 终检名单。

#### 3.2.1-f 段 6 updateEngine（L299–343）

**新增 /etc/init/update_engine_compat.rc**：先从 **system 镜像** cat
`/etc/init/update_engine.rc` 并校验 `service update_engine /system/bin/update_engine`
+ `disabled` 存在；落位内容固定两行 `on boot` / `    enable update_engine`，
0x81a4/0/0/vendor_configs_file。作用：恢复 GSI 中被禁用的 A/B 更新引擎。

#### 3.2.1-g 段 7 GPUServiceVenusPatchScript（pixelvendor 共享库段）

对 vendor `/bin/hw/android.hardware.graphics.allocator@4-service`（gpuservice 侧
图形服务）做 Venus 身份对齐（共享于多设备的通用段，marker
`pixel6-15.vendor-gpuservice-venus-v1`）——把服务报出的渲染器标识统一为
Virtio-GPU Venus，供 SurfaceFlinger/Vulkan 层选择器命中 virgl 链路。

#### 3.2.1-h 段 8 VulkanRuntimeIdentityPatch（L345–405）

**改写 /lib64/hw/vulkan.virtio.so**（原样读出 → 等长 dd 改写 → rm+write 回）：
搜索单次出现的 21 字节模板 `Virtio-GPU Venus (%s)`，替换为
`Mali-G78` + 13 个 NUL（共 21 B，长度不变；写入后校验旧模板 0 次、新名恰 1 次）。
落位 0x81a4/0/0/**same_process_hal_file**。sepolicy 追加
`(allow shell vendor_graphics_prop (file (getattr map open read)))`。

#### 3.2.1-i 段 9 VendorAfterGPUServicePatch（vendor_policy.go L1509–2181）

基线链中体量最大的一段：接通 hvc2–hvc5 四个 virtio 安全终端、替换 Remote
KeyMint、写入 6 个自研 HAL 二进制与合并版 VINTF/init、Goodix 指纹 KO。

**(1) vendor CIL（写入 `${filtered}`，最终随 precompiled_sepolicy 落盘）**

| 动作 | 精确内容 |
|---|---|
| 删除 | `(allow system_server_202404 self (process (execmem)))`（virgl 链路下移除旧兼容规则，删后强制复检不存在） |
| 追加 | `(allow hal_bluetooth_default bt_device* (chr_file (ioctl)))`（bt_device 类型从既有规则动态唯一探测） |
| 追加 | keymaster_device 5 行：`(type keymaster_device)`/`(roletype object_r keymaster_device)`/`(typeattributeset dev_type (keymaster_device))`/`(allow hal_keymint_default keymaster_device (chr_file (getattr ioctl open read write)))`/`(allow hal_keymint_default serialno_prop (file (read getattr map open)))` |
| 追加 | adb_key_device：`(type adb_key_device)`+`(roletype object_r adb_key_device)` + 5 条 allow（dev_type 归并、adbd chr_file 五权限、`__PIXEL6_ADB_VENDOR_DIR__` dir、`__PIXEL6_ADB_KEYS_TYPE__` dir、file 六权限）+ `__PIXEL6_ADB_CAPABILITIES__` |
| 追加 | `(allow hal_keymint_default kmsg_device (chr_file (write)))` |
| 追加 | camera 2 条：`(allow pixelsystemservice_app cameraserver_service (service_manager (find)))`、`(allow hal_camera_default ashmem_device (chr_file (getattr ioctl open read write)))` |
| 追加 | thermal：`(allow hal_thermal_default proc_stat (file (getattr open read)))` |
| 追加 | Goodix KO 加载 2 条：`(allow vendor_init self (capability (sys_module)))`、`(allow vendor_init vendor_configs_file (system (module_load)))` |
| 追加 | system 侧 KO 加载 3 条：`(allow vendor_init self (capability (sys_module)))`、`(allow vendor_init system_file (system (module_load)))`、`(allow vendor_init system_file (file (read open getattr map)))` |
| 追加 | gnss_control_data_file：type/roletype/file_type/data_file_type 归并 + 5 条 allow（vendor_init dir、shell dir、shell file、hal_gnss_default dir、hal_gnss_default file） |
| 追加 | hal_light_default 完整域 14 行（type×2/roletype×2/domain/halserverdomain/hal_light/hal_light_server/vendor_file_type/file_type/exec_type + init transition/exec/typetransition + entrypoint） |
| 追加 | hal_nlinterceptor_default 完整域 18 行（同上结构 + self net_admin/net_userns + netlink_generic_socket/netlink_route_socket 全权限） |
| 追加 | hal_codec2_default 完整域 24 行（含 init siginh/rlimitinh、dontaudit noatsecure、servicemanager、su↔hal_codec2/audioserver/mediaswcodec 双向 binder + mediaswcodec fd use） |
| 追加 | hal_gnss_default 完整域 15 行 |

**(2) /etc/selinux/vendor_file_contexts（rm+write，0x81a4/0/0/vendor_configs_file）**

先 cat 读出 → awk 删除所有含 `lineage`（不区分大小写）的行和既有 hvc2–hvc5 行，再追加 6 行：

```
/dev/hvc2  u:object_r:adb_key_device:s0
/dev/hvc3  u:object_r:keymaster_device:s0
/dev/hvc4  u:object_r:radio_device:s0
/dev/hvc5  u:object_r:bt_device:s0
/vendor/bin/netmgr_setup  u:object_r:setup_wifi_exec:s0
/data/vendor/gps(/.*)?  u:object_r:gnss_control_data_file:s0
```

**(3) /etc/ueventd.rc（rm+write，0x81a4/0/0/vendor_configs_file）**

awk 删除 hvc2–hvc5 与 `/dev/video*` 既有行，追加 5 行设备权限：

```
/dev/hvc2 0600 root root
/dev/hvc3 0666 system system
/dev/hvc4 0660 radio radio
/dev/hvc5 0660 bluetooth bluetooth
/dev/video* 0660 system camera
```

**(4) /etc/selinux/vendor_property_contexts（rm+write，0x81a4/0/0/vendor_configs_file）**

删除 `ro.vendor.boot_security_patch` 既有行，追加：
`ro.vendor.boot_security_patch u:object_r:vendor_security_patch_level_prop:s0 exact string`

**(5) /etc/init/android.hardware.security.keymint-service.rc（rm+write，0x81a4/0/0/vendor_configs_file）**

awk 在唯一的 `service vendor.keymint-default /vendor/bin/hw/android.hardware.security.keymint-service`
行后插入 `    file /dev/kmsg w`（插入数与总出现数均强制校验为 1）。

**(6) 共享 HAL 注入（自研产物目录 `devices/pixel6-15/output/guest-helper/hal/pixel6/android15/`）**

删除（幂等 rm）：6 个既有 bin（lights/camera/nlinterceptor/codec2/gnss 条件删 +
`/bin/hw/cdh-pixel6-gnss-service` 条件删）、**无条件 rm**
`/bin/hw/android.hardware.security.keymint-service`（原厂 KeyMint 二进制）+
`/etc/vintf/manifest/android.hardware.security.keymint-service.xml`；stale 清理
12 个：`/etc/vintf/manifest/cdh-pixel6-{lights,vibrator,thermal,nfc}.xml`、
`/etc/init/cdh-pixel6-{lights,vibrator,thermal,nfc}.rc`（无条件）+
`/etc/vintf/manifest/cdh-pixel6-{camera-provider,hal}.xml`、
`/etc/init/cdh-pixel6-{camera-provider,hal}.rc`（条件）。

新增/替换写入（debugfs 四联写）：

| 落盘路径 | 来源 | mode/uid/gid | SELinux |
|---|---|---|---|
| /bin/hw/cdh-pixel6-lights-service | cdh-pixel6-lights-service | 0x81ed/0/2000 | hal_light_default_exec |
| /bin/hw/cdh-pixel6-camera-provider-service | cdh-pixel6-camera-provider-service | 0x81ed/0/2000 | hal_camera_default_exec |
| /bin/hw/cdh-pixel6-nlinterceptor-service | cdh-pixel6-nlinterceptor-service | 0x81ed/0/2000 | hal_nlinterceptor_default_exec |
| /bin/hw/cdh-pixel6-codec2-store-service | cdh-pixel6-codec2-store-service | 0x81ed/0/2000 | hal_codec2_default_exec |
| /bin/hw/android.hardware.gnss-service.pixel | android.hardware.gnss-service.pixel | 0x81ed/0/2000 | hal_gnss_default_exec |
| /bin/hw/android.hardware.security.keymint-service | **android.hardware.security.keymint-service.remote**（Remote KeyMint，覆盖原厂） | 0x81ed/0/2000 | **hal_keymint_default_exec** |
| /etc/vintf/manifest/android.hardware.security.keymint-service.xml | android.hardware.security.keymint-service.xml（校验含 `<fqname>IKeyMintDevice/default</fqname>` 且不含 IRemotelyProvisionedComponent） | 0x81a4/0/0 | vendor_configs_file |
| /etc/vintf/manifest/cdh-pixel6-hal.xml | 现场生成（heredoc） | 0x81a4/0/0 | vendor_configs_file |
| /etc/init/cdh-pixel6-hal.rc | 现场生成（heredoc） | 0x81a4/0/0 | vendor_configs_file |

cdh-pixel6-hal.xml 共 15 个 `<hal format="aidl">`：自建 8 个（light
ILights/default、vibrator IVibrator+IVibratorManager、thermal IThermal、nfc
INfc、camera.provider ICameraProvider/internal/0、net.nlinterceptor
IInterceptor、media.c2 IComponentStore 三实例 default/default1/default2、gnss
IGnss/default）+ Radio fragment 7 个（radio.config IRadioConfig/default、
radio.data/messaging/modem/network/sim/voice 各 slot1，均 v3）。

cdh-pixel6-hal.rc 内容（按顺序）：
- `on post-fs-data`：`mkdir /data/vendor/gps 0770 system shell`；
  `insmod /vendor/etc/cdh_pixel6_goodix_fingerprint.ko`；
  `insmod /system/etc/cdh_pixel6_cpu_probe.ko`；
  `insmod /system/etc/cdh_pixel6_device_info.ko`；zram 段
  （exec_background：reset→lz4→`echo 3221225472 > /sys/block/zram0/disksize`
  即 3GiB→mkswap→swapon，SwapTotal 目标 3145724 kB）；
- sensors fragment（`pixel6SensorsVendorInitFragment()`）；
- 11 个 service：`vendor.cdh_pixel6_lights|vibrator|thermal|nfc`（user system
  group system，各自 seclabel）、`vendor.cdh_pixel6_camera_provider`（group
  system camera）、`vendor.cdh_pixel6_nlinterceptor`（user root group system
  inet）、`vendor.cdh_pixel6_codec2_default`
  （**可执行路径在 /system/bin/hw/android.hardware.media.c2-service**）、
  `vendor.cdh_pixel6_codec2_default1/default2`
  （/vendor/bin/hw/cdh-pixel6-codec2-store-service --instance default1|default2，
  三实例均 user mediacodec group camera drmrpc mediadrm）、`gnss_service`
  （user system group system shell）、Radio fragment
  （`vendor.cdh_pixel6_radio`，user radio group radio system inet net_admin，
  capabilities NET_ADMIN，seclabel hal_radio_cdh）。

**(7) /etc/cdh_pixel6_goodix_fingerprint.ko（rm 幂等+write）**

来源 `devices/pixel6-15/prebuilts/kernel/15/lineage-22.1-pixel6-15/modules/cdh_pixel6_goodix_fingerprint.ko`，
0x81a4/0/0/**vendor_configs_file**。

#### 3.2.1-j 段 10 rroPatch（vendor_rro.go）

/overlay/ 下 4 个 RRO APK 的原字节迁移（dump 导出→rm 旧名→write 新名，登记
required_sha256 + forbidden 旧路径）：

| 旧名（删除） | 新名（写入，0x81a4/0/0/vendor_overlay_file） |
|---|---|
| SettingsProvider__lineage_virtio_arm64only__auto_generated_rro_vendor.apk | SettingsProvider__pixel6_vendor_auto_generated_rro.apk |
| Settings__lineage_virtio_arm64only__auto_generated_rro_vendor.apk | Settings__pixel6_vendor_auto_generated_rro.apk |
| framework-res__lineage_virtio_arm64only__auto_generated_rro_vendor.apk | framework-res__pixel6_vendor_auto_generated_rro.apk |
| org.lineageos.platform-res__lineage_virtio_arm64only__auto_generated_rro_vendor.apk | （纯删除，Lineage 专属 overlay 不迁移） |

APK 字节内容不变（sha256 前后一致），仅换文件名以抹掉 Lineage 构建痕迹。

#### 3.2.1-k 段 11 RadioPatch（vendor_policy.go L437–536）

**(1) /etc/selinux/vendor_property_contexts（再次 rm+write，0x81a4/0/0/vendor_configs_file）**

在段 9 输出的文件上继续：awk 删除 `vendor.radio.` 前缀行，追加
`vendor.radio. u:object_r:vendor_rild_prop:s0`。

**(2) /etc/selinux/vendor_seapp_contexts（rm+write，0x81a4/0/0/vendor_configs_file）**

删除 `name=com.samsung.slsi.telephony.oemril` 行，追加：
`user=_app isPrivApp=true name=com.samsung.slsi.telephony.oemril domain=oemrilservice_app levelFrom=all`

**(3) vendor CIL 追加两域**：hal_radio_cdh 域 20 行（type/exec type/roletype×2/
domain/halserverdomain/vendor_file_type/file_type/exec_type 归并 + init
transition/typetransition/entrypoint + servicemanager binder + hal_radio_service
add+find + hal_telephony_client 双向 binder + radio_device chr_file + self
net_admin + netlink_route_socket）+ oemrilservice_app 域 16 行（domain/appdomain/
hal_telephony_client 归并 + vendor_rild_prop 属性 type 三归并 + property_socket_202404
sock_file write + init_202404 connectto + vendor_rild_prop property_service set 与
file 读 + app_api_service/radio_service_202404 find）。

**(4) /bin/hw/android.hardware.radio-service.cdh-pixel6**（rm 幂等+write
0x81ed/0/2000/**hal_radio_cdh_exec**）

来源 `output/guest-helper/hal/pixel6/android15/android.hardware.radio-service.cdh-pixel6`
（AIDL v3，源码 android-15.0.0_r36，12 个 .cpp 自研编译）。stale 删除：
/etc/vintf/manifest/cdh-pixel6-radio.xml、/etc/init/cdh-pixel6-radio.rc。
Radio 的 VINTF 与 init 声明不单写，已并入段 9 的 cdh-pixel6-hal.xml/rc
（7 包 fragment + vendor.cdh_pixel6_radio service）。

#### 3.2.1-l 段 12 tensorPatch（tensor_patch.go）

**(1) vendor /build.prop（cat 读出 → gsifix patch-props → rm+write）**

`gsifix gsifix patch-props -in … -o … -set/-unset …` 精确改写：
- `-set` 61 键：基础 12（ro.board.platform=gs101、ro.product.board=oriole、
  ro.product.first_api_level=31、ro.product.vendor.{brand,device,manufacturer,model,name}
  =google/oriole/Google/Pixel 6/oriole、dalvik.vm.heapgrowthlimit=256m、
  heapmaxfree=32m、heapminfree=8m、heaptargetutilization=0.5）+ 机型专有 49
  （ro.build.ab_update=true、ro.vendor.api_level=31、ro.product.vendor_dlkm.*
  5 键、ro.vendor.build.ab_ota_partitions=abl,bl1,bl2,bl31,boot,dtbo,gsa,ldfw,
  modem,pbl,product,system,system_ext,tzsw,vbmeta,vbmeta_system,vbmeta_vendor,
  vendor,vendor_boot,vendor_dlkm、ro.vendor.build.{date,date.utc=1743029832,
  fingerprint=google/oriole/oriole:15/BP1A.250505.005/13277524:user/release-keys,
  id=BP1A.250505.005,security_patch=2025-05-05,svn=02,tags=release-keys,type=user,
  version.incremental=13277524,version.release=15,version.release_or_codename=15,
  version.sdk=35,version.sdk_minor=0}、ro.vendor.boot_security_patch=2025-05-05、
  ro.vendor_dlkm.build.* 10 键、dalvik.vm.dex2oat-*/isa.arm*/restore-dex2oat-*/
  systemuicompilerfilter=speed 9 键、ro.com.android.prov_mobiledata=false、
  ro.gms.dck.{eligible_wcc,se_capability}、ro.lmk.* 3 键、
  ro.control_privapp_permissions=enforce、ro.soc.manufacturer=Google、
  ro.soc.model=Tensor）；
- `-unset` 38 键（删整行）：debug.sf.vsync_reactor_ignore_present_fences、
  ro.arch、ro.setupwizard.enterprise_mode/esim_cid_ignore、
  ro.surface_flinger.enable_frame_rate_override/native_window_buffers_format、
  qemu.hw.mainkeys、ro.mediaserver.64b.enable、ro.cpuvulkan.version、
  ro.telephony.default_network、ro.storage_manager.show_opt_in、
  ro.support_hide_display_cutout/one_handed_mode、ro.thermal_warmreset、
  ro.vendor.camera.extensions.package/service、ro.wifi.channels、
  security.perf_harden、setupwizard.feature.* 12 键、setupwizard.theme、
  telephony.active_modems.max_count、bluetooth.profile.* 7 键。

落位 **0x8180(0600)/0/0/vendor_file**。写前逐键 grep 校验 61 个 expected 齐全、
38 个 absent 无残留。

**(2) odm /etc/build.prop（独立镜像会话）**

`gsifix extract -raw -partition odm` → cat /etc/build.prop → patch-props
`-set` 9 键（ro.product.odm.{brand,device,manufacturer,model,name} 5 +
ro.odm.build.{fingerprint,date,date.utc=1743029832,version.incremental=13277524}
4）→ 独立 debugfs 会话写回（0x8180/0/0/**vendor_configs_file**）→
`gsifix replace -raw -partition odm` 回写 super。

#### 3.2.1-m 段 13 VendorDLKMPatch（vendor_policy.go L2183–2241）

**逐字节恢复官方 vendor_dlkm /etc/build.prop**（本仓链已天然满足，基线链需
回填）：从官方 factory vendor_dlkm.img cat 出
`/etc/build.prop`（**887 B，sha256=9ddbcc56a7a6d2836f2c776265aa31762fd80715fee
4423d9f06dc9b51f4a604**，写前强制校验）→ 写入自产 vendor_dlkm.img：
- `mkdir /etc`（若缺失）0x41ed/0/0/vendor_configs_file，四时间戳 0x495c0780；
- rm 幂等 + write /etc/build.prop 0x81a4/0/0/vendor_configs_file，四时间戳
  0x495c0780（与官方 inode 时间一致）；
- 回读 sha256 必须等于官方值 → e2fsck → `gsifix replace -raw -partition
  vendor_dlkm`。

#### 3.2.1-n 段 14 wifiScanPolicy（wifi_scan_policy.go）

vendor CIL 追加 7 行（cdh_wifi_scan_param 类型 + genfscon 挂到 sysfs 模块参数）：

```
(type cdh_wifi_scan_param)
(roletype object_r cdh_wifi_scan_param)
(typeattributeset file_type (cdh_wifi_scan_param))
(typeattributeset sysfs_type (cdh_wifi_scan_param))
(genfscon sysfs "/module/cdh_virt_wifi/parameters/scan_profile" (u object_r cdh_wifi_scan_param ((s0) (s0))))
(allow shell cdh_wifi_scan_param (file (getattr open read write)))
(allow hal_wifi_default cdh_wifi_scan_param (file (getattr open read)))
```

#### 3.2.1-o 段 15 WifiPatch（vendor_policy.go L2243–2501）

**(1) /bin/hw/android.hardware.wifi-service（rm 幂等+write）**

来源 `output/guest-helper/hal/pixel6/android15/cdh-pixel6-wifi-service`（写前
`strings` 校验不得含 /Volumes/、/Users/、cloud-droid-hub、cdh_pixel6_wifi 宿主
痕迹）。**0x81ed/0/1010/hal_wifi_default_exec**（gid 1010 = wifi 用户）。

**(2) /etc/vintf/manifest/android.hardware.wifi-service.xml（rm 幂等+write
0x81a4/0/0/vendor_configs_file）**：IWifi v2 default 单 HAL 声明。

**(3) /etc/init/android.hardware.wifi-service.rc（rm 幂等+write
0x81a4/0/0/vendor_configs_file）**：

```
on post-fs-data
    start setup_wifi
on property:sys.boot_completed=1
    start vendor.wifi_default_on
service vendor.wifi_default_on /system/bin/sh -c \
    "/system/bin/svc wifi enable; /system/bin/sleep 2; \
     /system/bin/cmd wifi connect-network VirtWifi open"
    class late_start  user shell  group shell wifi inet
    seclabel u:r:shell:s0  disabled  oneshot
service vendor.wifi_hal_legacy /vendor/bin/hw/android.hardware.wifi-service
    class hal  capabilities NET_ADMIN NET_RAW SYS_MODULE
    user wifi  group wifi gps inet net_admin net_raw
    seclabel u:r:hal_wifi_default:s0
    interface aidl android.hardware.wifi.IWifi/default
```

stale 删除：/etc/vintf/manifest/cdh-pixel6-wifi.xml、/etc/init/cdh-pixel6-wifi.rc。

**(4) /build.prop（在段 12 输出文件上继续改）**

sed 删 3 键（ro.vendor.wifi_impl、ro.vendor.virtwifi.port、
persist.vendor.cdh.wifi.enabled）+ 追加 2 键
（**ro.vendor.wifi_impl=virt_wifi、ro.vendor.virtwifi.port=eth1**）→
rm+write 0x8180/0/0/vendor_file。

**(5) vendor_dlkm 五模块与四索引（独立镜像会话）**

来源 `prebuilts/kernel/15/lineage-22.1-pixel6-15/modules/`：
- write 5 个 ko（均 rm 幂等后写，0x81a4/0/0/**vendor_file**）：
  virtio_console.ko、rfkill.ko、cfg80211.ko、cdh_virt_wifi.ko、cdh_media.ko；
- **/lib/modules/modules.load** 4 行：virtio_console、cfg80211、cdh_virt_wifi、
  cdh_media；
- **/lib/modules/modules.dep** 5 行：virtio_console.ko: / rfkill.ko: /
  cfg80211.ko: rfkill / cdh_virt_wifi.ko: cfg80211 rfkill / cdh_media.ko:；
- **/lib/modules/modules.alias**：删旧 `rtnl-link-virt_wifi` 别名行，追加
  `alias rtnl-link-virt_wifi cdh_virt_wifi`；幂等追加
  `alias virtio:d00000030v* cdh_media`（精确计数防重复）；
- **/lib/modules/modules.options**：幂等追加
  `options cdh_media allow_userptr=1`；
- 删除 25 个无用 ko（virt_wifi.ko、kheaders.ko、14 个 kunit/test ko、
  tipc.ko、tipc_diag.ko 等）；
- 四索引 0x81a4/0/0/vendor_file；e2fsck -fy + 五 ko 与四索引逐个 dump 回读
  cmp 比对 → `gsifix replace -raw -partition vendor_dlkm`。

#### 3.2.1-p 段 16 codec2V4L2ServicePatch（codec2_v4l2_service.go）

**vendor /etc/media_codecs_c2.xml（改写段 1 已排队的 write 工作副本）**：
校验 debugfs_cmd 中该路径的 write 命令恰 1 条 → awk 在 `</MediaCodecs>` 前
幂等注入 3 个 V4L2 编解码器登记：

```
<Decoders>
    <MediaCodec name="c2.v4l2.avc.decoder" type="video/avc">
        <Feature name="adaptive-playback" />
    </MediaCodec>
    <MediaCodec name="c2.v4l2.hevc.decoder" type="video/hevc">
        <Feature name="adaptive-playback" />
    </MediaCodec>
</Decoders>
<Encoders>
    <MediaCodec name="c2.v4l2.avc.encoder" type="video/avc">
        <Limit name="size" min="32x32" max="1920x1088" />
        <Limit name="alignment" value="2x2" />
        <Limit name="block-size" value="16x16" />
        <Limit name="bitrate" range="1-40000000" />
        <Feature name="bitrate-modes" value="VBR,CBR" />
    </MediaCodec>
</Encoders>
```

注入后 grep 校验三个 codec 名齐全 → mv 回工作副本 → 计算新 sha256 登记
required_sha256（最终由统一 debugfs 会话写入 vendor 分区）。

**system 侧 3+3 件不在本段写盘**，由 vdc 的 GSI bundle 承载（见 §4）：
/system/bin/hw/android.hardware.media.c2-service、
/system/lib64/libc2plugin_store.so，以及
/system/lib64/{libcodec2_hidl@1.0.so, libcodec2_aidl.so,
libstagefright_bufferqueue_helper_novndk.so} 三个运行时依赖。default 实例由
system 侧 c2-service 承载（cdh-pixel6-hal.rc 中
vendor.cdh_pixel6_codec2_default 的可执行路径即指向它），default1/default2
元数据实例仍由 vendor 侧 cdh-pixel6-codec2-store-service 承载。

## 3.2.2 本仓链 vendor 分区重打包（build/build-vendor.py → out/dist/rom/vendor.img）

基座与基线链同源：**lineage-22.2 编译的 oriole vendor.img**
（`lineage-rom/lineage-22.2/super/vendor.img`，UTM virtio_arm64only super
拆出件，raw ext4）。全树实测证明基线盘 vendor 分区 = 同一 lineage 基座 +
§3.2.1 的 16 段补丁（fstab 三件套等逐字节一致，见 §3.2.2.6）；本仓链用
build-vendor.py 在同一基座上复刻「自研 HAL + goodix ko + Widevine APEX +
官方件回填 + 自生成件」全集（表 A–G），所有差异以 debugfs 原地注入完成。
官方 factory vendor.img 降级为**提取源**（GPU 临时库链接依赖 + Widevine
恢复 + 基线对齐官方件，表 F/G）。lineage 基座空闲块 ~128 MiB 但**空闲
inode 仅 16**（ext4 inode 只随新增 block group 增长），注入 ~113 MiB
（GPU 扫描库 ~87 MiB 为主）/ 需 59 inode（含 128 inode 余量共需 187）
必然触发扩容（§3.2.2.2）。SEPolicy/contexts 追加不落 vendor.img，由
system 侧已 patch 的策略承载（§4）；vintf 声明/合并总 rc/rro 迁移三类
刻意不复刻（剩余差异清单见 §3.2.2.6）。

### 3.2.2.0 输入与输出

| 角色 | 路径 | 来源 |
| --- | --- | --- |
| 基座（复制为工作副本） | lineage-rom/lineage-22.2/super/vendor.img | lineage-22.2 编译的 oriole vendor（virtio_arm64only super 拆出件；实测 238202880 B = 227MiB / 512 inode / 空闲 inode 仅 16） |
| HAL 产物树 | out/dist/vendor/{bin/hw, etc/init, lib64} | vendor/build 总驱动（go run vendor/build --runtime-lib-dir vendor-compile-dep）编译 vendor/ 各子项目与 guest-helper HAL |
| goodix ko | out/dist/kernel/modules/cdh_pixel6_goodix_fingerprint.ko | build-kernel.py 归集（本仓自编译内核产物） |
| Widevine 恢复源（仅提取用） | 官方 vendor.img 内 /apex/com.google.android.widevine-12141839.apex | Google 官方 factory 镜像（lineage 基座无此闭源件，从此恢复） |
| GPU 临时库源（仅提取用） | 官方 vendor.img /lib64/{egl/libGLES_mali.so, libOpenCL.so, libOpenCL-pixel.so} | Google 官方 factory 镜像（→ vendor-compile-dep/gpu-temp-libs，不进镜像） |
| 基线对齐官方件源（仅提取用） | 官方 vendor.img /etc/*（14 XML + public.libraries.txt）+ GPU 扫描库 6 件 | Google 官方 factory 镜像（表 G：校验 len+sha256 后回填进镜像） |
| 工作目录 | out/tmp/vendor-pack/（vendor.work.img + debugfs.cmd + widevine 临时件） | 脚本自建，每次重跑先清空 |
| 输出 | out/dist/rom/vendor.img | 工作副本 e2fsck+校验后 copy2 落盘（402653184 B = 384MiB / 768 inode，3 个 block group） |

### 3.2.2.1 编译依赖预收集（宿主侧 vendor-compile-dep/，不进镜像）

基座切到 lineage 后，官方 factory vendor.img 仅剩两个用途：GPU 临时库
提取 + Widevine APEX 恢复源（表 F）；vendor-compile-dep/ 全树只作链接期
依赖，不进镜像。

写入镜像前先备齐链接期依赖（`collect_deps / collect_gpu_temp_libs /
collect_codec2_deps`，e2cp 定点抽取，已存在即跳过）：

**基础七库（DEP_FILES，全部 ← 官方 system.img /system/lib64/）**：
libbinder_ndk.so、libc++.so、libnativewindow.so、libbase.so、
libcrypto.so、libkeymaster_messages.so、liblog.so
→ `vendor-compile-dep/<名>`（guest keymint 与 codec2 链接用）。

**GPU 临时库（GPU_TEMP_LIBS，← 官方 vendor.img）**：
`/lib64/egl/libGLES_mali.so` → `gpu-temp-libs/egl/libGLES_mali.so`、
`/lib64/libOpenCL.so`、`/lib64/libOpenCL-pixel.so` → `gpu-temp-libs/<名>`；
vulkan.pastel.so 为自研（vendor/vulkan-stub 编译直出 out/dist），不抽取。

**codec2 system 分组 16 库（← 官方 system.img /system/lib64/）** →
`vendor-compile-dep/system/lib64/`：
android.hardware.graphics.common@1.0.so、android.hardware.media.c2@1.0/
@1.1/@1.2.so、libavservices_minijail.so、libbase.so、libbinder_ndk.so、
libchrome.so、libcutils.so、libhidlbase.so、liblog.so、
libsfplugin_ccodec_utils.so、libstagefright_bufferqueue_helper.so、
libstagefright_foundation.so、libui.so、libutils.so。

**codec2 APEX 分组 14 库（← system.img 内
/system/apex/com.google.android.media.swcodec.apex）** →
`vendor-compile-dep/apex/media-swcodec/lib64/`：双层 zip 容器解包链
（e2cp 抽 .apex → zip 解 original_apex → zip 解 apex_payload.img →
e2cp 自 payload:lib64/ 抽库，缓存在 out/tmp/codec2-apex/）：
android.hardware.common-V2-ndk.so、android.hardware.common.fmq-V1-ndk.so、
android.hardware.graphics.common-V5-ndk.so、
android.hardware.media.bufferpool2-V2-ndk.so、
android.hardware.media.c2-V1-ndk.so、libc++.so、libcodec2_aidl.so、
libcodec2_hidl@1.0/@1.1/@1.2.so、libcodec2_soft_common.so、
libcodec2_vndk.so、libfmq.so、libstagefright_bufferqueue_helper_novndk.so。

工具链依赖：e2tools（e2cp）、e2fsprogs（debugfs/e2fsck/resize2fs，
env `E2FSPROGS_<TOOL>` > `/opt/homebrew/opt/e2fsprogs/sbin` > PATH）。

### 3.2.2.2 容量扩展（块 + inode 双指标，group 边界取整 + resize2fs）

lineage 基座空闲块 ~128 MiB，基线对齐注入（GPU 扫描库 ~87 MiB 为主）后
总需求 ~113 MiB 已逼近空闲上限；且 ext4 的 inode 只随新增 block group
增长（每组固定 256 inode），基座**空闲 inode 仅 16**——块或 inode 任一
不足即触发扩容（`_ensure_vendor_capacity`）：

- `needed_bytes` = goodix ko + Widevine APEX + out/dist/vendor 树全部文件
  + GPU 扫描库 6 件 + 官方 XML 14 件 + 自生成件长度（对原位覆盖件保守
  按新增计入）+ 2 MiB margin（`byte_short = needed − free_blocks × 4096`，
  向上取整块数）；
- `inode_need` = bin/hw + etc/init + lib64 条目数 + 2（goodix ko 与
  Widevine APEX）+ 27（官方 XML 14 + GPU 扫描库 6 + 自生成件 6 +
  /lib/egl 目录 1）+ **128 余量**（对齐基线链
  VendorExt4MinFreeInodes=128）；
- inode 不足时按组取整：`groups_target = ceil((已用 inode + inode_need) /
  256)`，扩到 `groups_target × 32768 块`（与块需求取 max）；
- `new_size = (blocks + extra_blocks) × 4096`，truncate 后 `resize2fs`
  （"Nothing to do" 视为可接受）。

实测三方对照（当前注入量下扩容仍由 inode 驱动：16 空闲 → 需 187 →
取整 3 组 768 inode）：

| | lineage 基座 | 本仓产物 | 基线盘 vendor |
| --- | --- | --- | --- |
| 分区大小 | 238202880 B（227 MiB） | 402653184 B（384 MiB） | 402653184 B（384 MiB） |
| 块数 / 空闲块 | 58155 / 32864 | 98304 / 45512 | 98304 / 47008 |
| inode / 空闲 inode | 512 / 16 | 768 / 224 | 768 / 254 |

产物 = 3 × 32768 块 = **3 个 block group（768 inode）**，与基线盘 vendor
分区同形态、同大小；产物已用块比基线多 ~1.5k（lineage 专有件保留 +
codec2 V4L2 / keymint remote 注入产物），空闲块 45512 vs 47008。super 的
vendor LP 无硬编码大小，pack-roms.py 按本产物实际大小组装（§3.0/§3.1）。

### 3.2.2.3 逐文件注入清单（debugfs 单命令文件会话，命令序即执行序）

统一模式：已存在先 `rm`（幂等）→ `write <宿主源> <镜像内路径>` →
`sif mode/uid/gid` → `ea_set security.selinux`。

**表 A radio HAL 替换（replace_radio_hal，最先执行）**

| 镜像内路径 | 操作 | mode/uid/gid | SELinux | 说明 |
| --- | --- | --- | --- | --- |
| /bin/hw/vendor.google.radioext@1.0-service | 删除（存在时） | - | - | 官方 radioext，与自研 HAL 抢 radio 实例 |
| /bin/hw/rild_exynos | 删除（存在时） | - | - | 官方调制解调器守护进程 |
| /etc/vintf/manifest/cdh-pixel6-radio.xml | 删除（幂等重跑） | - | - | 旧 VINTF 声明 |
| /etc/init/cdh-pixel6-radio.rc | 删除（幂等重跑） | - | - | 旧 init 声明 |
| /bin/hw/android.hardware.radio-service.cdh-pixel6 | rm+write ← out/dist/vendor/bin/hw/ | 0x81ed=0755 / 0 / **2000（radio 组）** | u:object_r:hal_radio_cdh_exec:s0 | 自研 radio service（对齐基线链段 11 的 gid 2000） |

注：lineage 基座不含 Google 闭源 radio 组件（radioext/rild_exynos 两行
为防御性删除，防基座误换回 factory 版本时抢实例，常态 no-op）。lineage
基座自带的 reference RIL 支持库（lib64/android.hardware.radio@1.0/@1.1.so、
libreference-ril/libril/librilutils.so）保留不删（基线链 RadioPatch 删除
这 5 库，本仓链差异见 §3.2.2.6）。

**表 B /bin/hw/* HAL 二进制（os.listdir 枚举注入，radio 除外；
VENDOR_BIN_PLACEMENT 落位重定向 + VENDOR_EXEC_LABEL 映射标签，未映射者兜底
u:object_r:vendor_file:s0）**。基线盘实测统一 **gid 2000**（wifi 例外
gid 1010=AID_WIFI）；落位重定向三件（§3.2.2.6-a 路径/命名差异行）：
adb_auth_loader → `/bin/adb_auth_loader`、cdh-pixel6-adbd-netns-launcher →
`/bin/adbd_netns_launcher`（两件 netmgr 成员 mode 0750/0/2000）、
cdh-pixel6-sensors-service →
`/bin/hw/android.hardware.sensors-service.example`；其余默认
`/bin/hw/<产物名>` + mode 0x81ed=0755 / 0 / 2000。脚本硬编码的 16 条 exec 标签映射：

| 产物文件名（out/dist/vendor/bin/hw/） | SELinux exec 标签 |
| --- | --- |
| adb_auth_loader | u:object_r:adbd_exec:s0（落位 /bin/，mode 0750） |
| android.hardware.gnss-service.pixel | u:object_r:hal_gnss_default_exec:s0 |
| android.hardware.media.c2-service-v4l2-64 | u:object_r:hal_codec2_default_exec:s0 |
| android.hardware.media.c2@1.2-service-v4l2-64 | u:object_r:hal_codec2_default_exec:s0 |
| android.hardware.radio-service.cdh-pixel6 | u:object_r:hal_radio_cdh_exec:s0（表 A 承载，gid 2000） |
| android.hardware.security.keymint-service.remote | u:object_r:vendor_file:s0（兜底） |
| android.hardware.wifi-service | u:object_r:hal_wifi_default_exec:s0（gid 1010） |
| cdh-pixel6-adbd-netns-launcher | u:object_r:adbd_exec:s0（落位 /bin/adbd_netns_launcher，mode 0750） |
| cdh-pixel6-camera-provider-service | u:object_r:hal_camera_default_exec:s0 |
| cdh-pixel6-codec2-store-service | u:object_r:hal_codec2_default_exec:s0 |
| cdh-pixel6-lights-service | u:object_r:hal_light_default_exec:s0 |
| cdh-pixel6-nfc-service | u:object_r:hal_nfc_default_exec:s0 |
| cdh-pixel6-nlinterceptor-service | u:object_r:vendor_file:s0（兜底） |
| cdh-pixel6-sensors-service | u:object_r:hal_sensors_default_exec:s0（落位名 android.hardware.sensors-service.example） |
| cdh-pixel6-thermal-service | u:object_r:hal_thermal_default_exec:s0 |
| cdh-pixel6-vibrator-service | u:object_r:hal_vibrator_default_exec:s0 |

注：keymint.remote / nlinterceptor 在基线链有专用域（hal_keymint_default_exec
等），本仓链因 vendor 镜像不带对应 SEPolicy 定义而以 vendor_file 兜底
（策略差异见 §3.2.2.6）；netns-launcher 例外——基线链以 adbd_exec 落
/bin/（netmgr 四件套成员），标签已对齐。

**表 C /etc/init/*.rc（枚举注入，均 0x81a4=0644 / 0 / 0 /
u:object_r:vendor_configs_file:s0）**：与表 B 二进制配套的全部 service rc
（本仓为分 rc：cdh-pixel6-{camera,codec2,gnss,lights,nfc,nlinterceptor,
radio,sensors,thermal,vibrator}.rc + wifi/keymint/codec2-v4l2 声明，seclabel
与表 B 标签一一对应；基线为合并总 rc cdh-pixel6-hal.rc，差异见
§3.2.2.6-a）。**vendor.adbd.rc 跳过不写**——adbd service 与触发段已并入
基线 netmgr.rc（表 G 自生成件），重复定义会导致 init 拒载第二个同名
service。

**表 D /lib64/*（枚举注入，均 0x81a4=0644 / 0 / 0 /
u:object_r:vendor_file:s0）**：HAL 运行时库（自研 HAL 链接产物；
官方 vendor 自带的 /lib64 原件不动，同名仅在被覆盖时 rm+write）。

**表 E goodix 指纹 ko**

| 镜像内路径 | 宿主源 | mode/uid/gid | SELinux |
| --- | --- | --- | --- |
| /etc/cdh_pixel6_goodix_fingerprint.ko | out/dist/kernel/modules/cdh_pixel6_goodix_fingerprint.ko | 0x81a4=0644 / 0 / 0 | u:object_r:vendor_configs_file:s0 |

（对齐基线链段 9 的 Goodix KO 落位：/etc/ 下、vendor_configs_file、
post-fs-data 阶段由 rc insmod。）

**表 F Widevine APEX 三态幂等（inject_widevine_apex）**

镜像内路径 `/apex/com.google.android.widevine-12141839.apex`，
期望 length=**5459968**、sha256=**01b4ff35c88868ea332a5ebc0de36b4aadc17cc512b3369b7788316e72264ac6**：

1. 目标已存在且 len+sha 一致 → **跳过写入**，仅补
   `ea_set … u:object_r:vendor_apex_file:s0`（保持幂等）；
2. 目标缺失 → 从官方 vendor.img `cat` 提取，先校验官方源 len+sha
   （不一致即 sys.exit），再 rm（若残留）+ write + 0x81a4/0/0 +
   vendor_apex_file；
3. 目标被篡改（摘要不符）→ 同 2 从官方源恢复覆盖。

（对齐基线链段 5 widevineAPEX 的校验式注入；lineage 基座无此 Google 闭源
APEX，常态走分支 2/3 从官方 vendor.img 恢复——恢复源先校验 len+sha256，
不符即退出。）

**表 G 基线对齐注入（inject_baseline_alignment，表 F 之后同一 debugfs
会话落盘；官方件 = 自官方 vendor.img cat 提取 + 硬编码 len+sha256 校验，
自生成件 = 生成后按基线盘实测 len+sha256 自校验；统一 rm 幂等 +
write + sif mode/uid/gid + ea_set 四联写）**

| 组（基线链段） | 件与落位参数 |
| --- | --- |
| 官方 XML 14 件（段 1/16） | /etc/permissions camera 权限 4 件（camera.full 1045 / camera.raw 912 / hardware_keystore_V3 765 / verified_boot 753）+ media_codecs 6 件（3258/12975/2584→段 16 产物/1441/697/17238）+ media_profiles_V1_0 37643 + audio_policy_configuration 17682 + audio_policy_volumes 13748；均 0644/0/0/vendor_configs_file；权限 4 件与音量曲线带 4×sif 时间戳 0x495c0780 + /etc/permissions 目录 4×sif；音量曲线双态校验（基座现状 6732440a… 或官方目标 63d7b49f… 均接受，未知即退出） |
| GPU 扫描库 6 件 + /lib/egl 目录（段 4） | /lib/egl/libGLES_mali.so 36727472、/lib64/egl/libGLES_mali.so 50687312、/lib{,64}/libOpenCL.so 22152+35440、/lib{,64}/libOpenCL-pixel.so 3032+14360；均 0644/0/0/same_process_hal_file；先 mkdir /lib/egl（0755/0/2000/same_process_hal_file）再写库（顺序不可倒置） |
| public.libraries.txt（段 4） | 官方 119 B 件回填（0644/0/0/vendor_configs_file）；六条 native public library 行核对（libOpenCL.so / libOpenCL-pixel.so / libedgetpu_client.google.so / libedgetpu_util.so / lib_aion_buffer.so / libmetrics_logger.so） |
| netmgr 自生成两件（段 4） | /bin/netmgr_setup（0700/0/0/setup_wifi_exec，1371 B，五模式 shell：cellular/setup/finalize/configure/keep）+ /etc/init/netmgr.rc（0600/0/0/vendor_configs_file，1607 B，五 netmgr service + adbd service + 触发段）；adb_auth_loader/adbd_netns_launcher 两件由表 B 落位 |
| init 链三件（段 1/5/6） | /etc/init/hw/init.oriole.rc（2213 B，自基座 init.virtio.rc 删 seriallogging service 块 185 B + start 行 24 B 派生）+ /etc/init/hw/init.virt.rc（2243 B，删 ro.cpuvulkan.version setprop 行 41 B）+ /etc/init/update_engine_compat.rc（33 B，`on boot\n    enable update_engine`）；均 0644/0/0/vendor_configs_file |
| media_codecs_c2.xml V4L2 注入（段 16） | 官方 2584 B 件在 `</MediaCodecs>` 前注入 707 B 登记（c2.v4l2.avc/hevc.decoder + adaptive-playback、c2.v4l2.avc.encoder size 32x32~1920x1088 / bitrate 1-40000000 / VBR,CBR）→ 3291 B；0644/0/0/vendor_configs_file |
| 陈旧件清理（段 1/4） | BASELINE_STALE_PATHS 10 项 rm（hardware_keystore 旧版 / ethernet / sensor×3 权限 XML、media_profiles_vendor.xml、cdh-pixel6-sensors-apex-bindmount.rc、/bin/hw 旧落位三件），存在才删（幂等） |

**fstab：lineage 基座三件套保留原样（不删、不覆盖）**

lineage 基座自带 virtio fstab 三件套（/etc/fstab.virtio / fstab.virtio.gsi.sda /
fstab.virtio.gsi.vdc），实测与基线盘 vendor 分区**逐字节一致**（gsi.vdc 即
GSI-from-vdc 变体：/system 挂 /dev/block/vdc、/data 带 fileencryption），
对齐基线链 3 盘模型；基线链的 rewriteVirtioFstabForSingleDisk 仅用于 pack
单盘模式（cdh_system 分区），3 盘模式不动该文件。官方 UFS 变体
（fstab.gs101/oriole）只存在于 factory 基座，lineage 基座无需处理。
`FSTAB_BASE_PATHS` 常量仅作回读校验锚点（§3.2.2.4 第 6 项）。

### 3.2.2.4 e2fsck 与回读校验（9 项，任一失败即退出）

1. `e2fsck -fy` 工作副本（rc 0/1 均视为通过）；
2. out/dist/vendor/bin/hw 每个产物在镜像内存在（按 VENDOR_BIN_PLACEMENT
   落位路径核对）；
3. out/dist/vendor/etc/init 每个 rc 在镜像内存在（vendor.adbd.rc 除外——
   adbd service 已并入基线 netmgr.rc）；
4. radio：radioext/rild_exynos 不存在（lineage 基座本无此二件，防御性
   删除后的校验面常态直接通过）+ 新 radio bin 存在；
5. Widevine APEX 回读 cat，len+sha256 与硬编码一致；
6. fstab：FSTAB_BASE_PATHS 三件套（fstab.virtio / fstab.virtio.gsi.sda /
   fstab.virtio.gsi.vdc）均存在——lineage 基座自带、保留原样（无需删除
   gs101/oriole 变体，lineage 基座本就没有）；
7. 基线对齐官方件：XML 13 件（media_codecs_c2 除外）+ GPU 扫描库 6 件，
   回读 debugfs stat 长度与硬编码一致（内容已在提取时 sha256 校验）；
8. 基线对齐自生成件 7 件：public.libraries.txt 119 / netmgr_setup 1371 /
   netmgr.rc 1607 / init.oriole.rc 2213 / init.virt.rc 2243 /
   update_engine_compat.rc 33 / media_codecs_c2.xml 3291，回读长度与
   基线实测一致（内容已在生成时 sha256 自校验）；
9. /lib/egl 目录存在；BASELINE_STALE_PATHS 10 项与 /etc/init/vendor.adbd.rc
   均不存在。

全部通过后 copy2 工作副本 → `out/dist/rom/vendor.img`。

### 3.2.2.5 基线链 16 段在本仓链 vendor.img 的去向（lineage 基座视角）

| 基线链段 | 本仓链去向 |
| --- | --- |
| 1 preAudioPatch（官方 XML 回填 + init.oriole.rc + 删 9 件腾 inode） | 已复刻：官方 XML 14 件回填 + init.oriole.rc 派生 + 陈旧件删除（表 G）；删件腾 inode 部分不需要——基座即 lineage 自身，扩容后空闲 inode 充足，lineage 专有件全部保留（差异见 §3.2.2.6-b） |
| 2 audioAPEXGuard | lineage 基座自带 audio APEX（与基线同件），无需注入 |
| 3 widevinePolicy / 7 GPUServiceVenusPatchScript / 14 wifiScanPolicy | SEPolicy/脚本类不落本仓 vendor.img，由 system 侧策略与 vdc bundle 承载（§4） |
| 4 vendorPostAudio（GPU 扫描库/media_codecs/public.libraries.txt/netmgr） | 已复刻（表 G：GPU 扫描库 6 件 + /lib/egl 目录 + media_codecs 6 xml + public.libraries.txt + netmgr 自生成两件；adb_auth_loader/adbd_netns_launcher 由表 B 落位） |
| 5 widevineAPEX | 表 F（lineage 基座缺失，从官方 vendor.img 恢复） |
| 6 updateEngine | rc 已复刻（表 G：/etc/init/update_engine_compat.rc 33 B）；其余 SEPolicy 部分由 system 侧承载（§4） |
| 8 VulkanRuntimeIdentityPatch | init.virt.rc setprop 删除已复刻（表 G：2243 B）；SEPolicy 部分由 system 侧承载（§4） |
| 9 VendorAfterGPUServicePatch（CIL/contexts/ueventd/共享 HAL/manifest/rc/Goodix KO/netmgr） | HAL bin+rc=表 B/C/D；Goodix KO=表 E；netmgr 四件套=表 B 落位两件 + 表 G 自生成两件；CIL/file_contexts/property_contexts/ueventd/keymint kmsg 与 manifest 声明不落本仓 vendor.img（system 侧策略承载，§4；差异见 §3.2.2.6-a/c） |
| 10 rroPatch（lineage→pixel6 overlay 迁移改名） | 不适用——基座即 lineage，auto_generated_rro_vendor 4 件原样保留，无需迁移改名 |
| 11 RadioPatch | 表 A（bin gid 2000/hal_radio_cdh_exec 对齐；CIL/seapp/property_contexts 由 system 侧承载）；reference RIL 5 库保留差异见 §3.2.2.6-b |
| 12 tensorPatch | 不适用 |
| 13 vendor_dlkm 回填 | 由 build-vendor_dlkm.py（§3.3）处理 |
| 15 WifiPatch | bin=表 B（hal_wifi_default_exec，gid 1010）；rc=表 C；build.prop 不改（lineage 原样）；vintf 声明差异见 §3.2.2.6-a |
| 16 codec2V4L2ServicePatch | vendor 侧 media_codecs 6 xml 已回填（表 G；media_codecs_c2 带注入段 3291 B）；system 侧 3+3 件由 vdc 承载（见 §4） |

### 3.2.2.6 与基线 vendor 分区的剩余差异清单（基线对齐注入后实测）

对比口径：debugfs 全树递归，ours = `out/dist/rom/vendor.img`（534 条目）vs
baseline = 基线盘 vendor 分区（504 条目）；基线 = lineage 基座 + 16 段补丁
这一构成已获证实，本仓链已按 §3.2.1 复刻官方件回填与自生成件（表 G），
产物分区大小与基线一致（402653184 B）。

**a. 仅基线有（6 项）——刻意不复刻**

| 类别 | 件与实测大小（B） |
| --- | --- |
| vintf 声明 2 件 | /etc/vintf/manifest/cdh-pixel6-hal.xml 3201、android.hardware.wifi-service.xml 271（基线链由 vendor 承载，本仓链由 system 侧 vdc 承载） |
| 合并总 rc | /etc/init/cdh-pixel6-hal.rc 4392（基线把分 rc 合并为总 rc，本仓为分 rc） |
| rroPatch 产物 3 件 | /overlay/{Settings,SettingsProvider,framework-res}__pixel6_vendor_auto_generated_rro.apk 8542×2+12638（基座即 lineage，本仓保留 lineage RRO 4 件原样，无需迁移改名） |

**b. 仅 ours 有（36 项）= 注入产物 16 件 + lineage 基座自带 20 件**

注入产物 16 件：codec2 V4L2 service 2 bin（824896×2）+ 其 rc、keymint
remote 变体 bin（1619568）+ 其 rc、分 rc 10 件（cdh-pixel6-{camera,codec2,
gnss,lights,nfc,nlinterceptor,radio,sensors,thermal,vibrator}.rc）、
/lib64/libc2plugin_store.so（codec2 store 运行库）。
lineage 基座自带 20 件（基线链段 1 删除对象及 lineage 专有件）：
cuttlefish_sensor_injection 52200、gpio 五件套 gpiodetect/gpiofind/gpioget/
gpioinfo/gpioset（各 13）、memeater 13、/etc/NOTICE.xml.gz 178599、
boringssl_self_test.zygote32.rc 34 与 zygote64_32.rc 194、reference RIL
5 库（radio@1.0 1379944 + radio@1.1 1003952 + libreference-ril 168656 +
libril 317496 + librilutils 35296）、lineage RRO overlay 4 件（8542×3 +
12638）、pci.ids 1458186。基线链段 1 删除的 permissions 5 xml（ethernet/
sensor×3/hardware_keystore）与 media_profiles_vendor.xml 已随
BASELINE_STALE_PATHS 复刻删除。

**c. 两边都有但内容不同（18 项）**

- 重编产物微差（B~KB 量级）：radio-service.cdh-pixel6 5224696/5225528、
  wifi-service 2585400/2647760、cdh-pixel6-camera-provider 1453936/1450080、
  codec2-store 1699616/1699744、vibrator 356008/356072、
  sensors-example 709552/709608 六 bin 与 goodix ko 13504/13728——本仓
  out/dist 产物 vs 基线链产物的构建差异；
- keymint 特例：ours 保留 lineage 自带的
  android.hardware.security.keymint-service（52504B 小件）并另行注入
  remote 变体（1619568B，表 B）；baseline 为官方回填件（1641832B）；
- SELinux 5 件：vendor_file_contexts 28797/27845、vendor_property_contexts
  4535/4670、vendor_seapp_contexts 0/103、vendor_sepolicy.cil 203088/213758、
  vendor_service_contexts 2188/2277——基线链在 vendor 侧注入自研 HAL 策略
  （hal_cdh_* 域），本仓链由 system 侧 sepolicy 承载（§4）；
- lineage 编译值 vs 基线链官方+补丁值：/build.prop 5251/6148（基线链
  WifiPatch 段重写，本仓按 §3.2.2.5 保留 lineage 原样）、ueventd.rc
  317/464、keymint rc 124/145、wifi rc 283/655、keymint vintf xml 524/304。

**结论**：fstab 三件套、/etc/build.prop、public.libraries.txt 与基线
**逐字节一致**；官方件回填面（GPU 扫描库 6 件、media_codecs 6 xml、
camera 权限 4 xml、netmgr 四件套、init 链三件、路径规范化三件）已全部
对齐基线；剩余差异全部属于「vintf/合并 rc/rro 三类刻意不复刻 + SELinux
策略 + 重编微差」。

## 3.3 vendor_dlkm 分区重打包（build/build-vendor_dlkm.py → out/dist/rom/vendor_dlkm.img）

与基线链段 13+15(vendor_dlkm 部分) 逐条对齐：官方基座、不扩容、
先删后写。产物分区**大小不变**（与 vendor 的扩容策略不同）。

### 3.3.0 输入与输出

| 角色 | 路径 |
| --- | --- |
| 基座（copy2 工作副本，不扩容） | roms/image-oriole-bp1a.250505.005/vendor_dlkm.img（官方 raw ext4） |
| 模块源 | out/dist/kernel/modules/{virtio_console,rfkill,cfg80211,cdh_virt_wifi,cdh_media}.ko（build-kernel.py 归集） |
| 工作目录 | out/tmp/vendor-dlkm/（vendor_dlkm.work.img + 宿主侧索引临时件 + debugfs.cmd） |
| 输出 | out/dist/rom/vendor_dlkm.img（--out/--base/--modules 可覆盖） |

镜像内模块目录 `/lib/modules`（官方无 kver 子目录）；
modules.dep 前缀 `/vendor_dlkm/lib/modules`（对齐官方实测与基线链）；
全分区 SELinux 统一 `u:object_r:vendor_file:s0`。

### 3.3.1 模块校验（写入前）

逐个 `strings` 抽 `.modinfo` 的 `name=`/`vermagic=`，打印并核对
modinfo name == 文件名去 .ko（不一致仅警告，索引按 name 生成）。

### 3.3.2 索引构造（宿主侧，写入前只读收集）

| 索引 | 构造策略 | 内容 |
| --- | --- | --- |
| /lib/modules/modules.load | **全量重建**（固定加载顺序） | virtio_console、cfg80211、cdh_virt_wifi、cdh_media（4 行） |
| /lib/modules/modules.dep | **全量重建** | `/vendor_dlkm/lib/modules/virtio_console.ko:`、`…/rfkill.ko:`、`…/cfg80211.ko: …/rfkill.ko`、`…/cdh_virt_wifi.ko: …/cfg80211.ko …/rfkill.ko`、`…/cdh_media.ko:`（5 行） |
| /lib/modules/modules.alias | **增量**：保留官方既有行，剔除旧行 `alias rtnl-link-virt_wifi …`，幂等追加 | `alias rtnl-link-virt_wifi cdh_virt_wifi`、`alias virtio:d00000030v* cdh_media` |
| /lib/modules/modules.options | **增量**：官方无此文件，保留既有（若有），幂等追加 | `options cdh_media allow_userptr=1` |

### 3.3.3 删除清单（RECLAIM_MODULES 25 个，单会话最先行）

官方 vendor_dlkm 内的测用/kunit/tipc 模块，先 rm 释放 inode（否则
新 ko + modules.options 无 inode 可分配）：
virt_wifi.ko、kheaders.ko、clk-gate_test.ko、clk-test.ko、
clk_kunit_helpers.ko、dev_addr_lists_test.ko、ext4-inode-test.ko、
fat_test.ko、hid-uclogic-test.ko、iio-test-format.ko、input_test.ko、
kunit-example-test.ko、kunit-test.ko、kunit.ko、lib_test.ko、
of_kunit_helpers.ko、platform-test.ko、regmap-kunit.ko、regmap-ram.ko、
regmap-raw-ram.ko、soc-topology-test.ko、soc-utils-test.ko、
time_test.ko、tipc.ko、tipc_diag.ko。
（与基线链段 15 的 25 回收清单逐项一致。）

### 3.3.4 写入清单（5 ko + 4 索引，均 rm 幂等 + write +
0x81a4=0644 / 0 / 0 / u:object_r:vendor_file:s0）

| 镜像内路径 | 宿主源 |
| --- | --- |
| /lib/modules/virtio_console.ko | out/dist/kernel/modules/virtio_console.ko |
| /lib/modules/rfkill.ko | out/dist/kernel/modules/rfkill.ko |
| /lib/modules/cfg80211.ko | out/dist/kernel/modules/cfg80211.ko |
| /lib/modules/cdh_virt_wifi.ko | out/dist/kernel/modules/cdh_virt_wifi.ko |
| /lib/modules/cdh_media.ko | out/dist/kernel/modules/cdh_media.ko |
| /lib/modules/modules.load | 宿主临时件（§3.3.2） |
| /lib/modules/modules.dep | 宿主临时件（§3.3.2） |
| /lib/modules/modules.alias | 宿主临时件（§3.3.2） |
| /lib/modules/modules.options | 宿主临时件（§3.3.2） |

### 3.3.5 e2fsck 与回读校验

1. `e2fsck -fy`（rc 0/1 通过）；
2. 5 个 ko：`debugfs dump` 导出后与宿主源**逐字节 cmp**（读回内容
   不等即失败）；
3. modules.dep：5 个模块条目（冒号前路径）集合包含于实际内容；
4. modules.alias 含 `alias virtio:d00000030v* cdh_media`；
5. modules.options 含 `options cdh_media allow_userptr=1`。

全部通过后 copy2 → `out/dist/rom/vendor_dlkm.img`（大小 = 官方基座，
不扩容）。

## 4. vdc：GSI system 补丁集逐文件清单（gsiPatchBundle 声明 → gsiprofile 引擎落盘）

### 4.0 vdc 盘构造与本节事实来源

| 项 | 值 |
|---|---|
| vdc 盘 | `system.qcow2` = factory 官方 `system.img`（`image-oriole-bp1a.250505.005`）整盘承载 + 下述补丁 |
| 挂载方式 | grub `androidboot.fstab_suffix=virtio.gsi.vdc` → vda vendor LP 内 `/etc/fstab.virtio.gsi.vdc` → `/system` 挂 `/dev/block/vdc`（见 §3.2.2.3 表 G） |
| 补丁声明 | `cloud-droid-hub/devices/pixel6-15/device/gsi_property_bundle.go` `gsiPatchBundle()`（699 行） |
| 属性定义 | 同目录 `gsi_properties.go`（464 行，§4.2）、`gsi_generated_files.go`（715 行，§4.6）、`codec2_v4l2_service.go`（§4.5 运行时库） |
| 执行引擎 | `pkg/devicekit/gsiprofile`：读 bundle 声明，逐项在 vdc 上落盘（文件替换/生成/文本替换/属性写删） |
| 参照基准 | `ReferenceImagePath = "build/gsi-base/base15/system.img"`（所有 FileReplacement 的 SourcePath 相对此镜像解析） |
| 幂等 marker | `MarkerSuffix = "pixel6-15-v235"`（补丁产物侧 marker，重放检测用） |
| 特殊开关 | `PixelBootstrapLibc = false`（本机型不走 PixelBootstrap libc 替换路径） |
| 唯一外部源镜像 | `pixel6Android15SourceSystemImagePath = "build/gsi-inputs/pixel6/15/oriole-bp1a.250505.005/system.img"`（仅 app_process64 一条用，见 §4.3 表 R20） |

### 4.1 bundle 顶层字段总览（gsiPatchBundle() 14 个字段）

| 字段 | 值 / 来源函数 | 数量 |
|---|---|---|
| System | `system15Props`（§4.2.1 组合链） | set 62 键（编译期净结果） |
| SystemExt | `pixel6Android15SystemExtPropertyPatches` | set 2 键 |
| Default | `default15Props`（同 System 链、不含 VerifiedPartition/rescueOff） | set 53 键 |
| SystemPropertyDeletes | `pixel6Android15SystemDefaultPropertyDeletes()` | 31 键 |
| SystemExtPropertyDeletes | `pixel6Android15VulkanSelectorPropertyDeletes()` | 1 键 |
| ProductPropertyDeletes | `pixel6Android15ProductPropertyDeletes()` | 33 键 |
| DefaultPropertyDeletes | `pixel6Android15SystemDefaultPropertyDeletes()`（同 System） | 31 键 |
| PixelBootstrapLibc | false | — |
| MarkerSuffix | `"pixel6-15-v235"` | — |
| ReferenceImagePath | `"build/gsi-base/base15/system.img"` | — |
| FileReplacements | 表 §4.3（media/drm/stagefright/codeclist/guisim/screencap/app_process64/bootanimation/codec2 系列） | 20 条 |
| GeneratedFiles | `generatedFiles()`（§4.6，全部 TimestampUnix=1743029832） | 19 条 |
| LocalFiles | 5 固定 + `codec2V4L2RuntimeLibraries()` 3 运行时库（§4.5） | 8 条 |
| FileRemovals | `/system/product/etc/security/adb_keys`、`/system/etc/init/mmd.rc` | 2 条 |
| TextReplacements | odsign / watchdog ctx / USB×2 / sepolicy / NFC×2 / property_contexts（§4.4） | 8 条 |

### 4.2 属性补丁（声明在 gsi_properties.go；引擎写入 GSI 各 prop 段后删旧键）

#### 4.2.1 System 集组合流（system15Props，函数逐层）

```
system15Props()                                     gsi_properties.go L108
├─ android15Props(pixel6SystemPropertyPatches)      L125
│  ├─ OrioleHardware(source)                        L157
│  │  ├─ ADBPropertyPatches(source)                L170
│  │  │  ├─ withoutProperty(source,"persist.sys.usb.config")   L31  先剥离（原值 adb 不进集）
│  │  │  ├─ 强制覆盖：ro.secure=1、ro.adb.secure=1、ro.debuggable=1
│  │  │  └─ 依序追加 13 组（下表 A1–A13）
│  │  ├─ + ro.hardware=oriole / ro.soc.manufacturer=Google / ro.soc.model=Tensor
│  │  └─ + persist.sys.usb.config=mtp（USBConfigPropertyPatches；注释：init.usb.rc 已改 none，
│  │        此 canonical 值对齐 stock Pixel6 但不启动 USB adbd）
│  ├─ + ro.build.version.security_patch=2025-05-05、ro.kernel.version=6.1
│  ├─ − SystemPatchDeleteKeys()（编译期从 map+order 双删 11 键，防自相矛盾）
│  └─ + VirglGraphics：ro.hardware.egl=mesa、ro.opengles.version=196610
├─ + VerifiedPartitionPropertyPatches() 8 键        L277
└─ + rescueOffProps()：persist.sys.disable_rescue=true  L191
```

**编译期覆盖链**（后写胜出，落盘前已完成）：

| 键 | pixel6SystemPropertyPatches 初值 | 中间覆盖 | 最终值 |
|---|---|---|---|
| ro.secure | 0 | ADBPropertyPatches→1 | **1** |
| ro.adb.secure | 0 | ADBPropertyPatches→1 | **1** |
| persist.sys.usb.config | adb | withoutProperty 剥离→OrioleHardware 重设 mtp | **mtp** |
| ro.hardware.egl | angle（VirtioGraphics） | VirglGraphics→mesa | **mesa** |
| ro.opengles.version | 196609（VirtioGraphics） | DisplaySurfaceFlinger→196610 | **196610** |
| ro.hardware.vulkan | pastel（VirtioGraphics） | SystemPatchDeleteKeys 删 | **不存在** |
| ro.cpuvulkan.version | 4206592（VirtioGraphics） | 同上删 | **不存在** |
| ro.surface_flinger.native_window_buffers_format | 5（VirtioGraphics） | 同上删 | **不存在** |
| ro.mediaserver.64b.enable | true | 同上删 | **不存在** |
| ro.arch | exynos9810 | 同上删 | **不存在** |

#### 4.2.2 System 集最终 set 键全表（62 键）

| 组（追加序） | 键 = 值 |
|---|---|
| A1 HardwareIdentity（15，L197） | ro.board.platform=gs101；ro.product.board=oriole；ro.boot.hardware.sku=GR1YH；ro.boot.product.hardware.sku=GR1YH；ro.product.brand_for_attestation=google；ro.product.device_for_attestation=oriole；ro.product.manufacturer_for_attestation=Google；ro.product.model_for_attestation=Pixel 6；ro.product.name_for_attestation=oriole；ro.product.first_api_level=31；ro.odm.build.media_performance_class=31；ro.bootloader=slider-15.3-13239612；ro.build.expect.baseband=g5123b-145971-250103-B-12866815；ro.build.expect.bootloader=slider-15.3-13239612；ro.oem.key1=GR1YH |
| A2 RuntimeHeap（4，L217） | dalvik.vm.heapgrowthlimit=256m；dalvik.vm.heapmaxfree=32m；dalvik.vm.heapminfree=8m；dalvik.vm.heaptargetutilization=0.5 |
| A3 AAudioPolicy（3，L450） | aaudio.hw_burst_min_usec=2000；aaudio.mmap_exclusive_policy=2；aaudio.mmap_policy=2 |
| A4 DisplaySurfaceFlinger（16，L320） | ro.opengles.version=196610；debug.sf.disable_backpressure=0；debug.sf.early.app.duration=16600000；debug.sf.early.sf.duration=16600000；debug.sf.earlyGl.app.duration=16600000；debug.sf.earlyGl.sf.duration=16600000；debug.sf.enable_adpf_cpu_hint=true；debug.sf.enable_gl_backpressure=1；debug.sf.frame_rate_multiple_threshold=120；debug.sf.late.app.duration=16600000；debug.sf.late.sf.duration=10500000；debug.sf.layer_caching_active_layer_timeout_ms=1000；debug.sf.treat_170m_as_sRGB=1；debug.sf.use_phase_offsets_as_durations=1；persist.sys.sf.color_saturation=1.0；persist.sys.sf.native_mode=2 |
| A5 GraphicsConfig（3，L312） | ro.egl.blobcache.multifile=true；ro.egl.blobcache.multifile_limit=33554432；graphics.gpu.profiler.support=true |
| A6 MediaPlatformDebug（9，L296） | debug.hwc.winupdate=1；debug.hwui.use_hint_manager=true；debug.mali.disable_backend_affinity=true；debug.slsi_platform=1；debug.stagefright.c2-poolmask=2031616（保留物理 Pixel 掩码并加分配器 ID 19/BLOB：虚拟 GKI 无 /dev/ion 与 /dev/dma_heap/system）；debug.stagefright.ccodec_delayed_params=1；media.c2.dmabuf.padding=512；media.mediadrmservice.enable=true；ota.warm_reset=0 |
| A7 ARTCompilerPolicy（9，L245） | dalvik.vm.dex2oat-cpu-set=0,1,2,3,4,5；dalvik.vm.dex2oat-threads=6；dalvik.vm.isa.arm.features=default；dalvik.vm.isa.arm.variant=cortex-a53；dalvik.vm.isa.arm64.variant=cortex-a55；dalvik.vm.restore-dex2oat-cpu-set=0,1,2,3,4,5；dalvik.vm.restore-dex2oat-threads=6；dalvik.vm.systemuicompilerfilter=speed；dalvik.vm.usejit=true |
| A8 StableRuntimeConfig（7，L259） | ro.com.android.prov_mobiledata=false；ro.gms.dck.eligible_wcc=2；ro.gms.dck.se_capability=1；ro.lmk.filecache_min_kb=153600；ro.lmk.kill_timeout_ms=50；ro.lmk.stall_limit_critical=40；ro.storage_manager.enabled=false |
| A9 PrivAppPolicy（1，L271） | ro.control_privapp_permissions=enforce |
| A10 BuildOTAIdentity（1，L290） | ro.build.ab_update=true |
| A11 VolumeStep（2，L226） | ro.config.media_vol_steps=25；ro.config.vc_call_vol_steps=7 |
| A12 LCDDensity（1，L233） | ro.sf.lcd_density=420 |
| A13 RadioPropertyPatches（3，L458） | persist.radio.uim.remote.mcc=（空）；persist.radio.uim.remote.slot=-1；ro.boot.hardware.radio.subtype=0 |
| B OrioleHardware 追加（3+1） | ro.hardware=oriole；ro.soc.manufacturer=Google；ro.soc.model=Tensor；persist.sys.usb.config=mtp |
| C android15Props 追加（2） | ro.build.version.security_patch=2025-05-05；ro.kernel.version=6.1 |
| D VirglGraphics（2，L139） | ro.hardware.egl=mesa；ro.opengles.version=196610 |
| E pixel6SystemPropertyPatches 存活（7，L60） | ro.product.cpu.abilist=arm64-v8a；ro.product.cpu.abilist32=（空）；ro.product.cpu.abilist64=arm64-v8a；persist.device_config.runtime_native_boot.iorap_perfetto_enable=false；service.adb.tcp.port=5555；persist.adb.tcp.port=5555；ro.debuggable=1 |
| F VerifiedPartition（8，L277） | partition.product.verified.check_at_most_once=0；partition.system_ext.verified.check_at_most_once=0；partition.system.verified.check_at_most_once=0；partition.vendor.verified.check_at_most_once=0；partition.vendor_dlkm.verified=2；partition.vendor_dlkm.verified.check_at_most_once=0；partition.vendor_dlkm.verified.hash_alg=sha256；partition.vendor_dlkm.verified.root_digest=8293495d4731cd8c1974e6c77ba7005ceb1456f97303f598b2da2d9eda46ebde（= §3.3 重打包后 vendor_dlkm 的 fs-verity 摘要，与 build-vendor_dlkm.py 校验闭环） |
| G rescueOff（1，L191） | persist.sys.disable_rescue=true |

> 计数：A1–A13 共 73 + B4 + C2 + D2 + E7 + F8 + G1 = 97 键次；扣除编译期删除 11 键与覆盖 8 键（ro.secure/ro.adb.secure/persist.sys.usb.config/egl/opengles/vulkan/cpuvulkan/native_window_buffers_format/mediaserver.64b/arch 计 9 个去重项）与跨组同键覆盖（ro.opengles.version 三次出现、egl 两次、secure/adb.secure/debuggable/persist.sys.usb.config 两次），净 set 62 键。
> `partition.vendor_dlkm.verified.root_digest` 必须等于 §3.3.4 写完、§3.3.5 校验通过的 vendor_dlkm.img 的 fs-verity 根摘要——两节互为闭环。

#### 4.2.3 SystemExt 集（2 键）

`pixel6Android15SystemExtPropertyPatches()`（L119）= 基础 `pixel6SystemExtPropertyPatches()`（ro.adb.secure=0、ro.audio.ihaladaptervendorextension_enabled=false）→ 再覆盖 **ro.adb.secure=1**。最终：`ro.adb.secure=1`、`ro.audio.ihaladaptervendorextension_enabled=false`。

#### 4.2.4 Default 集（default15Props）

`default15Props()`（L115）= `android15Props(pixel6DefaultPropertyPatches)`，其中 `pixel6DefaultPropertyPatches()`（L93）直接返回 `pixel6SystemPropertyPatches()`（与 System 同源）。因此 Default 集 = System 集去掉 F（VerifiedPartition 8 键）与 G（rescueOff 1 键），净 **53 键**。

#### 4.2.5 落盘期四类删除键全集（引擎从 GSI prop 文件移除既有键）

| 类别 | 组成 | 键（去重后） |
|---|---|---|
| SystemPropertyDeletes（31） | DisplaySF 3（L341）+ SetupWizard 15（L362）+ MissingUI 5（L402）+ BaseLegacy 2（L412）+ Qemu 1（L419）+ MediaServer64B 1（L425）+ CPUVulkan 1（L431）+ VulkanSelector 1（L437）+ TelephonyConfig 2（L395） | debug.sf.vsync_reactor_ignore_present_fences；ro.surface_flinger.enable_frame_rate_override；ro.surface_flinger.native_window_buffers_format；ro.setupwizard.enterprise_mode；ro.setupwizard.esim_cid_ignore；setupwizard.feature.baseline_setupwizard_enabled；setupwizard.feature.day_night_mode_enabled；setupwizard.feature.enable_gil；setupwizard.feature.enable_wifi_tracker；setupwizard.feature.lifecycle_refactoring；setupwizard.feature.notification_refactoring；setupwizard.feature.portal_notification；setupwizard.feature.show_pai_screen_in_main_flow.carrier1839；setupwizard.feature.show_pixel_tos；setupwizard.feature.show_support_link_in_deferred_setup；setupwizard.feature.skip_button_use_mobile_data.carrier1839；setupwizard.theme；ro.storage_manager.show_opt_in；ro.support_hide_display_cutout；ro.support_one_handed_mode；ro.thermal_warmreset；ro.wifi.channels；ro.arch；security.perf_harden；qemu.hw.mainkeys；ro.mediaserver.64b.enable；ro.cpuvulkan.version；ro.hardware.vulkan；ro.telephony.default_network；telephony.active_modems.max_count |
| SystemExtPropertyDeletes（1） | VulkanSelector（L437） | ro.hardware.vulkan |
| ProductPropertyDeletes（33） | CameraExtension 2（L443）+ 上表全部 31 | 上表 31 + ro.vendor.camera.extensions.package；ro.vendor.camera.extensions.service |
| DefaultPropertyDeletes（31） | 同 SystemPropertyDeletes | 同 SystemPropertyDeletes |

### 4.3 FileReplacement 20 条（gsi_property_bundle.go L21–42 声明序 = 执行序）

通用规则（`gsiprofile.FileReplacement`，plan.go L84–94）：源文件从参考镜像
`ReferenceImagePath`（表 R20 例外）按 `SourcePath` 提取 → 应用 StringRewrites
（**等长** ELF 字符串改写，如 libmedia.so→libm_gsi.so 同为 11 字节）→ 应用
BinaryPatches（按 `Offset` 就地十六进制替换）→ 写入 vdc 的 `Path`，并设
Mode/UID/GID/SELinuxXAttr。

#### 4.3.1 纯迁移类（10 条：R1–R4、R8、R14–R17、R20，不改动内容，只重设属主/标签——GSI 官方文件换到 virtual 化环境所需）

| # | Path（=SourcePath） | mode | uid | gid | SELinuxXAttr |
|---|---|---|---|---|---|
| R1 | /system/bin/mediaserver | 0o755 | 0 | 2000 | u:object_r:mediaserver_exec:s0 |
| R2 | /system/bin/drmserver | 0o755 | 0 | 2000 | u:object_r:drmserver_exec:s0 |
| R3 | /system/lib64/libmediaplayerservice.so | 0o644 | 0 | 0 | u:object_r:system_lib_file:s0 |
| R4 | /system/lib64/libresourcemanagerservice.so | 0o644 | 0 | 0 | u:object_r:system_lib_file:s0 |
| R8 | /system/lib64/libmedia_codeclist.so（pixel9 同名原件） | 0o644 | 0 | 0 | u:object_r:system_lib_file:s0 |
| R14 | /system/lib64/android.hardware.media.c2-V1-ndk.so（pixel9） | 0o644 | 0 | 0 | u:object_r:system_lib_file:s0 |
| R15 | /system/lib64/libcodec2_aidl_client.so（pixel9） | 0o644 | 0 | 0 | u:object_r:system_lib_file:s0 |
| R16 | /system/lib64/libcodec2_client.so（pixel9） | 0o644 | 0 | 0 | u:object_r:system_lib_file:s0 |
| R17 | /system/lib64/libsfplugin_ccodec.so（pixel9） | 0o644 | 0 | 0 | u:object_r:system_lib_file:s0 |
| R20 | /system/bin/bootanimation | 0o755 | 0 | 2000 | u:object_r:bootanim_exec:s0 |

> R14–R17 标注 "pixel9"：这四个 Codec2 接口库直接复用 pixel9 Android15 bundle 的同名
> replacement 函数（gsi_property_bundle.go L408–450），源/目标同路径纯迁移，为 §4.5 的
> V4L2 Codec2 数据面（android.hardware.media.c2-service + libc2plugin_store.so）提供
> system 侧依赖库。

#### 4.3.2 字符串改写类（6 条：libmedia.so/libgui.so 符号重定向到 R12/R13 垫片库）

| # | Path | mode/uid/gid/SELinux | StringRewrites（等长） | 语义 |
|---|---|---|---|---|
| R6 | /system/lib64/libstagefright_httplive.so | 0o644/0/0/system_lib_file | libmedia.so→libm_gsi.so | 依赖重定向到垫片 |
| R7 | /system/lib64/libstagefright_wfd.so | 同上 | libmedia.so→libm_gsi.so；libgui.so→libgux.so | 依赖重定向到两个垫片 |
| R9 | /system/lib64/libmedia_jni.so | 同上 | libmedia.so→libm_gsi.so；libgui.so→libgux.so | 同上 |
| R10 | /system/lib64/libmediandk.so | 同上 | libmedia.so→libm_gsi.so；libgui.so→libgux.so | 同上 |
| R11 | /system/lib64/libmedia.so | 同上 | libgui.so→libgux.so | 官方 libmedia 只需重定向 libgui 依赖 |
| R5 | /system/lib64/libstagefright.so | 同上 | libmedia.so→libm_gsi.so（在 pixel9 基础上追加，`appendELFStringRewriteIfMissing` 去重） | 见 4.3.3 |

#### 4.3.3 二进制补丁类（5 条：R5、R12、R13、R18、R19；其中 R5 兼属 4.3.2 字符串改写类）

**R5 libstagefright.so**（= pixel9 同名纯迁移 + StringRewrite + 3 处 BinaryPatches）：

| Offset | OldHex | NewHex | 说明 |
|---|---|---|---|
| 0xdd4fc | a13d0494 | 2e000094 | BL 调用改跳转微调（同长度 ARM64 指令替换） |
| 0x43c68 | 020400009f0b0000 | 0000000000000000 | 8 字节参数结构清零 |
| 0x119e0 | 807a0000 | 00000000 | 4 字节字段清零 |

**R12 libm_gsi.so（新增文件，源 = /system/lib64/libmedia.so）**：StringRewrites
libmedia.so→libm_gsi.so（改写自身 SONAME/依赖名）+ libgui.so→libgux.so，外加：

| Offset | OldHex | NewHex | 说明 |
|---|---|---|---|
| 0xa6d48 | 0800000000000000 | 0a00000000000000 | 8→10（ELF 头相关字段/符号表计数 8 字节改写） |

**R13 libgux.so（新增文件，源 = /system/lib64/libgui.so）**：StringRewrites
libgui.so→libgux.so（自身 SONAME 改写），外加：

| Offset | OldHex | NewHex | 说明 |
|---|---|---|---|
| 0x18d1a0 | 0800000000000000 | 0a00000000000000 | 同 R12：8→10 计数改写 |

> R12/R13 是「垫片对」：原 /system/lib64/libmedia.so（R11）与 libgui 的消费者在
> virtual 显示栈（virtio-gpu + mesa/angle）下改用 libm_gsi.so/libgux.so，两个垫片以
> 官方库为底、只改 SONAME 字符串与一处计数，并各自打上 R5/R7/R9/R10 依赖重定向。

**R18 screencap**（0o755/0/2000/u:object_r:system_file:s0）：

| Offset | OldHex | NewHex | 说明 |
|---|---|---|---|
| 0x4c44 | ff830791f44f45a9 | c80b80d2010000d4 | 函数序言（add sp,sp,#0x1e0; ldp x20,x21,[sp,#0x50]）→ `movz w8,#0x5e; svc #0`（94=exit_group）：虚拟帧缓冲下 screencap 直接退出，防崩溃 |

**R19 app_process64**（0o755/0/2000/u:object_r:zygote_exec:s0；
**唯一指定 SourceImagePath = `build/gsi-inputs/pixel6/15/oriole-bp1a.250505.005/system.img`**，即官方 factory system 而非 base15 参考镜像）：

| Offset | OldHex | NewHex | 说明 |
|---|---|---|---|
| 0x2998 | 2e0100942100805230010094 | 00008052c80b80d2010000d4 | `bl <fn>; mov w1,#1; bl <fn>` → `movz w0,#0; movz w8,#0x5e; svc #0`：zygote 启动期一处调用链短路 |

**R20 已并入 §4.3.1 表**（声明序最后一条）。

### 4.4 TextReplacement 8 条（逐条完整 Old/New；引擎=精确子串替换后按 Expected 回读校验）

#### T1 odsign 门控短路（/system/etc/init/hw/init.rc）

```diff
-    # Start the on-device signing daemon, and wait for it to finish, to ensure
-    # ART artifacts are generated if needed.
-    # Must start after 'derive_classpath' to have *CLASSPATH variables set.
-    start odsign
-
-    # Wait for odsign to be done with the key.
-    wait_for_prop odsign.key.done 1
+    # Start the on-device signing daemon, and wait for it to finish, to ensure
+    # ART artifacts are generated if needed.
+    # Pixel6 Android 15 BP1A GSI exits odsign before it sets init gates, so let
+    # init publish the verified state and continue zygote startup.
+    setprop odsign.key.done 1
+    setprop odsign.verification.success 1
+    setprop odsign.verification.done 1
```

原因：BP1A GSI 的 odsign 在设置 init 门控前退出，导致 `wait_for_prop` 永久阻塞、
zygote 不启动。改为 init 直接发布三个完成态属性。

#### T2 system_ext_file_contexts 追加 gs_watchdogd 路径（/system/system_ext/etc/selinux/system_ext_file_contexts）

```diff
 /system_ext/bin/gs_watchdogd            u:object_r:gs_watchdogd_exec:s0
+/system/system_ext/bin/gs_watchdogd     u:object_r:gs_watchdogd_exec:s0
```

GSI 挂载后 system_ext 实际路径带 `/system` 前缀，补一条 label 行防止恢复模式/
watchdog 落入 unlabeled。

#### T3 USB 启动期 gadget 置 none（/system/etc/init/hw/init.usb.rc）

```diff
 on boot && property:persist.sys.usb.config=*
-    setprop sys.usb.config ${persist.sys.usb.config}
+    setprop sys.usb.config none
```

云机用网络 adb（tcp 5555），启动期不激活 USB gadget。

#### T4 删除 adbd 启动行（/system/etc/init/hw/init.usb.rc，同文件第二处）

```diff
 on property:sys.usb.config=adb && property:sys.usb.configfs=0
     write /sys/class/android_usb/android0/enable 0
     write /sys/class/android_usb/android0/idVendor 18d1
     write /sys/class/android_usb/android0/idProduct 4EE7
     write /sys/class/android_usb/android0/functions ${sys.usb.config}
     write /sys/class/android_usb/android0/enable 1
-    start adbd
     setprop sys.usb.state ${sys.usb.config}
```

仅删 `start adbd` 一行：configfs=0 分支（虚拟机无真实 android_usb）不再拉起
USB adbd，adbd 由 tcp 路径自行启动。

#### T5 plat_sepolicy.cil 追加放行段（/system/etc/selinux/plat_sepolicy.cil）

锚点 `(allow adbd adbd_tradeinmode (process (dyntransition)))` 后追加（全部原文）：

```scheme
(allow shell proc (file (ioctl read getattr lock map open watch watch_reads)))
(allow shell sysfs (file (ioctl read getattr lock map open watch watch_reads)))
(allow webview_zygote zygote_exec (file (getattr open read map execute execute_no_trans)))
(allow webview_zygote tombstoned_crash_socket (sock_file (write)))
(allow webview_zygote logdr_socket (sock_file (write)))
(allow webview_zygote tombstoned (unix_stream_socket (connectto)))
(allow webview_zygote logd (unix_stream_socket (connectto)))
(allow webview_zygote misctrl_prop (file (getattr open read map)))
(allow shell timezone_prop (property_service (set)))
(allow shell locale_prop (property_service (set)))
(allow shell radio_prop (property_service (set)))
(allow shell telephony_status_prop (property_service (set)))
(dontaudit appdomain adb_data_file (dir (getattr search)))
(dontaudit untrusted_app adb_data_file (dir (getattr search)))
(dontaudit zygote adb_data_file (dir (search)))
(dontaudit appdomain qemu_sf_lcd_density_prop (file (read)))
(dontaudit untrusted_app qemu_sf_lcd_density_prop (file (read)))
(dontaudit isolated_app qemu_sf_lcd_density_prop (file (read)))
(dontaudit appdomain verity_status_prop (file (read)))
(dontaudit untrusted_app verity_status_prop (file (read)))
(dontaudit appdomain userdebug_or_eng_prop (file (read)))
(dontaudit untrusted_app userdebug_or_eng_prop (file (read)))
(dontaudit appdomain oem_unlock_prop (file (read)))
(dontaudit untrusted_app oem_unlock_prop (file (read)))
```

分组语义：①shell 读 proc/sysfs（探针）；②webview_zygote 读 app_process64 与
crash/log 套接字（boot 期 WebView 初始化）；③setup shell service 设
timezone/locale/radio/telephony 属性；④dontaudit 消噪（adb_data_file 与若干
prop 类）。

#### T6/T7 NFC 关闭 eSE 发现（两处同规则）

| # | 路径 | 替换 |
|---|---|---|
| T6 | /system/product/etc/libnfc-nci.conf | `NFA_MAX_EE_SUPPORTED=0x02` → `NFA_MAX_EE_SUPPORTED=0x00` |
| T7 | /system/etc/libnfc-nci.conf | 同上 |

云机无安全元件（eSE），保留 0x02 会让 NFCC 反复报错。

#### T8 plat_property_contexts 扩 verified 属性表（/system/etc/selinux/plat_property_contexts）

18 行旧表 → 23 行新表，diff 实质（重排 + 新增 5 行 vendor_dlkm/check_at_most_once）：

```diff
 partition.odm.verified        u:object_r:verity_status_prop:s0 exact string
+partition.vendor_dlkm.verified u:object_r:verity_status_prop:s0 exact string
+partition.system.verified.check_at_most_once     u:object_r:verity_status_prop:s0 exact string
+partition.system_ext.verified.check_at_most_once u:object_r:verity_status_prop:s0 exact string
+partition.product.verified.check_at_most_once    u:object_r:verity_status_prop:s0 exact string
+partition.vendor.verified.check_at_most_once     u:object_r:verity_status_prop:s0 exact string
+partition.vendor_dlkm.verified.check_at_most_once u:object_r:verity_status_prop:s0 exact string
 partition.system.verified.hash_alg     u:object_r:verity_status_prop:s0 exact string
+partition.vendor_dlkm.verified.hash_alg u:object_r:verity_status_prop:s0 exact string
 partition.system.verified.root_digest     u:object_r:verity_status_prop:s0 exact string
+partition.vendor_dlkm.verified.root_digest u:object_r:verity_status_prop:s0 exact string
```

为 §4.2.2 F 组的 8 个 partition.* 属性补 property_contexts 标签（verity_status_prop），
否则 init 拒载这些键。

### 4.5 LocalFile 8 条（宿主侧预编译产物直拷进 vdc；均带 Expected 回读锚点）

| # | guest Path | 宿主 SourcePath（cloud-droid-hub 仓内） | mode | uid | gid | SELinuxXAttr |
|---|---|---|---|---|---|---|
| L1 | /system/etc/cdh_pixel6_cpu_probe.ko | devices/pixel6-15/prebuilts/kernel/15/lineage-22.1-pixel6-15/modules/cdh_pixel6_cpu_probe.ko | 0o644 | 0 | 0 | —（system_file） |
| L2 | /system/etc/cdh_pixel6_device_info.ko | …/modules/cdh_pixel6_device_info.ko | 0o644 | 0 | 0 | — |
| L3 | /system/product/overlay/Pixel6FactoryAttestationOverlay.apk | devices/pixel6-15/output/guest-helper/overlay/pixel6/android15/Pixel6FactoryAttestationOverlay.apk | 0o644 | 0 | 0 | — |
| L4 | /system/bin/hw/android.hardware.media.c2-service | devices/pixel6-15/output/guest-helper/hal/pixel6/android15/android.hardware.media.c2-service | 0o755 | 0 | 2000 | **u:object_r:hal_codec2_default_exec:s0** |
| L5 | /system/lib64/libc2plugin_store.so | …/hal/pixel6/android15/lib64/libc2plugin_store.so | 0o644 | 0 | 0 | — |
| L6 | /system/lib64/libcodec2_hidl@1.0.so | …/hal/pixel6/android15/lib64/libcodec2_hidl@1.0.so | 0o644 | 0 | 0 | — |
| L7 | /system/lib64/libcodec2_aidl.so | …/lib64/libcodec2_aidl.so | 0o644 | 0 | 0 | — |
| L8 | /system/lib64/libstagefright_bufferqueue_helper_novndk.so | …/lib64/libstagefright_bufferqueue_helper_novndk.so | 0o644 | 0 | 0 | — |

- L1/L2：lineage 22.1 自编译内核模块（与 §3.1 表中 lineage odm/kernel 同源版本），guest 内
  由 init/脚本 `insmod`，供 cdh 探针读取 CPU/设备信息（对应 T5 的 shell 读 proc/sysfs）。
- L3：RRO 运行时资源覆盖包，覆盖键含
  `vendor_required_attestation_certificates`、`config_camera_sound_forced`、
  `power_profile`（Expected 锚点），让 Play Integrity / 电池配置走 factory 值。
- L4–L8：V4L2 Codec2 数据面五件套。**注意 vendor init 启动的是 system 侧服务**
  （见 §3.2.1-p 段 16）：vendor 内 default 实例切到
  `android.hardware.media.c2-service`，登记 c2.v4l2.avc/hevc 解码与 avc 编码；
  media_codecs_c2.xml 的注册表 patch 也在 vendor 侧完成，system 侧只放二进制与库。

### 4.6 GeneratedFile 19 条（内容全部由 Go 字符串生成；统一 TimestampUnix=1743029832 = build.date.utc）

| # | Path | mode/uid/gid | 内容要点 |
|---|---|---|---|
| G1 | /system/etc/init/cdh-adb-defaults.rc | 0o644/0/0 | 三触发器（zygote=running / boot_completed / user.0.ce_available）各 setprop service.adb.tcp.port+persist.adb.tcp.port=5555；exec_background u:r:shell:s0 打开 development_settings_enabled 1、adb_enabled 1、wm size 1080x2376（首个触发器带 user_setup_complete 轮询 36×5s；后两个带 sleep 15 兜底重放） |
| G2 | /system/etc/cdh-pixel6-15-setup.sh | 0o644/0/0 | 见下「setup 脚本五段」 |
| G3 | /system/etc/init/cdh-pixel6-15-setup.rc | 0o644/0/0 | service cdh_pixel6_15_setup（class late_start、user root、group root shell system、**seclabel u:r:shell:s0**、disabled、oneshot）；`on property:persist.sys.usb.config=` 空串时补写 mtp；`on property:init.svc.seriallogging=running` → stop；early-init 设 vendor.mesa.force.gl.vendor=ARM / renderer=Mali-G78；zygote/boot_completed/user.0.ce 三触发器各 start setup + 13 组 gsm.* setprop + persist.adb.tcp.port=5555 |
| G4 | /system/etc/init/usbd.rc | 0o644/0/0 | service usbd（class late_start、disabled、oneshot、user root、group root usb system）——覆盖官方同名 rc，保持 usbd 永不自动拉起 |
| G5 | /system/etc/permissions/android.software.telecom.xml | 0o644/0/0 | `<feature name="android.software.telecom" />` |
| G6 | /system/etc/permissions/cdh-virtual-hal-capabilities.xml | 0o644/0/0 | 23 个 feature 声明：usb.accessory×1；camera×5（camera/any/autofocus/flash/front）；fingerprint×1；location×3（location/gps/network）；sensor×9（accelerometer/barometer/compass/gyroscope/light/proximity/stepcounter/stepdetector + accelerometer 之前共 9 项）；nfc×4（nfc/any/hce/hcef）。头部注释说明各项声明前置条件（HAL 注册后才可见） |
| G7 | /system/system_ext/etc/permissions/com.google.android.camera.experimental2021.xml | 0o644/0/0 | `<library name=… file=/system_ext/framework/com.google.android.camera.experimental2021.jar />`：对齐真机 PackageManager 共享库清单 |
| G8 | /system/system_ext/framework/com.google.android.camera.experimental2021.jar | 0o644/0/0 | 内容=base64 内嵌 zip（classes.dex 含 com/google/android/camera/experimental2021/ExperimentalKeys），与 G7 配对（SystemConfig 要求 library 文件实际存在） |
| G9–G15 | /system/etc/permissions/android.hardware.telephony{,.calling,.data,.gsm,.messaging,.radio.access,.subscription}.xml | 0o644/0/0 | 7 个 telephony feature 单行声明（模板生成，同 G5 格式） |
| G16 | /system/system_ext/etc/permissions/cdh-pixel6-radio-oemril.xml | **0o640/0/1000** | privapp：com.samsung.slsi.telephony.oemril，5 权限（MODIFY_PHONE_STATE/READ_PRIVILEGED_PHONE_STATE/READ_PRECISE_PHONE_STATE/REBOOT/INTERACT_ACROSS_USERS） |
| G17 | …/com.shannon.imsservice.xml | 0o640/0/1000 | privapp：com.shannon.imsservice，11 权限（READ_PRIVILEGED/MODIFY_PHONE_STATE/CONNECTIVITY_INTERNAL/READ_PRECISE/WRITE_APN_SETTINGS/NETWORK_SIGNAL_STRENGTH_WAKEUP/CONNECTIVITY_USE_RESTRICTED_NETWORKS/BIND_IMS_SERVICE/LOCATION_BYPASS/LOCAL_MAC_ADDRESS/RENOUNCE_PERMISSIONS） |
| G18 | …/com.shannon.qualifiednetworksservice.xml | 0o640/0/1000 | privapp：8 权限（MODIFY_PHONE_STATE/NETWORK_SIGNAL_STRENGTH_WAKEUP/READ_PRECISE/READ_PRIVILEGED/MOUNT_UNMOUNT_FILESYSTEMS/CHANGE_NETWORK_STATE/SCHEDULE_EXACT_ALARM/USE_EXACT_ALARM） |
| G19 | …/com.shannon.rcsservice.xml | 0o640/0/1000 | privapp：8 权限（INTERACT_ACROSS_USERS/MODIFY_PHONE_STATE/CONNECTIVITY_INTERNAL/NETWORK_SIGNAL_STRENGTH_WAKEUP/READ_PRECISE/READ_PRIVILEGED/SCHEDULE_EXACT_ALARM/WRITE_APN_SETTINGS） |

**G2 setup 脚本五段结构**（`pixel6Android15SetupScriptFile()`，gsi_generated_files.go L237–454）：

1. SystemUI BLUETOOTH_CONNECT 授权（30×1s 轮询 pm grant）；
2. 时区/locale/rescue 固定：auto_time_zone=0、auto_time_zone_explicit=1、
   time_zone_detector 自动检测关、persist.sys.timezone=Asia/Shanghai、locale=en-US、
   disable_rescue=true；
3. radio 13 组 setprop（`pixel6Android15RadioPropertyCommands`，与 G3 内嵌同一函数）：
   gsm.version.baseband=g5123b-145971-250103-B-12866815、gsm.network.type=LTE、
   operator.alpha=中国联通、operator.iso-country=cn、isroaming=false、
   operator.numeric=46001、sim.operator.alpha=中国联通、sim.operator.iso-country=cn、
   sim.operator.numeric=46001、sim.state=READY、current.phone-type=1、
   ril-impl="Google Tensor Radio AIDL 3"；
4. 电池伪装拔出：cmd battery unplug + set ac/usb/wireless 0 + status 3（Overt 读
   ACTION_BATTERY_CHANGED 粘性广播）；
5. 主收敛循环 ×2（90×1s 至 8 应用禁用成功、60×3s 至 user_setup_complete）+
   收尾固定段 + 尾部 60×3s 时区钉死循环 + `while true; dismiss-keyguard; sleep 15`
   常驻守护；禁用清单：contacts/settings.intelligence/scone/chrome/tts/GoogleCamera/
   weather/pixel.dcservice + GMS 更新组件 9 项（SystemUpdateService 等 pm disable）；
   最终 `settings put global device_provisioned 1`、`secure user_setup_complete 1`、
   `setup_wizard_has_run 1`（global+secure）、`user_setup_personalization_state 10`、
   崩溃对话框三项关闭、wm density 420、wm size 1080x2400、am force-stop 4 包、
   回 HOME。
   注意与 G1 的分工：G1 在启动早期**开** development_settings/adb（供接管窗口），
   G2 收尾**关回 0**（生产态）。

### 4.7 FileRemovals 2 条（从 vdc 的 GSI system 删除）

| 路径 | 原因 |
|---|---|
| /system/product/etc/security/adb_keys | 禁用 GSI 自带 adb 公钥授权路径，adbd 走 ro.adb.secure=1+tcp 5555 的默认通道 |
| /system/etc/init/mmd.rc | GSI 的 mmd（内存管理守护，需要 zram0）在 base16 虚拟内核上无 zram 设备，直接删 rc 防其启动失败刷屏（pixel7 版本注释明确 "base16 virtual kernel does not provide zram0"） |

### 4.8 vdc 补丁总量小结

| 类别 | 数量 |
|---|---|
| 属性 set（System/SystemExt/Default） | 62 + 2 + 53 = 117 键 |
| 属性 delete（System/SystemExt/Product/Default） | 31 + 1 + 33 + 31 = 96 键（去重前） |
| 文件替换（迁移/字符串/二进制） | 20（纯迁移 10：R1–R4/R8/R14–R17/R20；字符串改写 6：R5–R7/R9–R11；二进制补丁 5：R5/R12/R13/R18/R19，R5 跨两类故 10+6+5−1=20；含 2 个新增文件名 libm_gsi.so/libgux.so） |
| 文本替换 | 8 |
| 本地文件注入 | 8 |
| 生成文件 | 19 |
| 删除文件 | 2 |

## 5. vdb：data.qcow2 用户数据盘（两链两种深度）

### 5.1 本仓链（cloud-phone-hub build/pack-roms.py `make_data_qcow2`，L1106–1127）

**空稀疏种子**，一条命令：

```
qemu-img create -f qcow2 -o compat=1.1,cluster_size=65536 \
    out/image/data.qcow2 118398996480        # DATA_VIRT_BYTES = 110 GiB
```

- 已存在且未 `--force` 时跳过（幂等）；
- 盘内**无文件系统、无种子**——f2fs 由 guest 首启时 fs_mgr/vold 对
  /dev/block/vdb 自行格式化（fstab 标 f2fs），daemon 侧以可写 overlay 叠于种子上；
- 产物仅 `vda.img` + `data.qcow2` 两个文件进 `out/image/`（IMAGE_DIR）。

### 5.2 基线链（cloud-droid-hub internal/creator/datadisk/run.go，1459 行）

`Initialize()` 完整流程（目录安全锁 + 原子发布，此处只列盘内容相关步骤）：

1. 工作目录 `build/init-data-*/`，创建 `data.raw`（0o600），
   `qemu-img create -f raw data.raw <Size>`；
2. **容器内 mkfs.f2fs**（缺 mkfs/debugfs 时走容器，镜像由 ensureDataCtr 拉起）：

   ```
   mkfs.f2fs -q -f -t 0 -g android -O compression -C utf8 data.raw
   ```

3. 种子目录 `f2fs-root/`（宿主 0o771，容器内 install/chown 构造后 sload）：

| 种子路径 | mode | uid:gid | SELinux（file_contexts 行） |
|---|---|---|---|
| / | 0771 | 当前用户 | `u:object_r:system_data_root_file:s0` |
| /local | 0771 | 0:0 | `u:object_r:system_data_file:s0`（SeedShellTmp） |
| /local/tmp | 0771 | 2000:2000 | `u:object_r:shell_data_file:s0`（SeedShellTmp） |
| /misc | 0771 | 1000:9998 | `u:object_r:system_data_file:s0`（SeedADBKeys） |
| /misc/adb | 0750 | 1000:2000 | `u:object_r:adb_keys_file:s0`（`/misc/adb(/.*)?`） |
| /misc/adb/adb_keys | 0640 | 1000:2000 | 宿主 adb 公钥（CDH_ADB_AUTH_KEYS） |
| /misc/adb/adb_temp_keys.xml | 0600 | 1000:1000 | adbd 临时密钥 XML（带时间戳） |

4. 落盘：`sload.f2fs -f f2fs-root -t / -T 0 -P -s data-file-contexts.txt data.raw`
   → `fsck.f2fs -f data.raw`；
5. `qemu-img convert -O qcow2` 到匿名临时文件 → `qemu-img check` → 原子发布为
   `data.qcow2`（0o600；ReadOnly 盘 0o444）；
6. 侧记 marker：fs marker（ext4/f2fs）、data-gen（版本号）、adbkey marker（公钥
   sha256）——已有盘且 fs/版本匹配则**保留**只走 `EnsureADBKeys`；不匹配才重建。
   ext4 路径（formatExt4Data）用 `mkfs.ext4 -q -F -m 0 -L userdata` + debugfs 写
   同款种子（`/local` 0x41e9、`/local/tmp` 0x41f9 uid/gid 2000），本机型不用。

> 结论：本仓链把「盘内预格式化+预置 adb 公钥」让渡给 guest 首启与 daemon 运行时
> （adb 走 tcp 5555，见 §4 G1/T3/T4），基线链则把种子固化进 qcow2。

## 6. 其余原样 LP 与内核产物（本仓链「零补丁件」清单）

### 6.1 super 内五个原样 LP（§3.1 表 #1/3/4/5/6，无任何 debugfs 改写）

| LP | 来源镜像 | 为什么可以原样 |
|---|---|---|
| system | factory `system.img` | 正常启动路径走 vdc（fstab.virtio.gsi.vdc 挂 /dev/block/vdc）；super 内 system 仅作 `gsifromsda` fallback 启动项 |
| product | factory `product.img` | 与 vdc 内 GSI 的 /product 同源同版本（BP1A.250505.005），属性/权限一致 |
| system_ext | factory `system_ext.img` | 同上；T2 只需在 vdc 侧补 file_contexts 行 |
| system_dlkm | factory `system_other.img` | GKI 模块分区，虚拟内核不加载其中模块，仅保持 LP 表完整 |
| odm | lineage `lineage-22.2/super/odm.img`（unpack-roms.py 拆出） | lineage 为 gs101/oriole 编译的 odm：提供 vendor 侧 RRO/配置基线；与 vendor 同源编译链 |

> 基线链对照：super 内 system/product/system_ext 被 slim-base **清零**（0x00 填充
> 但保留 LP 表项），内容让渡给 vdc；本仓链保留 factory 原件换取 `gsifromsda`
> 救援路径与 A/B 语义完整。odm 两链同源（lineage）。

### 6.2 内核产物（build/build-kernel.py 119 行 + kernel/build/*.sh）

| 项 | 值 |
|---|---|
| 源码树 | `kernel/source/`（lineage-22.1 内核源码，git worktree） |
| 编译脚本 | `kernel/build/build-pixel6-15-lineage-kernel.sh`（docker volume 沙箱，trap cleanup 原子发布） |
| builder 镜像 | 直跑脚本默认 `ghcr.io/feicong/droid-ddk:android15-6.6`；经 build-kernel.py 默认 `cloud-phone-hub-ubuntu-builder:latest`（缺失时 `docker load -i out/docker/ubuntu.tar`），可用 `CDH_PIXEL6_ANDROID15_KERNEL_BUILDER` 覆盖 |
| target / local_version / recipe | `android15-6.6` / `-4k-cdh` / `lineage-22.1-gki-virtio-wifi-pixel6-modules-v14` |
| 增量 config | `kernel/build/lineage-kernel.config`（如 `# CONFIG_DMIID is not set`：保 SMBIOS 但不向 app 暴露 DMI 桌面标识） |
| 外挂模块源码目录 | `kernel/modules/{pixel6_goodix_fingerprint, pixel6_cpu_probe, pixel6_device_info, virtio_media}` |
| 发布产物（out/kernel/） | `Image`、`config`、`Module.symvers`、`kernelrelease.txt`、`build-recipe.txt`、`SHA256SUMS` + `modules/` 下 8 个 ko：cdh_media、cdh_pixel6_cpu_probe、cdh_pixel6_device_info、cdh_pixel6_goodix_fingerprint、cdh_virt_wifi、cfg80211、rfkill、virtio_console |
| 归集（sync_kernel_dist） | ko 复制到 `out/dist/kernel/modules/`（copy2 保时间戳），供 build-vendor_dlkm.py（§3.3.4 的 5 ko）与 build-vendor.py（goodix ko，§3.2.2.3 表 E）消费；`Image` 供 build-boot.py（§2.3） |
| 幂等/校验 | 编译后强制检查 `out/kernel/Image` 非空；`out/kernel/modules` 目录缺失即异常退出（目录在但 0 个 ko 仅打印计数）；worktree 校验由 `check-kernel-worktree.sh`/`verify-module-versions.py` 辅助 |

> 与 §4.5 L1/L2 呼应：基线链把 cdh_pixel6_cpu_probe.ko / cdh_pixel6_device_info.ko
> 从 prebuilts 路径注入 vdc；本仓链 8 个 ko 中实际进镜像的是 vendor_dlkm 的 5 个
> （cdh_virt_wifi/cfg80211/rfkill/virtio_console/cdh_media，§3.3.4）与 vendor 的
> goodix（§3.2.2.3 表 E），cpu_probe/device_info 两个 ko 仅归集在
> out/dist/kernel/modules 备用（本仓链不产出 vdc，见 §7.0）。

## 7. 本仓 build/ 8 阶段制作流程（对齐 §2–§6 的事实清单）

### 7.0 两链差异总览（§3.1「§7 展开」的正式展开）

**基线链**：Stage A（justfile 3 步）→ Stage B（cdh device-prepare 16 步）→ Stage C
（patchDevice 5 步），全部「就地在既有盘/镜像上打补丁」。
**本仓链**：先产分区裸 img，再从零组装容器，最终只出 `vda.img + data.qcow2`
（vdc 沿用基线产物，不在本仓重建）。

| 产物 | 基线链做法 | 本仓链做法 | 差异本质 |
|---|---|---|---|
| 内核 | 预编译（prebuilts） | build-kernel.py 自编译（§6.2） | 源码自持 |
| boot.img | gsifix 就地补丁 firststage fstab | build-boot.py：lineage boot.img 基底 + 自编 Image 重打包 + firststage fstab cpio/lz4 注入（§2.3） | 重打包 |
| vendor_boot.img | 就地剔除隐藏键 | build-vendor-boot.py 剔除 console/theme 键（§2.4） | 同语义、不同载体 |
| vendor.img | lineage 基座 + 16 段脚本回填 | lineage 基座 + 单会话注入（§3.2.2，差异清单 §3.2.2.6） | 同一基座；官方件回填/自生成件已复刻（表 G），vintf/合并 rc/rro 三类刻意不复刻 |
| vendor_dlkm.img | 基线段 13 VendorDLKMPatch | build-vendor_dlkm.py（§3.3） | 同语义 |
| super.img | lineage super 就地（slim 清零三个 GSI LP） | pack_super.py 从零 liblp 6GiB、7 LP 原件（§3.0/§3.1） | 容器重建 |
| vda GPT | lineage vda 就地 | pack-roms.py build_vda 从零写 GPT（§2.1） | 容器重建 |
| GSI system（vdc） | Stage C patchDevice 5 步（gsiPatchBundle，§4） | **不产出**（沿用基线 system.qcow2） | 职责边界 |
| data.qcow2 | datadisk 预格式化 f2fs + 种子（§5.2） | 空稀疏种子（§5.1） | 种子让渡运行时 |

### 7.1 编排器 build/main.py（130 行）

STAGES 有序表（每项=短名→脚本→追加参数）：

| 序 | 短名 | 脚本 | 追加参数 | 对应章节 |
|---|---|---|---|---|
| 1 | unpack | unpack-roms.py | — | §7.2 |
| 2 | kernel | build-kernel.py | — | §6.2 |
| 3 | boot | build-boot.py | — | §2.3 |
| 4 | vendor_boot | build-vendor-boot.py | — | §2.4 |
| 5 | vendor | build-vendor.py | — | §3.2.2 |
| 6 | vendor_dlkm | build-vendor_dlkm.py | — | §3.3 |
| 7 | super | pack_super.py | `6442450944` + 7 个 `名=路径` + `out/dist/rom/super.img` | §3.0 |
| 8 | pack | pack-roms.py | （--force-pack 时追加 --force） | §7.3 |

- **单一数据源**：super 阶段参数用 importlib 按路径加载 pack-roms.py 取其
  `SUPER_SIZE/SUPER_PARTITIONS/ROM_DIST`，杜绝两处映射漂移；
- **依赖**：unpack 先于一切（vendor 需 roms/，boot 需 lineage-rom/）；kernel 先于
  boot（Image）/vendor（goodix ko）/vendor_dlkm（modules）；super 需 vendor+
  vendor_dlkm 产物；pack 最后；
- **CLI**：`--skip-unpack`（源已就位）、`--only STAGE`、`--from STAGE`、`--dry-run`
  （只打印）、`--force-pack`；任一脚本 rc≠0 即中止。

### 7.2 阶段 unpack（unpack-roms.py 325 行：源准备）

| 输入 | 动作 | 输出 |
|---|---|---|
| `roms/oriole-bp1a.250505.005-factory-9fc37bcc.zip` | 外层 zip 只解到临时目录定位内层 `image-oriole-bp1a.250505.005.zip`，取完即清；内层直接解出 | `roms/image-oriole-bp1a.250505.005/{boot,vendor,vendor_boot,product,system,system_ext,system_other,vbmeta…}.img` |
| `UTM-VM-lineage-22.2-20251011-jqssun-virtio_arm64only.zip`（成员 `LineageOS_on_arm64.utm/Data/vda.img`） | 解出 vda.img → 解析 GPT 分区表按偏移切分区 → 对 super.img 跑 **Python 版 lpunpack**（sparse 先 simg2img 转 raw） | `lineage-rom/lineage-22.2/{EFI.img,boot.img,metadata.img,super.img,…}` + `super/{system,vendor,product,odm,…}.img` |

- zip-slip 防护（非法成员路径跳过）；文件不存在 `[skip]`；`--list` 只打印 lineage
  分区表；参数可选 `factory`/`lineage` 子集。

### 7.3 阶段 pack（pack-roms.py 1208 行：终装）

```
pack():
 1) 7 个 super 源 + vda 源就位检查（缺一即退）
 2) super.img：存在跳过 / force 或缺失则 pack_super.build_super_image()
 3) read_super_image() 读回 7 LP（打印 size/data_sector）——liblp 完整性自证
 4) vda.img：EFI 分区取 prepare_efi_partition() 定制 grub 工作镜像（退回 lineage
    原始 EFI.img）；metadata.img 若非有效 ext4 → mke2fs 重格式化（否则 /metadata
    挂 Read-only，keystore2/vold/zygote 连锁启动失败）；build_vda() 从零写 GPT；
    EFI 比 vda 新（efi_newer）则强制重建
 5) make_data_qcow2()（§5.1）
 6) IMAGE_DIR 只保留 vda.img + data.qcow2
```

### 7.4 幂等性总表（重跑安全）

| 阶段 | 幂等策略 |
|---|---|
| unpack | 目标不存在才解压；内层 zip 不留中间层 |
| kernel | 每次全量编译（shell 脚本内部 docker volume 沙箱 + 原子发布；旧产物先 backup，失败回滚） |
| boot / vendor_boot | 无条件重建（不比对源时间戳，同输入同输出）；_atomic_write 原子落盘；vendor_boot 无隐藏键时逐字节原样复制 |
| vendor | Widevine 三态幂等（§3.2.2.3 表 F）；写入前 rm 同名；`_run_command_file` 用 fail_re 扫 debugfs 静默失败 |
| vendor_dlkm | 同上四联写（rm+write+sif+ea）；先删 RECLAIM 25 模块再写入 |
| super | out/dist/rom/super.img 存在即跳过 |
| pack | vda 按 efi_newer/force；data.qcow2 存在跳过 |

### 7.5 8 阶段产物依赖图

```
factory zip ──unpack──► roms/image-oriole-*/{vendor,system,product,system_ext,system_other}.img
lineage zip ──unpack──► lineage-rom/lineage-22.2/{EFI,boot,metadata,super/odm,…}.img
kernel/source ──kernel──► out/kernel/{Image,modules/*.ko} ──► out/dist/kernel/modules/
lineage boot.img + out/kernel/Image ──boot──► out/dist/rom/boot.img      ┐
lineage vendor_boot.img ──vendor_boot──► out/dist/rom/vendor_boot.img    ├─►
factory vendor.img + HAL 树 ──vendor──► out/dist/rom/vendor.img          │  pack ──► out/image/vda.img
factory vendor_dlkm 基座 + 5 ko ──vendor_dlkm──► out/dist/rom/vendor_dlkm.img ┘        + data.qcow2
                                    └─super──► out/dist/rom/super.img ──┘
```

## 8. 校验清单与排障（构建期→运行期）

### 8.1 构建期校验汇总

| 阶段 | 校验点 | 失败行为 |
|---|---|---|
| kernel | `out/kernel/Image` 非空、ko 清单打印 | sys.exit |
| boot | boot header 解析（kernel_size/ramdisk_size/header_size）、os_version 读回 | 退出 |
| vendor | resize2fs 后 e2fsck -fy；六项回读（§3.2.2.4）：文件存在/mode/uid/gid/SELinux/length+sha（Widevine 5459968B/01b4ff35…） | 退出 |
| vendor_dlkm | 五项回读（§3.3.5）：5 ko+4 索引存在与内容 | 退出 |
| super | read_super_image 读回 7 LP 名/size/data_sector | 退出 |
| pack | vda 源就位；metadata 重格式化后再验 ext4 有效 | 退出 |
| data | qemu-img create 返回码（基线链另有 qemu-img check） | 退出 |
| vdc（基线链） | 每个 TextReplacement/GeneratedFile/LocalFile 的 Expected 锚点回读；ELF BinaryPatch OldHex 前值比对 | 退出 |

### 8.2 跨分区闭环（改动一处必须同步另一处）

1. `partition.vendor_dlkm.verified.root_digest`（§4.2.2 F，vdc 属性）≡ §3.3 重打包
   vendor_dlkm.img 的 fs-verity 根摘要 `8293495d…ebde`——重打包后必须复算；
2. `fstab.virtio.gsi.vdc`（vendor，§3.2.2.3 表 G）≡ grub `androidboot.fstab_suffix=
   virtio.gsi.vdc`（§2.2.1）≡ vdc 承载 /system——三者缺一 vold 挂不上 vdc；
3. vendor_dlkm modules.load 4 行 ⊂ modules.dep 5 行 ⊇ 写入 5 ko（§3.3.2）；
4. vdc R14–R17（Codec2 接口库）依赖 L4–L8（V4L2 数据面五件套）同批存在；
5. T8 property_contexts 的 `partition.vendor_dlkm.verified*` 三行支撑 F 组 8 键
   （init 拒载无标签属性）。

### 8.3 常见故障速查（按现象→根因→修复）

| 现象 | 根因 | 修复/预防 |
|---|---|---|
| 开机卡 Google logo，日志有 `wait_for_prop odsign.key.done` | GSI odsign 提前退出，init 门控永不满足 | T1 已短路；确认 vdc 补丁完整（marker pixel6-15-v235） |
| /metadata 挂载 Read-only，keystore2/vold/zygote 连锁失败 | lineage 导出的 metadata.img 是空白镜像（非 ext4） | pack 阶段 ensure_metadata_ext4 用 mke2fs 重格式化（已内置） |
| avc denied: shell 时区/locale/gsm.* setprop | sepolicy 未放行 | T5 四组 property_service set（已内置） |
| mmd 反复崩溃/avc 噪声 | 虚拟内核无 zram0 | vdc FileRemovals 删 /system/etc/init/mmd.rc（已内置） |
| WiFi 扫描不可用/载入失败 | 旧 virt_wifi 模块残留在 vendor_dlkm | §3.3.3 RECLAIM 25 模块删除 + 写入 cdh_virt_wifi/cfg80211/rfkill |
| Widevine L3 播放失败 | APEX 未注入或 sha 不符 | vendor 六项回读校验（5459968B/01b4ff35…） |
| 分辨率/密度错乱 | G1（1080x2376）与 G2（1080x2400）两处 wm size 并存 | 属设计：早启 2376、setup 收尾 2400；排查时看最后生效者 |
| adb 连不上 | ro.adb.secure=1 + tcp 5555 需 adbd 已起 | 检查 T3/T4（USB gadget none + 删 start adbd）与 G1 三触发器 |
| debugfs 写入静默丢失 | debugfs 对不存在路径的 write 不报错 | build-vendor.py `_run_command_file` fail_re 扫描（已内置） |

### 8.4 回归建议

1. 重跑 `python3 build/main.py --from super` 前后 diff `out/dist/rom/super.img`
   sha256（super 阶段存在即跳过，强制重建需删文件或 --force-pack）；
2. vendor/vendor_dlkm 重打包后跑 `debugfs -R "stat <path>"` 抽查 mode/uid/gid 与
   `ea_get security.selinux`；
3. vdc 侧重放补丁以 `MarkerSuffix=pixel6-15-v235` 判定已打，避免二次叠加
   TextReplacement（Old 已消失会报错而非重复替换）；
4. 内核重编译后核对 `out/kernel/SHA256SUMS` 与 `kernelrelease.txt`
   （含 `-4k-cdh` local version），并同步 vendor_dlkm 索引（modinfo name 为准）。

---

## 附：文档自身的事实来源索引

| 章节 | 代码回溯 |
|---|---|
| §2.1–2.8 | pack-roms.py（GPT/EFI/metadata/cache/misc）、build-boot.py、build-vendor-boot.py、基线 justfile/gsifix |
| §3.0–3.1 | pack_super.py、pack-roms.py SUPER_PARTITIONS |
| §3.2.0–3.2.1 | cloud-droid-hub cmd/gsifix/vendor_policy.go 及其拼接的 16 段 |
| §3.2.2 | build/build-vendor.py（785 行） |
| §3.3 | build/build-vendor_dlkm.py（389 行） |
| §4 | cloud-droid-hub devices/pixel6-15/device/{gsi_property_bundle,gsi_properties,gsi_generated_files,codec2_v4l2_service,attestation_overlay}.go + pkg/devicekit/gsiprofile/plan.go |
| §5 | pack-roms.py make_data_qcow2、cloud-droid-hub internal/creator/datadisk/run.go |
| §6 | unpack-roms.py、build-kernel.py、kernel/build/build-pixel6-15-lineage-kernel.sh |
| §7 | build/main.py STAGES |
| §8 | 各脚本校验函数 + 代码注释中的因果说明 |


