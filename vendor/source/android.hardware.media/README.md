# android.hardware.media — note13-5G-15 Codec2 假登记 store

> **形态对齐 note13 真机**（gold / 2312DRAABC / Android 15）：真机 `/vendor` 的两个
> media.c2 store 二进制、两条 rc、两个 vintf manifest、7 个 media_codecs xml
> 全部按同名同路径复刻，组件清单取自真机 `dumpsys media.player`。

---

## 0. 模块定位

`IComponentStore`（AIDL `android.hardware.media.c2`）的**假登记实现**：

- `listComponents()` 按 `config/codec2.json` 静态表应答组件 traits；
- 组件级 `IConfigurable::querySupportedValues` 应答 profile/level（同真机 store，
  见 §9）——否则 framework 会退回 mime 默认上限、能力塌缩；
- `createComponent()` / `copyBuffer()` / `createInputSurface()` 全部返回 `Status::CANNOT_DO`
  ——组件不可实例化，仅用于让框架侧 `MediaCodecList` 列出与真机同构的编解码器清单。

真机上每个 store 由**独立二进制**承担，实例名不来自命令行参数，而由二进制名决定；
本模块同款（一份源码编译出两个二进制）：

| 二进制（=`/vendor/bin/hw/` 内名） | 注册实例 | 内容 |
|---|---|---|
| `android.hardware.media.c2-mediatek` | `IComponentStore/default` | MTK 硬件 15 组件（11 解码 + 4 编码，`c2.mtk.*`，rank=512） |
| `vendor.dolby.media.c2-default-service-dax` | `IComponentStore/default9` | Dolby 音频 2 组件（`c2.dolby.eac3.decoder` / `c2.dolby.ac4.decoder`，rank=8） |

`software` 实例（`c2.android.*` 软解全套）由 APEX
`/apex/com.android.media.swcodec` 提供，**与本模块无关**。

视频编解码实际能力：system/apex 侧 `c2.android.*` 软解（本模块只补清单，不提供编解码执行）。

## 1. 目录布局

```
build/vendor/source/android.hardware.media/
├── README.md                      # 本文档
├── build.py                       # 编排：AIDL 生成 → targets.inc → 编译 → 链接双二进制 → 资产落盘 → 标签登记
├── deps.py                        # 依赖准备：AIDL 展开 + 链接期 ABI 库（3 库）
├── .gitignore                     # /aidl/aosp（生成物，与 wifi 等模块一致）
├── aidl/
│   └── aosp/                      # 编译 -I 根（deps.py 展开）
├── config/
│   └── codec2.json                # 契约：schema cdh.note13.android15.codec2.v1，2 stores / 17 组件
├── src/
│   └── cdh_codec2_store_service.cpp    # BnComponentStore 占位实现（唯一源文件）
├── test/                          # mediandk 编解码测试客户端（不参与服务编译）
└── vendor/etc/                    # 真机 /vendor/etc 同名同路径资产
    ├── init/
    │   ├── android.hardware.media.c2-mediatek.rc              # 真机原样 + seclabel
    │   └── vendor.dolby.media.c2-default-service-dax.rc       # 真机原样 + seclabel
    ├── vintf/manifest/
    │   ├── manifest_media_c2_default.xml                      # 真机原样（default）
    │   └── vendor.dolby.media.c2-default-service-dax.xml      # 真机原样（default9）
    └── media_codecs{,_c2,_dolby_audio,_google_audio,_google_video_le,_performance,
        _system_default}.xml                                   # 真机 7 件套原样
```

> `media_codecs.xml`（入口）内含 `<Include href="media_codecs_mediatek_audio.xml" />`，
> 但该分表真机 `/vendor/etc` 亦不存在（Include 静默跳过）——本模块保持同形态，不新建。

## 2. 构建编排（build.py）

1. **AIDL 代码生成**：4 个版本化包，共用 `-I aidl/aosp`：
   `aidl --lang=ndk --structured --stability=vintf --min_sdk_version=35 --version=N --hash=H`
