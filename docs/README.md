# Pixel6 Android15（pixel6-15）构建过程与依赖

本文整理自 `cloud-droid-hub` 的 Pixel6 Android15 设备支持（设备仓
`cloud-droid-device-pixel6-15` + 主仓 `cloud-droid-hub` 通用执行引擎），记录
pixel6-15 的**构建过程与依赖**，供本仓（cloud-phone-hub）在 `devices/google/pixel6/`
下对齐实现。ROM 输入置于本目录 `roms/`。

> 说明：本文是「整理/梳理」，字段与步骤以 source 仓 `AGENTS.md`、`justfile`、
> `device/*.go`、`tools/kernel/*.sh` 为准。迁移到本仓时按本仓目录约定落位。

---

## 1. 概览与职责边界

Pixel6 Android15 是**一次典型的「通用虚拟化引擎 + 机型补丁」云手机**：

- **主仓（cloud-droid-hub）**：通用能力——QEMU 运行引擎、镜像拆分/合并、GSI 补丁、
  CDI、运行器、`cd c`/`cd d`/`cd h`/`cd r`/宿主 FIFO 桥。
- **设备仓（cloud-droid-device-pixel6-15）**：机型专属事实源——设备定义、ROM、
  HAL、KO、内核配方、EFI 变量模板、验收证据。
- **设备仓只提供声明式事实**（`Definition()/Runtime()/GPUCapability()`），编译进
  宿主 CLI；命令式执行（HAL 构建、镜像注入、QEMU 参数、GPU 渲染进程生命周期）由主仓实现，
  设备仓不复制执行代码。

### 设备事实快照
| 项 | 值 |
|----|----|
| 设备 ID | `pixel6-15`（短名 `pixel6` 不登记，避免与 Android16 冲突） |
| 代号 | `oriole`，Android 主版本 15 |
| 构建号 | `BP1A.250505.005` |
| 默认 ROM | `oriole-bp1a.250505.005` |
| SourceID | `oriole-20250505` |
| 宿主平台 | `darwin-arm64`、`linux-arm64` |
| 最小主仓版本 | `0.30.16` |
| 验证分辨率 | `1080x2400` |
| 三盘布局 | `base.qcow2`(vda)、`data.qcow2`(vdb)、`system.qcow2`(vdc) |

### 源码目录清单（统一 `src/` 根）

本仓所有源码**统一归拢在 `devices/google/pixel6/src/` 一个根目录**；`roms/` 只存官方固件输入（非源码）。外部源码以嵌套子模块固定在 `src/` 下，本仓自维护源码也直接落在 `src/` 子目录。

```
devices/google/pixel6/
├── README.md
├── roms/                          # 官方固件输入（factory ZIP，非源码）
└── src/                           # 全部源码统一根目录
    ├── device/                    # 编译期 Go 事实源（Definition/Runtime/GPU…）
    ├── kernel/                    # 私有派生内核（cloud-droid-kernel 的 pixel6-15 分支）
    ├── aosp/                      # AOSP android-15.0.0_r36（16 项 HAL 接口源码）
    ├── hal/                       # 机型 HAL（Android AArch64 服务，NDK r29 + Clang/LLD 22 构建）
    │   ├── radio/                 # cdh_pixel6_radio_service（模拟 RIL/Radio：sim/data/network/voice/msg…）
    │   ├── camera/                # cdh_pixel6_camera_provider_service（Camera Provider）
    │   ├── gnss/                  # cdh_pixel6_gnss_service（GNSS）
    │   ├── sensors/               # cdh_pixel6_sensors_service（AIDL sensors@2 + FMQ）
    │   ├── thermal/               # cdh_pixel6_thermal_service + cdh_thermal_model（热模型）
    │   ├── vibrator/              # cdh_pixel6_vibrator_service
    │   ├── lights/                # cdh_pixel6_lights_service
    │   ├── wifi/                  # cdh_pixel6_wifi_service + virt_wifi_probe + 扫描模型
    │   ├── nfc/                   # cdh_pixel6_nfc_service（NXPNFC/OPLUS AIDL + HIDL 桥）
    │   ├── fake_codec2/          # cdh_pixel6_codec2_store_service（占位/假编解码登记；仅上报组件清单给检测工具，不编解码；真解码走 v4l2_codec2）
    │   ├── v4l2_codec2/           # 上游 Codec2 组件（真实编解码：c2.v4l2.* 驱动 /dev/video* 对接 vhost-media）
    │   ├── nlinterceptor/         # cdh_pixel6_nlinterceptor_service（网络策略 netlink 拦截）
    │   ├── opencl_shim/           # cdh_pixel6_opencl_shim（OpenCL shim）
    │   ├── vulkan_stub/           # cdh_pixel6_vulkan_hal_candidate（Vulkan HAL）
    │   ├── gpu_probe/             # cdh_pixel6_gpu_loader_probe（GPU 探测）
    │   ├── adb-auth/              # adb_auth（ADB 认证）
    │   ├── adbd-netns-launcher/   # adbd netns launcher（网络命名空间控制）
    │   └── common/                # bionic15/libcxx-platform（基础运行时支持）
    ├── ko/                        # 外置 KO 工程（4 个，各含 .dddk-config/Kbuild）
    ├── overlay/                   # vendor overlay
    ├── config/                    # 机型内核配置（lineage-kernel.config）
    └── tools/                     # 构建配方（内核 build 脚本、worktree 检查…）
```

| `src/` 子目录 | 内容 / 来源 | 用途 |
|--------------|-------------|------|
| `device/` | 本仓维护 | 编译期身份/ROM/HAL/GPU 事实源（TEE/KeyMint 已抽到 cuttlefish） |
| `kernel/` | 子模块 `cloud-droid-kernel` 的 `pixel6-15` | GKI 6.6 内核 |
| `aosp/` | `android-15.0.0_r36`（16 项 HALSources） | HAL 构建接口源码 |
| `hal/` | 本仓维护 + 子模块 `cloud-droid-camera-hal`(pixel6-15) + AOSP `v4l2_codec2` | 机型 HAL（radio/camera/gnss/sensors/thermal/vibrator/wifi/nfc/lights/fake_codec2…，明细见下） |
| `ko/` | 本仓维护 | 外置 KO（`pixel6_goodix_fingerprint`/`pixel6_cpu_probe`/`pixel6_device_info`/`virtio_media`） |
| `overlay/` | 本仓维护 | vendor overlay |
| `config/` | 本仓维护 | 机型内核配置 `lineage-kernel.config` |
| `tools/` | 本仓维护 | 内核构建配方脚本 |

#### `src/hal/` 机型 HAL 明细

| HAL | 源目录 / 服务名 | 作用 |
|-----|-----------------|------|
| radio | `hal/radio/` → `cdh_pixel6_radio_service` | 模拟 RIL/Radio：SIM（`radio_sim`）、数据（`radio_data`）、网络（`radio_network`）、语音（`radio_voice`）、短信（`radio_messaging`）、配置（`radio_config`） |
| camera | `hal/camera/src/`（子模块 `cloud-droid-camera-hal` 的 `pixel6-15`）→ `cdh_pixel6_camera_provider_service` | Camera Provider |
| gnss | `hal/gnss/` → `cdh_pixel6_gnss_service` | GNSS 定位 |
| sensors | `hal/sensors/` → `cdh_pixel6_sensors_service`（AIDL sensors@2 + FMQ `cdh_fmq_support` + `cdh_sensor_event_model`） | 传感器事件模型 |
| thermal | `hal/thermal/` → `cdh_pixel6_thermal_service` + `cdh_thermal_model` | 热管理温度模型 |
| vibrator | `hal/vibrator/` → `cdh_pixel6_vibrator_service` | 振动/触觉反馈 |
| lights | `hal/lights/` → `cdh_pixel6_lights_service` | 指示灯状态 |
| wifi | `hal/wifi/` → `cdh_pixel6_wifi_service` + `virt_wifi_probe` + `cdh_wifi_scan_model` | 虚拟 WiFi 扫描 |
| nfc | `hal/nfc/` → `cdh_pixel6_nfc_service`（NXPNFC/OPLUS AIDL + HIDL 桥） | NFC 近场通信 |
| fake_codec2 | `hal/fake_codec2/` → `cdh_pixel6_codec2_store_service`（占位/假编解码登记：`listComponents` 报 Pixel6 OEM 档位供检测工具，`createComponent` 一律 `CANNOT_DO`，仅上报不编解码）、`hal/v4l2_codec2/`（上游 `c2.v4l2.*`，真实编解码，驱动 `/dev/video*` 对接 vhost-media，独占 `default` 实例；占位 store 只在 `default1/2` 实例） | 编解码能力（假登记表 + 真实现，按实例分治） |
| nlinterceptor | `hal/nlinterceptor/` → `cdh_pixel6_nlinterceptor_service` | 网络策略 netlink 拦截 |
| opencl_shim | `hal/opencl_shim/` → `cdh_pixel6_opencl_shim` | OpenCL shim |
| vulkan_stub | `hal/vulkan_stub/` → `cdh_pixel6_vulkan_hal_candidate` | Vulkan HAL |
| gpu_probe | `hal/gpu_probe/` → `cdh_pixel6_gpu_loader_probe` | GPU 探测 |
| adb-auth | `hal/adb-auth/` → `adb_auth` | ADB 认证 |
| adbd-netns-launcher | `hal/adbd-netns-launcher/` → `adbd-netns-launcher` | 网络命名空间控制 |
| common | `hal/common/` → bionic15/libcxx-platform | 基础运行时支持（`cdh_crtbegin_dynamic`、`cdh_crtend_android`、`__config_site`） |