2. **`codec2_targets.inc`**：由 `config/codec2.json` 生成 17 组件静态表 + 别名数组
   + 各组件 profile/level 表（13 条组件带该信息，见 §9）。
3. **编译 store 源**：`src/cdh_codec2_store_service.cpp` + AIDL 生成的 NDK `.cpp`。
4. **链接两个 PIE**（同一批 .o 链接两次，仅输出名不同）：
   `-fPIE -pie -Wl,--dynamic-linker,/system/bin/linker64 -z relro -z now`，
   crtbegin/crtend 来自 `build/vendor/source/common/bionic15`。
5. **vendor 静态资产落盘**：`VENDOR_ASSETS` 11 项（2 rc + 2 vintf + 7 media_codecs xml）。
6. **baselines 标签登记**（LABELS 13 条，见 §5）。

运行时实例选择：`main()` 读 `/proc/self/exe` basename，命中
`android.hardware.media.c2-mediatek` → `default`，命中
`vendor.dolby.media.c2-default-service-dax` → `default9`；rc 不传任何参数（同真机）。
显式 `--instance <name>` 仍支持（`--instance` 优先于 basename）。

## 3. 依赖（deps.py）

**链接期 ABI 库仅 3 个**，全部来自 system 侧（`out/dist/vendor-compile-dep/media/system/lib64/`）：

| 库 | 说明 |
|---|---|
| `libc++.so` | C++ 运行时（VNDK APEX） |
| `libbinder_ndk.so` | AIDL NDK binder |
| `libnativewindow.so` | Surface/ANativeWindow |

`-llog -ldl -pthread -lm -lc` 走 NDK sysroot。

**AIDL 输入**：`.gitignore = /aidl/aosp`，由 `deps.py` 从 `CDH_AIDL_AOSP` 或下载缓存展开。

## 4. 产物清单（相对 `VENDOR_STAGE_DIR` 即镜像内 `/vendor`）

| 路径 | 说明 |
|---|---|
| `bin/hw/android.hardware.media.c2-mediatek` | MTK store 二进制（注册 default） |
| `bin/hw/vendor.dolby.media.c2-default-service-dax` | Dolby store 二进制（注册 default9） |
| `etc/init/android.hardware.media.c2-mediatek.rc` | MTK 服务定义（对齐真机字段） |
| `etc/init/vendor.dolby.media.c2-default-service-dax.rc` | Dolby 服务定义（对齐真机字段） |
| `etc/vintf/manifest/manifest_media_c2_default.xml` | `IComponentStore/default` 声明 |
| `etc/vintf/manifest/vendor.dolby.media.c2-default-service-dax.xml` | `IComponentStore/default9` 声明 |
| `etc/media_codecs*.xml`（7 个） | 真机静态注册表（入口 + c2 + dolby/google audio + google video_le + performance + system_default） |

## 5. SELinux / baselines

**baselines 标签登记**（`build.py` LABELS，13 条）：两个 bin →
`u:object_r:hal_codec2_default_exec:s0`；2 rc + 2 vintf + 7 xml →
`u:object_r:vendor_configs_file:s0`。

**rc**：两服务均 `user mediacodec`、**追加** `seclabel u:r:mediacodec:s0`
（真机 rc 无 seclabel——真机 `hal_codec2_default` 域由真机 sepolicy 提供；
note13 基座 lineage sepolicy 无该独立域，故显式落 `mediacodec` 域，
exec 文件类型仍走 `hal_codec2_default_exec`）。
其余字段（service 名、二进制路径、group、ioprio、task_profiles）与真机逐字段一致。

**`build/vendor/build/sepolicy.py` media 段**（`media_hal_cil()` / `MEDIA_VSC_LINES`）：