> 构建入口见 3.4；实际接线（镜像注入方式、能力声明等）以设备事实与后续对齐为准。

---

## 2. 依赖清单

### 2.1 必须的宿主工具
`go`（≥1.26）、`just`、`git`、`docker`、`tar`、`sha256sum`、`llvm-strip`（内核侧）、
`python3`（dry-run/辅助脚本）。

### 2.2 工具链
以 Android 为目标的 **C/C++ 用户态组件与启动对象**固定：
- **NDK r29**，revision `29.0.14206865`（取 sysroot）。
- **Clang/LLD 22**（宿主原生，非 NDK 自带交叉编译器；宿主架构本机架构）。
- 构建前必须先校验 `source.properties`、Clang 与 LLD 主版本、编译器可执行文件宿主架构。
- 禁止 NDK r30、Clang 21 或其它主版本、跨宿主架构编译器、静默回退。

内核/KO、QEMU 宿主程序、纯宿主 TEE 不使用 NDK，各自使用登记工具链：
- 内核/KO：**DroidDDK** 镜像 `ghcr.io/feicong/droid-ddk:android15-6.6`，
  整内核用完整镜像，外置 KO 用 DroidDDK 精简镜像（`slim=true`）。

### 2.3 外部源码 / 仓库依赖
（以下全部源码统一归拢在 `devices/google/pixel6/src/` 根下，见上文「源码目录清单」）

| 依赖 | 统一根下目录 | 来源 | 用途 |
|------|--------------|------|------|
| 内核源码 | `src/kernel/` | 私有统一仓 `cloud-droid-kernel` 的 `pixel6-15` 分支 | GKI 6.6 内核 |
| Camera HAL | `src/hal/camera/src/` | 私有统一仓 `cloud-droid-camera-hal` 的 `pixel6-15` 分支 | Camera HAL |
| AOSP HAL 接口 | `src/aosp/` | `android-15.0.0_r36`（16 项 HALSources） | HAL 构建 |

Android15 AOSP 固定来源**统一 `android-15.0.0_r36`**（16 项：
audio/bluetooth-audio/camera/codec2/drm/fingerprint/gnss/keymint/light/
netlinkinterceptor/nfc/radio/sensors/thermal/vibrator/wifi）。

### 2.4 官方固件与镜像输入
- 官方 factory ZIP：`oriole-bp1a.250505.005-factory-9fc37bcc.zip`
  （URL `…/dl.google.com/dl/android/aosp/…`，SHA-256 `9fc37bcc…3316c1`），本仓放在
  `devices/google/pixel6/roms/`（源仓主仓 `prebuilts/`）。