- `init` 域对 exec 文件 `read/open/getattr/execute/map` + `init → mediacodec` process transition；
- `mediacodec` 域对 exec 文件 `entrypoint/read/open/getattr/execute/map`；
- `mediacodec` 域注册 AIDL 服务 `android.hardware.media.c2.IComponentStore/<instance>`
  （service_contexts 为**精确实例名**匹配：`default` 由 lineage/AOSP 的
  `plat_service_contexts` 提供，本模块在 `vendor_service_contexts` 只补 **`default9`**
  一条——与真机"vendor 只声明额外实例"一致，也避免 libselinux
  `Multiple same specifications` 告警）；
- `mediacodec` 域 `ashmem_device` chr_file 访问（FMQ 需要）；
- 组件不可实例化（`createComponent` 返回 `CANNOT_DO`），**无** `/dev/video*`、dma-buf 设备访问需求。

## 6. 实测阻塞（已修复）

### 6.1 store 服务 SIGSEGV —— `mediacodec` 域 `map` 权限

首次云机验证（2026-09-23）暴露：store 服务启动即 SIGSEGV、4 分钟重启 4 次，
`MediaCodecList` 阻塞约 8s 不返回。avc 拒权原文：

```
avc: denied { map } for comm="android.hardw"
  path="/mi_ext/vendor/bin/hw/android.hardware.media.c2-mediatek"
  dev="vdc"
  scontext=u:r:mediacodec:s0 tcontext=u:object_r:hal_codec2_default_exec:s0
  tclass=file permissive=0
```

根因是 SELinux 域权限不足，非代码缺陷：`mediacodec` 对 exec 文件起初只有 `entrypoint`；
execve 之后动态链接器需 `open/read/getattr/mmap` 装载代码段，缺失即 `map` 被拒 → SIGSEGV。
现已补全为 `entrypoint read open getattr execute map`，服务稳定运行（无重启循环）。

### 6.2 `default9` 注册被拒 —— service_contexts 为精确实例名匹配

`default`（mtk）注册成功但 `default9`（dolby）每 5s 重启一次：

```
avc: denied { add } for pid=5269 uid=1046
  name=android.hardware.media.c2.IComponentStore/default9
  scontext=u:r:mediacodec:s0
  tcontext=u:object_r:default_android_service:s0
  tclass=service_manager permissive=0
```

原判据（`grep vendor_service_contexts` 见裸接口名一行）曾误以为 service_contexts 支持
**接口名前缀**匹配。实测真机与 guest 后确认是**精确实例名**匹配：

| 文件 | 内容 |
|---|---|
| 真机 + guest `plat_service_contexts` | `.../default`、`.../default1`、`.../default2`、`.../software` 四行 |
| 真机 `vendor_service_contexts` | 额外实例（含 `.../default9`；shell 无读权限，按 service 列表 + 机制推定） |

故 `default` 能注册只是"plat 恰好声明了它"，`default9` 无人声明 → 落
`default_android_service` → `add` 被拒。修复：`MEDIA_VSC_LINES` 只登记 `default9`
（与真机"vendor 只补额外实例"一致）；重复声明 `default` 会触发 libselinux
`Multiple same specifications` 告警，故不重复登记。

## 7. 命名对照（真机 ↔ 本模块）

| 真机 note13 `/vendor` | 本模块 |
|---|---|
| `bin/hw/android.hardware.media.c2-mediatek` | 同名 |
| `bin/hw/vendor.dolby.media.c2-default-service-dax` | 同名 |
| `etc/init/android.hardware.media.c2-mediatek.rc` | 同名（+ seclabel） |
| `etc/init/vendor.dolby.media.c2-default-service-dax.rc` | 同名（+ seclabel） |
| `etc/vintf/manifest/manifest_media_c2_default.xml` | 同名 |
| `etc/vintf/manifest/vendor.dolby.media.c2-default-service-dax.xml` | 同名 |
| `etc/media_codecs*.xml`（7 个） | 同名 |
| `config/codec2.json`（组件清单来源：`dumpsys media.player`） | 同名（schema `cdh.note13.android15.codec2.v1`） |

## 8. 验证步骤

1. `python3 build.py`：AIDL 4 包 + targets.inc（17 组件）+ 双 PIE + 11 项资产 + 13 条标签。
2. `python3 build.py --dry-run`：只打印不执行。
3. 云机（实例 23）：

   ```
   adb shell 'ps -A | grep media.c2'                    # 两个进程稳定（无重启循环）
   adb shell 'service list | grep -i componentstore'     # default / default9 / software 三实例
   adb shell '/vendor/bin/hw/android.hardware.media.c2-mediatek --probe'
   ```

   `--probe` 走 framework 同款路径（`AServiceManager_checkService` + `listComponents`），
   实测返回：`default` 15 组件（`c2.mtk.*`，rank 512）+ `default9` 2 组件
   （`c2.dolby.*`，rank 8）+ `software` 32 组件（apex）——与真机
   `dumpsys media.player` 基准（33 解码 + 20 编码）逐项对应。

   > **`dumpsys media.player` 在云机上不可用**（平台既有问题，非本模块引入）：
   > 宿主 shell 域 `pioneer` 下，凡 dump 输出由**非 system_server** 进程写回
   > pioneer stdio 管道的服务一律 `FAILED_TRANSACTION`——`media.player` /
   > `media.extractor` / `media.audio_flinger` / `SurfaceFlinger` / `netd` /
   > `stats` 全失败，而 system_server 内的 `power` / `window` / `battery` /
   > `input` 与 CDH 自有 HAL 正常。`sepolicy.py` pioneer 段目前只给
   > `system_server` 授了 `pioneer:fifo_file write`；如后续要在云机上直接读
   > `dumpsys media.player`，需把该授权扩展到 `mediaserver` 等媒体域。
4. 可选：`test/mediandk_{decode,encode}_client.c` 走软解路径回归（配合
   `cdh_mediandk_client_start.S` 免 crt 独立编译）。

## 9. 能力上报（对齐真机）：profile/level

**背景**：假登记 store 起初只报组件名，不答 profile/level，导致 framework 能力塌缩 ——
device-checker `dev.test_media` 真机 vs 云机对拍（2026-09-23）实测：

| codec | 真机 widths/heights/bitrate/fps | 修复前云机 |
|---|---|---|
| `c2.mtk.avc.decoder` | 64-2560 / 64-1440 / 1-60000000 / 0-960 | 64-**384** / 64-**384** / 1-**64000** / 0-**92** |
| `c2.mtk.hevc.decoder` | 16-2560 / 16-1440 / 1-60000000 | 16-**528** / 16-**528** / 1-**128000** |
| `c2.mtk.mpeg4.decoder` | 16-1344 / 16-1088 / 12-60 | 16-**176** / 16-**144** / 12-**15** |
| `c2.mtk.h263.decoder` | 128-720 / 96-576 | **176-176 / 144-144** |
| `c2.mtk.hevc.encoder` | 160-2560 / 128-1440 / 1-100000000 / 0-960 | 160-**288** / 128-**224** / 1-**128000** / 0-**27** |
| `c2.mtk.avc.encoder` | 160-2560 / 128-1440 … | **整条从 MediaCodecList 消失** |
| `c2.mtk.heif.encoder` | 160-2560 / 128-1440 | 一致（mime 无 level 分支，两侧同走默认） |

**根因链**（均有实测证据）：

1. 组件确实进了 framework：mediaserver 侧 `Codec2InfoBuilder` 日志
   `I Codec2InfoBuilder: adding type 'video/avc'` 覆盖全部 15+2 组件（含 `c2.mtk.avc.encoder`）。
2. App 侧 Java `MediaCodecList.initCodecList()` 逐条 `getNewCodecInfoAt(i)`；
   能力解析抛异常即 `Log.e("Could not get codec capabilities")` 并**整条丢弃**
   （该 codec 连 `ALL_CODECS` 都不出现）。云机实测异常
   `IllegalArgumentException: lower must be less than or equal to upper`
   （`Range.intersect` ← `MediaCodecInfo$VideoCapabilities.parseFromInfo`）。