- 启动盘基线：GKI 6.6 `base.qcow2`。
- 每个 ROM 保存自己的只读 `efi_vars.fd` 模板（约 67.5MB，普通 Git blob，禁用 Git LFS）；
  运行实例必须复制到独立输出目录。

### 2.5 内核 KO 清单
`virtio_console.ko`、`rfkill.ko`、`cfg80211.ko`、`cdh_virt_wifi.ko`
+ 项目自建 4 个：`cdh_pixel6_goodix_fingerprint.ko`、`cdh_pixel6_cpu_probe.ko`、
`cdh_pixel6_device_info.ko`、`cdh_media.ko`。

---

## 3. 构建流程

### 3.1 准备 / 检出
1. 检出设备仓，并更新 `src/` 下嵌套子模块：
   - `src/kernel/` → `cloud-droid-kernel` `pixel6-15`（固定 branch + 已验收 gitlink）
   - `src/hal/camera/src/` → `cloud-droid-camera-hal` `pixel6-15`
2. 设备仓 `go test ./...`（覆盖源码路径、KO 缓存复用等契约）。

### 3.2 编译设备事实进宿主 CLI
主仓显式聚合 `device.Definition()`、`device.Runtime()`、`device.GPUCapability()`，
编译进 `cdh`、`cdd`、`cdc`、`cdr`。设备身份/ROM/HAL/GPU/Radio/端口/镜像注入方式
都由这些编译期事实源承担。（TEE/KeyMint 已抽到 cuttlefish，不再经设备仓事实源。）

### 3.3 内核构建（DroidDDK 容器）
入口：`just build-kernel` → `src/tools/kernel/build-pixel6-15-lineage-kernel.sh`。
1. 检查源码：`src/kernel/Makefile`、`gki_defconfig`、`virtio.config`、`wifi.config`、
   机型 config `src/config/lineage-kernel.config`、
   `src/ko/*` 各 KO 工程的 `Kbuild`。
2. 复用判定：目标目录已含 `Image/kernelrelease.txt/build-recipe.txt/SHA256SUMS`，
   `kernelrelease = 6.6.127-4k-cdh`、`build-recipe = lineage-22.1-gki-virtio-wifi-pixel6-modules-v14`，
   且 `sha256sum -c` 通过即复用（`CDH_REFRESH_…=1` 强制重建）。
3. 归档进大小写敏感 Linux Docker volume：
   - 带 Git 元数据：`git archive HEAD`（macOS 大小写不敏感文件系统无法表达
     13 组同名文件，先跑 `check-kernel-worktree.sh`）。
   - 无 Git 元数据：直接 `tar` 归档源码目录。
4. 容器内构建（`LLVM=1`、`ARCH=arm64`）：
   ```
   make O=out ARCH=arm64 LLVM=1 gki_defconfig
   merge_config.sh -m -r .config  src/kernel/arch/arm64/configs/lineageos/virtio.config \
       src/kernel/arch/arm64/configs/lineageos/peripheral/wifi.config \
       src/config/lineage-kernel.config
   scripts/config --file out/.config --set-str LOCALVERSION "-4k-cdh" \
       --disable LOCALVERSION_AUTO --module VIRTIO_CONSOLE
   make O=out ARCH=arm64 LLVM=1 olddefconfig
   make O=out ARCH=arm64 LLVM=1 -j<N> Image modules
   # KO 工程逐个 make M=src/ko/<name> modules
   ```
5. 产物装配：`Image`、`.config`→`config`、`Module.symvers`、`kernelrelease.txt`、
   各 `.ko`→`modules/`，`llvm-strip --strip-debug`，写 `build-recipe.txt`，
   `(cd export && sha256sum … > SHA256SUMS)`。
6. 发布（原子）：先 `mktemp` 候选目录，校验后替换 `prebuilt_dir`。

发布目录（设备仓内，普通 blob）：
`prebuilts/kernel/15/lineage-22.1-pixel6-15/`（Image、modules/*.ko、Module.symvers、SHA256SUMS）。

### 3.4 HAL / KO 构建
- **HAL（Android 用户态服务）**：以 NDK r29 `29.0.14206865` sysroot + 宿主原生
  Clang/LLD 22，构建 Android AArch64 服务/插件；AOSP 来源统一 `android-15.0.0_r36`；
  AIDL 只消费上游快照原生 `.hash`。产物安装到 `/vendor/bin/hw/`（vendor 私有路径，
  由 vendor init 直接启动并注册标准接口），不得触碰/替换官方 APEX。
- **Codec2（v4l2_codec2）**：随机型 HAL 一起构建（`src/hal/v4l2_codec2/`），
  `c2.v4l2.*` 组件 + `android.hardware.media.c2@1.2-service-v4l2` 服务消费
  `/dev/video*`，即 vhost-media 暴露的 V4L2 节点（见「与 vhost-media 的关系」）。
- **KO**：4 个外置 KD 工程使用 `.dddk-config`（`version/mode/source/slim=true`），
  由 droid-ddk 工具按宿主架构选 Linux arm64 / x86_64 入口构建精简 KO。

### 3.5 镜像准备与补丁
`just prepare-device-pixel6-15`：
1. 从官方 factory ZIP 提取并重组，产出三盘 `base.qcow2` / `data.qcow2` / `system.qcow2`。
2. 注入机型 HAL（radio/gnss/camera/sensors/thermal/vibrator/wifi/nfc…）、KO、
   overlay、`efi_vars.fd`、SELinux/vendor marker 补丁。
3. 保持官方 APEX 与 `/apex/` 原样，禁止改写/重签/替换/遮蔽。
固化已镜像直接跑 `just device-pixel6-15`。

### 3.6 运行与验收
- 运行：`just device-pixel6-15`（或 `output/<goos>-<goarch>/bin/cdh`），QEMU 引擎 +
  三盘 + 模拟 Radio；
- 验收/QA：
  - `just radio-simulated-qa`：模拟 Radio 状态与业务注入；
  - `just wallpaper-runtime-qa`：SystemUI/墙纸画面；
  - 音视频/编解码/GPU 按声明的 Codec 档位（30/300/600 帧，两轮）验收；
  - `cdh device-matrix-qa -devices pixel6-15 -refresh=true` 刷验收到证据目录。

---

## 4. 产物与发布位置

| 产物 | 位置（源仓约定） |
|------|------|
| 内核 Image + KO + Module.symvers + SHA256SUMS | 设备仓 `prebuilts/kernel/15/lineage-22.1-pixel6-15/` |
| 三盘镜像、ROM 版目录 | `prebuilts/gsi/pixel6/15/oriole-bp1a.250505.005/` |
| 构建中间/未验收生成物 | 仅 `build/`、`output/` |
| 验收证据 | `docs/pixel6/evidence/15/oriole-bp1a.250505.005/` |
| 宿主 CLI | `output/<goos>-<goarch>/bin/cdh`（现行路径） |

---

## 5. 硬性约束（红线）

1. **工具链锁定**：Android 目标用户态一律 NDK r29 + Clang/LLD 22；禁止 r30/Clang21/
   跨宿主/静默回退；构建前校验 `source.properties`/版本/架构。
2. **AOSP tag 锁定**：Android15 一律 `android-15.0.0_r36`；16 项 HALSources 必须逐项发布，
   不得删改排序或换 tag。
3. **APEX 原样**：任何分区更新保持官方 APEX 与路径原样；自建 HAL 只装 `/vendor/bin/hw/`。
4. **禁用 Git LFS**：`efi_vars.fd` 等大文件作普通 Git blob。
5. **源码不含摘要**：源码只记远端/分支/发布 tag，配置/文档/构建摘要不记源码 commit 与摘要；
   非源码输入（ROM/镜像/APK/ELF/证书）正常校验。
6. **内核大小写敏感**：macOS 默认文件系统无法完整表达内核同名文件，必须在 Linux
   大小写敏感 Docker volume 中构建；`git archive HEAD` 取本节点。
7. **构建中间物** 只能写 `build/`、`output/`；已验收可复用内核/KO 再入库 `prebuilts/kernel/`。