3. `VideoCapabilities.applyLevelLimits()` 按 **mime 分支的默认上限**起步
   （AVC：`maxBlocks=99 / maxBlocksPerSecond=1485 / maxBps=64000`，约 AVC Level 1），
   再按组件上报的 **profile/level** 逐条放大。假登记 store 的
   `IConfigurable::querySupportedValues` 一律回 `OMITTED` → 默认值原样保留 → 能力塌缩。
4. 塌缩后 `c2.mtk.avc.encoder` 的 XML `block-count range="100-14400"`（下限 100）
   与 AVC 默认 `maxBlocks=99` 求交 → 下界>上界 → 抛异常 → 整条被丢弃；
   `c2.mtk.avc.decoder` 的 `block-count` 下限为 16（≤99）故只是塌缩、未消失。

**修复（已落地）**：store 按组件真实应答 profile/level。

- `config/codec2.json` 每个组件新增 `profiles` / `levels`（C2Config 枚举值），
  由真机 `dumpsys media.player` 的 `profile/levels`（SDK 值）经 `Codec2Mapper`
  反查得到（`build.py` 生成到 `codec2_targets.inc`，共 13 条组件带该信息）。
- `src/cdh_codec2_store_service.cpp` 的 `Codec2StoreConfigurable::querySupportedValues`
  识别 `C2StreamProfileLevelInfo`（core index `0x808`，`profile@offset 0` / `level@offset 4`，
  param index = `INFO|dir|IS_STREAM_FLAG|0x808`）并回 `FieldSupportedValues.values`；
  其它字段仍 `OMITTED`。`createInterface()` 把该组件的 profile/level 表交给组件级 configurable。
- 典型值：AVC 解码 Baseline/ConstrainedBaseline/Main/High/High10 @ Level 5.1；
  AVC 编码 Baseline/Main/High @ Level 5；HEVC 解码 Main/MainStill/Main10 @ High 5.1；
  HEVC 编码 Main @ High 5；H.263 Baseline @ Level 40…70（framework 会自行补第二条 40）。

> `colorFormats` 无需处理：`addSupportedColorFormats()` 对非 `c2.android.*` 组件本就无条件
> 追加 `Surface + YUV420Flexible/Planar/SemiPlanar/Packed*`；真机多出的几项来自真机
> store 另答的 pixel-format 查询（本模块不回该查询，只少几个次要格式，不影响能力与列表）。

**验收（device-checker `dev.test_media`，真机 vs 云机同脚本）**：

| 指标 | 真机 | 云机（修复后） |
|---|---|---|
| REGULAR_CODECS 条数 | 77 | 77 |
| ALL_CODECS 条数 | 84 | 84 |
| 其中 `c2.*` | 42 | 42 |
| 仅 ALL 有（secure 等） | 7 | 7 |
| 本项目组件能力明细逐字段 diff | — | **0 处差异 / 17 组件** |

清单与能力数值逐项一致（如 `c2.mtk.avc.decoder` 64-2560 / 64-1440 / 60Mbps / 0-960fps，
`c2.mtk.h263.decoder` 128-720 / 96-576，`c2.mtk.avc.encoder` 回归并给出 160-2560 / 128-1440）。

**协议细节备忘**（踩过的坑）：framework 查询用的是 `C2StreamProfileLevelInfo pl(encoder, 0u)`，
落到 AIDL 的 `ParamField.index` 低位是 core index `0x808`，但 `fieldId.offset` 以**整个 param 对象**
为基准 —— `sizeof=16 = 8(C2Param 头) + 4(profile) + 4(level)`，故 **profile 偏移 8、level 偏移 12**
（不是 0/4，第一版按 0/4 匹配导致全部漏答）。store 的 `querySupportedValues` 会对每次查询打一行
INFO 日志（tag `cdh-codec2`，形如 `name: qv idx=0x... off=.. size=.. type=.. -> answer|omit`），
便于后续核对该契约。
