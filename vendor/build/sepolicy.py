"""note13-5G-15 vendor sepolicy：向 /etc/selinux/vendor_sepolicy.cil 追加 pioneer 最小声明。

pioneer 是 guest 值守域（daemon 及其自研 adbd 的 shell/sync 子进程以
seclabel u:r:pioneer:s0 运行，user/group root 且不降权）。该域需要完全 root 语义：
凡是它作为主体的判定都要放行，逐条 allow 枚举既写不全也易碎（vendor_sepolicy.cil
追加段还存在互相覆盖丢失的历史事故，见 doc/evidence/issue-pioneer-selinux-overwrite/）。

本盘 init 内嵌 secilc 不支持 CIL `permissive` 语句（Unknown keyword → init fatal，
见下），策略侧无法自声明 permissive，故 permissive 改由**内核**承担：开关
CONFIG_CDH_PIONEER_PERMISSIVE 在 security_load_policy() 解析完策略、发布前把
pioneer 写进 policydb->permissive_map，等价于策略文件里的 "permissive pioneer"，
且免疫策略追加段覆盖。
  * 内核补丁：kernel/source/security/selinux/ss/services.c (cdh_mark_pioneer_permissive)
  * 内核配置：kernel/build/lineage-kernel.config

因此本段只需保留 permissive **覆盖不到** 的两类规则，加上结构性类型声明：
  1. 启动链：源域是 init（换域 execve 的前置 process transition / file execute），
     permissive 只看源上下文 type，init 不是 pioneer，覆盖不到；
  2. 反向授权：源域是对端域、目标才是 pioneer（netd fwmark / system_server 管道 /
     媒体域 binder 回程），这些判定同样不因 pioneer 是 permissive 而放行；
  3. 类型/属性声明本身：init 的 `seclabel u:r:pioneer:s0` 若解析不到该类型会直接
     Invalid argument（上述事故），故 (type pioneer)/(type pioneer_exec) 及其
     domain/exec_type/file_type/vendor_file_type 归属必须始终存在。

两条 vendor cil 约束（均实测自基座 vendor_sepolicy.cil）：
  * plat 类型在 vendor cil 中必须以「版本化影子名」引用（如 init_202404 / shell_exec_202404），
    版本号运行时读 /etc/selinux/plat_sepolicy_vers.txt，不得硬编码。vendor 自有类型
    （如 mediacodec）未版本化，直接引用原名；
  * 属性（domain/exec_type/file_type/vendor_file_type）以原名关联，追加单成员
    typeattributeset 与基座的大集合语句按 union 语义合并。

entrypoint 永远只落 exec_type（plat 约束 neverallow domain ~exec_type:file entrypoint），
故 pioneer_exec / shell_exec_{vers} 仅授 entrypoint。

validate_cil 在构建期对每条 allow 的源/目标类型名做存在性校验（plat cil +
plat_pub_versioned.cil + vendor 基座 cil 三份声明全集，精确名匹配），片段内自声明的
类型（pioneer/pioneer_exec）跳过；引用错名不再等到开机 secilc 失败才发现。
"""

import os
import re
import sys

from buildenv import LINEAGE_SUPER_DIR, ROM_ROOT_DIR, TMP_DIR
from debugfs import cat
import gold_props

SELINUX_DIR = "/etc/selinux"
VERS_FILE = SELINUX_DIR + "/plat_sepolicy_vers.txt"
POLICY_FILE = SELINUX_DIR + "/vendor_sepolicy.cil"
PUB_VER_FILE = SELINUX_DIR + "/plat_pub_versioned.cil"
# 基座内该文件的原始标签（rm+write 新 inode 后须回填，debugfs write 不带 xattr）
POLICY_LABEL = "u:object_r:vendor_configs_file:s0"
MARKER = ";; cdh note13-5G-15: pioneer"

# ---- audio AIDL HAL（自研）服务注册附加 ----
AUDIO_MARKER = ";; cdh note13-5G-15: audio-hal"
# v2 增量段标记：ashmem 授权（FMQ）。分离标记使既有 vendor_sepolicy.cil
# 无需重写 v1 段即可增量追加，重复构建幂等。
AUDIO_V2_MARKER = ";; cdh note13-5G-15: audio-hal-v2-ashmem"
# v3 增量段标记：/dev/snd（QEMU intel-hda 声卡）授权。分离标记使既有
# vendor_sepolicy.cil 无需重写 v1/v2 段即可增量追加，重复构建幂等。
AUDIO_V3_MARKER = ";; cdh note13-5G-15: audio-hal-v3-alsa"
VSC = "/etc/selinux/vendor_service_contexts"
VSC_LABEL = "u:object_r:vendor_service_contexts_file:s0"
AUDIO_VSC_LINES = [
    "android.hardware.audio.core.IModule        u:object_r:hal_audio_service:s0",
    "android.hardware.audio.core.IConfig        u:object_r:hal_audio_service:s0",
    "android.hardware.audio.effect.IFactory     u:object_r:hal_audio_service:s0",
]

# ---- keymint HAL（Remote KeyMint）注册附加 ----
KEYMINT_MARKER = ";; cdh note13-5G-15: keymint-hal"
# v2 增量段标记（hvc3 wait stat + kmsg_debug(tmpfs) write）；分离标记使既有
# vendor_sepolicy.cil 无需重写 v1 段即可增量追加，重复构建幂等。
KEYMINT_MARKER2 = ";; cdh note13-5G-15: keymint-hal-v2"
VFC = "/etc/selinux/vendor_file_contexts"
VFC_LABEL = "u:object_r:vendor_configs_file:s0"
# 基座 vendor_file_contexts 以正则 /dev/hvc[0-9]+ -> console_device 覆盖全部
# virtio 串口；Android file_contexts 匹配优先字面量，追加 /dev/hvc3 字面行即可
# 遮蔽正则，不必删基座行（其余 hvc 仍归 console_device）。
# v3 顺延 virtconsole 布局：keymint = hvc3 = Cuttlefish 基线编号（hvc0/1
# 留给固件 console，hvc2 空置预留）。
KEYMINT_FC_LINES = [
    "# cdh note13-5G-15: remote keymint /dev/hvc3",
    "/dev/hvc3  u:object_r:keymaster_device:s0",
]
# FRP 块设备标签：PersistentDataBlockService 按 ro.frp.pst=/dev/block/by-name/frp
# 打开 FRP 分区，本盘官方 GPT 无 frp 分区 → ENOENT → 该服务 init 超时 →
# "Failed to boot service com.android.server.pdb.PersistentDataBlockService" →
# system_server FATAL 反复重启（实例 9 实测 2026-10-10）。
# 解法是在 vda 末尾新增第 12 个 GPT 分区 frp（1MiB 全零，见 pack-roms.VDA_PARTITIONS）：
# init 按 GPT 标签自动建 /dev/block/by-name/frp（androidboot.boot_devices=any），
# 此处只需给该节点打上平台已授权的 frp_block_device 标签——平台策略里
#   (allow system_server frp_block_device (blk_file (ioctl read write getattr lock
#    append map open watch watch_reads)))
# 已就位，无需任何 vendor cil 授权。
# 反例（勿走）：软链到未使用的 misc(vda3) 并补 system_server -> misc_block_device
# 授权。平台有 neverallow base_typeattr_481 misc_block_device:blk_file，其中
# base_typeattr_481 = domain - {hal_bootctl_server fastbootd init recovery ueventd
# uncrypt update_engine vendor_init vendor_misc_writer vold kcmdlinectrl misctrl
# mtectrl} —— system_server 在禁列内，vendor cil 里的 allow 会让 secilc 编译期即拒。
# Android file_contexts 字面量优先于正则，此行可遮蔽平台的 /dev/block/vd[a-z][0-9]*。
FRP_FC_LINES = [
    "# cdh oriole-16: FRP block device (vda12, PersistentDataBlockService)",
    "/dev/block/vda12  u:object_r:frp_block_device:s0",
]
# vendor_property_contexts：放行 init 加载 /vendor/build.prop 时 set
# ro.vendor.boot_security_patch（对齐 pixel6 transforms.go vendorPropertyContextsAdd
# —— 新增键无 exact 规则会匹配默认 vendor 类型，vendor_init 无 set 权限 -> 属性不
# 生效，boot patchlevel 读空）。基座对 ro.vendor.build.security_patch 已有 exact
# 规则（init 序列化时重复 exact 会 Duplicate match -> InitFatalReboot，实测），
# 故只补 boot 一条。plat 已定义 vendor_security_patch_level_prop
# （见 plat_pub_versioned.cil system_property_type）。
VPC = "/etc/selinux/vendor_property_contexts"
VPC_LABEL = "u:object_r:vendor_configs_file:s0"
KEYMINT_VPC_LINES = [
    "# cdh note13-5G-15: remote keymint boot security_patch set",
    "ro.vendor.boot_security_patch u:object_r:vendor_security_patch_level_prop:s0 exact string",
]

# ---- radio AIDL HAL（自研，cdh 频道协议）注册附加 ----
RADIO_MARKER = ";; cdh note13-5G-15: radio-hal-v4"
# v2：MTK radioex AIDL 桩的客户端 find（gold MIUI/MTK RIL 域授权）。
# 独立 marker：v1 段已部署过的 vendor_sepolicy.cil 需增量追加本段。
RADIO_V2_MARKER = ";; cdh note13-5G-15: radio-hal-v2: mtkradioex stub find"
# v3：gsm.version.baseband（vendor/build.prop 注入）需 vendor_init 可 set radio_prop。
# 独立 marker：v1/v2 段已部署过的 vendor_sepolicy.cil 需增量追加本段。
RADIO_V3_MARKER = ";; cdh note13-5G-15: radio-hal-v3: gsm.version.baseband prop"
# v4：dsds 配置属性（persist.radio.multisim.config / ro.telephony.default_network）
# 需 vendor_init 可 set radio_control_prop / telephony_config_prop。
RADIO_V4_MARKER = ";; cdh note13-5G-15: radio-hal-v4: dsds config props"
# 7 个 radio AIDL 接口 + 6 个 MTK radioex 桩前缀 -> hal_radio_service
# （lineage 基座无 AIDL radio 表项，同 audio 机制补齐；service_contexts
# 最长前缀匹配，不带实例名。mtkradioex 前缀一条覆盖 data/messaging/modem/
# network/sim/voice 六个 radioex AIDL 桩——gold MIUI/MTK RIL 的
# isRildReady() 要求它们存在才解锁初始化）。
RADIO_VSC_LINES = [
    "android.hardware.radio.config.IRadioConfig       u:object_r:hal_radio_service:s0",
    "android.hardware.radio.data.IRadioData           u:object_r:hal_radio_service:s0",
    "android.hardware.radio.messaging.IRadioMessaging u:object_r:hal_radio_service:s0",
    "android.hardware.radio.modem.IRadioModem         u:object_r:hal_radio_service:s0",
    "android.hardware.radio.network.IRadioNetwork     u:object_r:hal_radio_service:s0",
    "android.hardware.radio.sim.IRadioSim             u:object_r:hal_radio_service:s0",
    "android.hardware.radio.voice.IRadioVoice         u:object_r:hal_radio_service:s0",
    "vendor.mediatek.hardware.mtkradioex              u:object_r:hal_radio_service:s0",
]

# ---- sensors AIDL HAL（自研虚拟 HAL）注册附加 ----
SENSORS_MARKER = ";; cdh note13-5G-15: sensors-hal"
# lineage 基座 vendor 侧没有 sensors 服务表项 -> 补齐 ISensors/default 的标注
SENSORS_VSC_LINES = [
    "android.hardware.sensors.ISensors            u:object_r:hal_sensors_service:s0",
]
# ---- thermal AIDL HAL（自研虚拟 HAL）注册附加 ----
THERMAL_MARKER = ";; cdh note13-5G-15: thermal-hal"
THERMAL_VSC_LINES = [
    "android.hardware.thermal.IThermal            u:object_r:hal_thermal_service:s0",
]
# ---- vibrator AIDL HAL（自研虚拟 HAL）注册附加 ----
VIBRATOR_MARKER = ";; cdh note13-5G-15: vibrator-hal"
VIBRATOR_VSC_LINES = [
    "android.hardware.vibrator.IVibrator         u:object_r:hal_vibrator_service:s0",
    "android.hardware.vibrator.IVibratorManager  u:object_r:hal_vibrator_service:s0",
]
# ---- lights AIDL HAL（自研虚拟 HAL）注册附加 ----
LIGHTS_MARKER = ";; cdh note13-5G-15: lights-hal"
LIGHTS_VSC_LINES = [
    "android.hardware.light.ILights              u:object_r:hal_light_service:s0",
]
# ---- gnss AIDL HAL（自研虚拟 HAL）注册附加 ----
GNSS_MARKER = ";; cdh note13-5G-15: gnss-hal"
GNSS_VSC_LINES = [
    "android.hardware.gnss.IGnss                 u:object_r:hal_gnss_service:s0",
]
# ---- chipinfo（SoC 身份对齐，对齐真机 gold 的 chipinfo 域）----
CHIPINFO_MARKER = ";; cdh note13-5G-15: chipinfo (soc identity)"
# ---- wifi AIDL HAL（自研虚拟 HAL）注册附加 ----
WIFI_MARKER = ";; cdh note13-5G-15: wifi-hal"
# ---- coredomain 直读 vendor_default_prop（引导修复段，见 coredomain_vendor_prop_read_cil）----
VENDOR_PROP_READ_MARKER = ";; cdh note13-5G-15: coredomain-vendor-prop-read"
# ---- 官方 gs101 应用域 service find（Pixel 专属，见 pixel_gs101_app_cil）----
PIXEL_APP_MARKER = ";; cdh oriole-16: gs101 app-domain service finds"
WIFI_VSC_LINES = [
    "android.hardware.wifi.IWifi                 u:object_r:hal_wifi_service:s0",
]
# 内核模块 mac80211_hwsim 的 module param（扫描档位注入点）独立 sysfs 标签：
# sysfs 节点无法用 file_contexts 打标，只能 genfscon；标签同时挂 file_type
# （console/pioneer 的 file_type 宽授权 => 可写扫描档位）与 sysfs_type
# （plat 已给 hal_wifi -> sysfs_type 只读规则 => HAL 可读档位）。
WIFI_SCAN_PARAM = "cdh_wifi_scan_param"
# v8：档位节点由 mac80211_hwsim 提供（原 cdh_virt_wifi）
WIFI_SCAN_PATH = "/module/mac80211_hwsim/parameters/scan_profile"
# 宿主 wifi 通道 /dev/hvc9 的独立设备标签：基座 vendor_file_contexts 用正则
# /dev/hvc[0-9]+ 统一标 console_device，追加字面行以免把 root console(0/1) 与
# radio(4) 通道一并放开给 wifi HAL。
WIFI_CHANNEL_TYPE = "cdh_wifi_channel"
WIFI_FC_LINES = [
    "# cdh note13-5G-15: wifi host channel /dev/hvc9",
    "/dev/hvc9  u:object_r:cdh_wifi_channel:s0",
]
# v8：数据面 /dev/hvc10 由内核 mac80211_hwsim 直桥独占（filp_open/kernel_read/
# kernel_write），SELinux 主体是内核（u:r:kernel:s0）而非某个 HAL 域；基座正则
# /dev/hvc[0-9]+ -> console_device 不给 kernel 读写，故单独建标签并授权 kernel。
WIFI_DATA_TYPE = "cdh_wifi_data"
WIFI_DATA_FC_LINES = [
    "# cdh note13-5G-15: wifi data plane /dev/hvc10 (kernel hwsim 直桥)",
    "/dev/hvc10  u:object_r:cdh_wifi_data:s0",
]

# ---- media (codec2) AIDL HAL（假登记 store）注册附加 ----
# 基座 lineage virtio vendor 无 media.c2 HAL（VINTF 无声明、vendor_service_contexts
# 无表项），但 plat 已含 hal_codec2 类型属性（挂 mediacodec 域）、hal_codec2_service_202404
# 版本化 service 类型与 ashmem_device 版本化类型。
# 唯一服务源码编译出两个二进制（对齐真机 note13：android.hardware.media.c2-mediatek
# 注册 default，vendor.dolby.media.c2-default-service-dax 注册 default9；basename 判定
# 实例，rc 无参数）以
# seclabel u:r:mediacodec:s0 运行（基座既有 halserverdomain 域，hal_codec2_client/
# server 属性已挂 mediacodec）。组件不可实例化（createComponent 返回 CANNOT_DO，
# 对齐真机 media.c2 服务形态），无 /dev/video*、dma-buf 设备访问需求。
# 故需：
#   * vendor_service_contexts 表项（IComponentStore -> hal_codec2_service），否则
#     AServiceManager_addService 无上下文而失败；
#   * mediacodec -> hal_codec2_service 的 service_manager add（store 服务注册
#     default/default9 两实例），mediametrics/mediacodec 客户端 find；
#   * mediacodec -> ashmem_device（FMQ 共享内存，同 audio v2 机制）。
MEDIA_MARKER = ";; cdh note13-5G-15: media-hal"
# service_contexts 为**精确实例名**匹配（非接口名前缀）：guest 的
# plat_service_contexts 由 lineage/AOSP 提供 default/default1/default2/software 四条，
# 而本模块注册的 default9 无人声明 → AServiceManager_addService 落
# default_android_service 被拒（avc denied add）。故按真机"只补额外实例"写法登记
# default9；default 已由 plat 覆盖，此处重复声明会触发 libselinux
# "Multiple same specifications" 告警，故不再重复。
MEDIA_VSC_LINES = [
    "android.hardware.media.c2.IComponentStore/default9 u:object_r:hal_codec2_service:s0",
]

# ---- camera AIDL HAL（自研虚拟 provider，移植自 pixel6-15）注册附加 ----
CAMERA_MARKER = ";; cdh note13-5G-15: camera-hal"
# lineage virtio 底座无 camera HAL：vendor 侧既无 VINTF 声明也无 service_contexts
# 表项（CameraService 因此报 "No cameras on the device"）。自研 provider 以
# hal_camera_default 域注册 android.hardware.camera.provider.ICameraProvider/internal/0，
# 故需：
#   * service_contexts 表项（接口名 -> hal_camera_service），否则 addService/find
#     落不到 hal_camera_service 类型上；
#   * hal_camera_default -> hal_camera_service 的 service_manager add/find
#     （vendor 侧声明，plat 规则在 lineage 基座里未必齐）；
#   * cameraserver -> hal_camera_service 的 find（CameraService 经 servicemanager
#     枚举 provider）；
#   * ashmem 授权：camera device session 的 request/result metadata 经 FMQ
#     （ashmem 共享内存）传递描述符，HAL 侧 map 需要 ashmem_device 的 open/map 权限。
CAMERA_VSC_LINES = [
    "android.hardware.camera.provider.ICameraProvider u:object_r:hal_camera_service:s0",
]
# 版本化引用机制（实测自本盘三份 cil）:
#   plat 公共类型在 vendor cil 中以「版本化 typeattribute 影子」引用——
#   plat_pub_versioned.cil 声明 (type X) + (typeattribute X_{vers}) +
#   (typeattributeset X_{vers} (X))，allow 里的 X_{vers} 即经属性展开到 X；
#   vendor 自有类型（如 mediacodec）在 vendor cil 里未版本化声明，直接引用原名。
#   引用错（mediacodec_202404 / audioserver 不带版本）都会让 init 内嵌 secilc
#   编译失败 -> 无法开机，故 validate_cil 对全量名字集做存在性校验。

# 平台策略声明源（class/common/classcommon 由内核 classmap 冻结，AOSP 15 各家一致；
# gold system 为 MIUI 私有 EROFS，宿主 dump.erofs 不识别，lineage system 为 raw ext4）
PLAT_POLICY_IMG = os.path.join(LINEAGE_SUPER_DIR, "system_a.img")
PLAT_POLICY_PATH = "/etc/selinux/plat_sepolicy.cil"

# ---- 服务 execv 前置检查权限集（init_{vers} -> pioneer_exec:file）----
_INIT_EXEC_P = "ioctl read open execute getattr map".split()


def _allow(target, ttype, cls, perms):
    """渲染一条 allow；权限 set 化去重排序（输出稳定、天然无重复权限）。"""
    body = " ".join(sorted(set(perms)))
    return f"(allow {target} {ttype} ({cls} ({body})))\n"


def pioneer_cil(vers: str) -> str:
    """生成 pioneer 追加段（vers = plat 策略版本后缀，如 202404）。

    内核已把 pioneer 类型置入 permissive_map（内核开关 CONFIG_CDH_PIONEER_PERMISSIVE），
    凡以 pioneer 为主体的 AVC 判定一律放行，故本段只保留 permissive **覆盖不到** 的
    两类规则：
      1. 启动链：源域是 init（换域 exec 的前置 process transition / file execute）；
      2. 反向授权：源域是对端域、目标才是 pioneer（netd / system_server / 媒体域），
         这些判定不因 pioneer 是 permissive 而放行。
    另有两条 pioneer -> {pioneer_exec,shell_exec}:file entrypoint 属结构性声明，内核侧
    已覆盖，保留以保证「内核开关关掉时仍能起服务」。
    """
    stmts = [
        "(type pioneer)\n",
        "(type pioneer_exec)\n",
        "(typeattributeset domain (pioneer))\n",
        "(typeattributeset exec_type (pioneer_exec))\n",
        "(typeattributeset file_type (pioneer_exec))\n",
        "(typeattributeset vendor_file_type (pioneer_exec))\n",
        # ---- 启动链：源域 init，permissive 覆盖不到 ----
        # init 换域拉起 pioneer 服务（noatsecure/siginh/rlimitinh 消 transition 噪音）
        _allow(f"init_{vers}", "pioneer", "process",
               "transition noatsecure siginh rlimitinh".split()),
        _allow(f"init_{vers}", "pioneer_exec", "file", _INIT_EXEC_P),
        # ---- 结构性 entrypoint（内核侧已覆盖，保留兜底）----
        _allow("pioneer", "pioneer_exec", "file", ["entrypoint"]),
        _allow("pioneer", f"shell_exec_{vers}", "file", ["entrypoint"]),
        # ---- 反向授权：源域为对端域，permissive 覆盖不到 ----
        # fwmarkd（netd 域）经 SCM_RIGHTS 持有 pioneer 的 socket fd 写 fwmark：
        # 缺 use 时 connect 在 fwmark 客户端路径被拒（实测 avc denied { use }
        # scontext=netd tcontext=pioneer tclass=fd，instance26）
        _allow(f"netd_{vers}", "pioneer", "fd", ["use"]),
        # netd InetDiag（destroySockets）要读写 pioneer 的 socket 才能完成销毁，
        # 否则每 0.5s 刷 avc denied（instance27 实测）；udp 同理预留。
        # setopt：fwmarkd（binder 线程）给 connect 路径上 pioneer 的 socket 打
        # SO_MARK，缺它 connect 直接 EACCES（instance32 实测，2117 次 avc 全是它）
        _allow(f"netd_{vers}", "pioneer", "tcp_socket",
               ["getattr", "getopt", "setattr", "setopt", "read", "write"]),
        _allow(f"netd_{vers}", "pioneer", "udp_socket", ["getattr", "getopt", "setattr", "read", "write"]),
        # 投屏媒体链路反向授权（instance30 实测）: binder 回程（reply 携带 binder
        # 对象 / 服务回调）由对端域发起 transfer/call。缺 mediaswcodec_{vers} →
        # pioneer:binder transfer 时 Codec2 AIDL "software" 取 IConfigurable 的
        # transaction 直接 FAILED_TRANSACTION，CHECK abort（且被 vendor dontaudit
        # 吞掉 avc，logcat 无痕迹）。
        # mediacodec 是 vendor 自有类型（未版本化）；mediaswcodec 是 plat 公共类型
        # （版本化影子），swcodec 软编解码进程二选一实跑，两个域都授。
        *(_allow(src, "pioneer", cls, perms)
          for src in ("mediacodec", f"mediaswcodec_{vers}",
                      f"mediaextractor_{vers}", f"mediaserver_{vers}",
                      f"audioserver_{vers}", f"cameraserver_{vers}",
                      f"surfaceflinger_{vers}", f"system_server_{vers}",
                      f"servicemanager_{vers}", f"hwservicemanager_{vers}")
          for cls, perms in (("binder", ["transfer", "call"]),
                             ("fd", ["use"]))),
        # 自研 adbd 的 shell/sync 服务子进程继承 pioneer 域（daemon 是 root 常驻服务，
        # 子进程不降权）。system_server 需把命令输出写回子进程的 stdio 管道
        # （fifo_file）、并读写它的 socketpair（unix_stream_socket），缺任一条时
        # `dumpsys`/`settings`/`pm` 一律 FAILED_TRANSACTION（实例 5 实测 avc：
        # comm="dumpsys" tclass=fifo_file denied { write }；
        # comm="cmd" tclass=unix_stream_socket denied { read write }）。
        _allow(f"system_server_{vers}", "pioneer", "fifo_file",
               ["read", "write", "getattr", "ioctl"]),
        _allow(f"system_server_{vers}", "pioneer", "unix_stream_socket",
               ["read", "write", "getattr", "ioctl", "getopt", "setopt", "shutdown"]),
    ]
    return f"{MARKER}: pioneer minimal declaration (kernel permissive).\n" + "".join(stmts)


_RE_CLASS = re.compile(r"\(class\s+(\S+)\s+\(([^)]*)\)\)")
_RE_COMMON = re.compile(r"\(common\s+(\S+)\s+\(([^)]*)\)\)")
_RE_CLASSCOMMON = re.compile(r"\(classcommon\s+(\S+)\s+(\S+)\)")
_RE_ALLOW = re.compile(r"\(allow\s+(\S+)\s+(\S+)\s+\((\S+)\s+\(([^)]*)\)\)\)")
_RE_TYPE = re.compile(r"\(type\s+(\S+)\)")
_RE_TYPEATTR = re.compile(r"\(typeattribute\s+(\S+)\)")

_decl_cache = None
_name_cache = None


def plat_class_perms() -> dict:
    """解析平台 plat_sepolicy.cil 的 (class)/(common)/(classcommon) 声明，
    返回 {class: 合法权限集}；读不到即报错退出（校验数据不允许静默降级）。"""
    global _decl_cache
    if _decl_cache is not None:
        return _decl_cache
    blob = _read_cil(PLAT_POLICY_IMG, PLAT_POLICY_PATH)
    classes, commons, links = {}, {}, {}
    for raw in blob.splitlines():
        s = raw.strip()
        m = _RE_CLASS.match(s)
        if m:
            classes[m[1]] = set(m[2].split())
            continue
        m = _RE_COMMON.match(s)
        if m:
            commons[m[1]] = set(m[2].split())
            continue
        m = _RE_CLASSCOMMON.match(s)
        if m:
            links[m[1]] = m[2]
    _decl_cache = {c: own | commons.get(links.get(c), set())
                   for c, own in classes.items()}
    return _decl_cache


def _read_cil(img, path) -> str:
    blob = cat(img, path)
    if not blob:
        print(f"[err] 读不到策略声明: {path} @ {img}")
        sys.exit(1)
    return blob.decode("utf-8", "replace")


def _cil_names(text: str) -> set:
    """(type X) / (typeattribute X) 声明名集合。
    正则不误伤: (typetransition/typeattributeset 无 type 后空白，不匹配)。"""
    return {m[1] for m in _RE_TYPE.finditer(text)} | \
           {m[1] for m in _RE_TYPEATTR.finditer(text)}


def plat_name_set() -> set:
    """plat cil 的 type/typeattribute 名全集。版本化影子的 typeattribute 声明
    在 plat_pub_versioned.cil（vendor 侧文件），由调用方经 _cil_names 解析后并入。"""
    global _name_cache
    if _name_cache is None:
        _name_cache = _cil_names(_read_cil(PLAT_POLICY_IMG, PLAT_POLICY_PATH))
    return _name_cache


# 类型名可含连字符（gold MTK 属性类型，如 vendor_mtk_ctl_ril-proxy_prop）：字符集漏掉
# '-' 会把片段内自声明的连字符类型当未知名字误报（实测 2026-09-27）。
_TYPE_DECL_RE = re.compile(r"^\(type\s+([a-z0-9_\- ]+?)\s*\)\s*$", re.M)


def _declared_types(cil: str) -> set:
    """收集 CIL 片段里 (type X) 自声明的类型名。

    这些类型随本片段一起写进 vendor_sepolicy.cil，编译期必定存在，故 validate_cil
    对它们跳过「名字全集」存在性检查（此前用硬编码白名单，新增类型会误报）。
    """
    names = set()
    for group in _TYPE_DECL_RE.findall(cil):
        names.update(group.split())
    return names


def validate_cil(cil: str, name_universe: set) -> None:
    """逐条 allow 校验: class/perm 合法性（按平台声明）、源/目标名存在性
    （name_universe = plat cil + plat_pub_versioned.cil + vendor 基座 cil 的
    声明全集，精确名匹配）与冗余（完全重复语句、同一 (src,tgt,class) 拆分授权）。
    违规即报错退出。"""
    valid = plat_class_perms()
    names = plat_name_set() | set(name_universe)
    # 本段自有类型/标签（片段内 (type X) 声明，如 pioneer/cdh_wifi_channel）
    # + CIL 关键字目标，不做存在性检查
    skip = {"self"} | _declared_types(cil)
    seen_stmt, seen_pair, errs, n = set(), set(), [], 0
    for ln in cil.splitlines():
        s = ln.strip()
        if not s.startswith("(allow "):
            continue
        n += 1
        m = _RE_ALLOW.fullmatch(s)
        if not m:
            errs.append(f"语句格式不可解析: {s}")
            continue
        src, tgt, cls, perms = m[1], m[2], m[3], m[4].split()
        if s in seen_stmt:
            errs.append(f"重复语句: {s}")
            continue
        seen_stmt.add(s)
        if cls not in valid:
            errs.append(f"未知 class {cls}: {s}")
            continue
        bad = sorted(set(perms) - valid[cls])
        if bad:
            errs.append(f"非法权限 {cls}:{bad} -> {tgt}")
        for name in (src, tgt):
            if name in skip or name in names:
                continue
            errs.append(f"未知 type/attribute {name}: {s}")
        key = (src, tgt, cls)
        if key in seen_pair:
            errs.append(f"拆分冗余授权 ({src} {tgt} {cls}): {s}")
        seen_pair.add(key)
    if errs:
        for e in errs:
            print(f"[err] sepolicy 校验: {e}")
        sys.exit(1)
    print(f"[ok] pioneer CIL 校验通过（{n} 条 allow，平台 {len(valid)} class、"
          f"{len(names)} type/attr 声明）")


def audio_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """追加 audio AIDL HAL 注册所需的 CIL allow + vendor_service_contexts 映射。

    背景：自研 android.hardware.audio 服务（hal_audio_default 域）向 servicemanager
    注册 IModule/IConfig 时被拒（avc denied {add} tcontext=default_android_service）：
      * vendor_sepolicy.cil 无 hal_audio_default -> hal_audio_service* 的 add 授权；
      * vendor_service_contexts 无 android.hardware.audio.core.* 映射（lineage 基座
        音频 HAL 在 APEX 内，selinux 表没有相应行）。
    本函数追加：
      (allow hal_audio_default hal_audio_service_{vers} (service_manager (add find)))
    并在 vendor_service_contexts 追加三条 接口->hal_audio_service 映射。
    幂等：重复构建同一 work.img 时按标记/内容跳过。
    """
    vers = cat(work_img, VERS_FILE)
    if not vers:
        print(f"[err] 读不到 {VERS_FILE}")
        sys.exit(1)
    vers = vers.decode("ascii", "ignore").strip()
    tmp_dir = tmp_dir or os.path.join(TMP_DIR, "vendor")
    os.makedirs(tmp_dir, exist_ok=True)

    cmds = []
    # 1) CIL allow 段（v1/v2 各自按标记幂等增量追加）
    orig = cat(work_img, POLICY_FILE)
    if orig is None:
        print(f"[err] 读不到 {POLICY_FILE}")
        sys.exit(1)
    merged = orig.decode("utf-8", "replace")
    need_v1 = AUDIO_MARKER not in merged
    need_v2 = AUDIO_V2_MARKER not in merged
    need_v3 = AUDIO_V3_MARKER not in merged
    if need_v1 or need_v2 or need_v3:
        # 注意：本函数与 patch_commands 都从同一原始 vendor_sepolicy.cil 追加并
        # rm+write 回写同一文件，命令按顺序执行、后者覆盖前者。patch_commands 的
        # pioneer 段会被本段覆盖丢失（实测运行时 pioneer 域"not valid left
        # unmapped"）。这里在追加 audio 段的同时若 pioneer 段缺失则一并带上。
        cil = ""
        if need_v1:
            if MARKER not in merged:
                cil += pioneer_cil(vers)
            cil += audio_hal_cil(vers)
        if need_v2:
            cil += audio_hal_cil_v2(vers)
        if need_v3:
            cil += audio_hal_cil_v3(vers)
        name_universe = _cil_names(merged) | \
            _cil_names(_read_cil(work_img, PUB_VER_FILE))
        validate_cil(cil, name_universe)
        tmp = os.path.join(tmp_dir, "vendor_sepolicy_audio.cil")
        with open(tmp, "wb") as f:
            f.write(orig + cil.encode("utf-8"))
        print(f"[ok] vendor_sepolicy.cil 追加 audio HAL 段（plat vers {vers}，"
              f"+{len(cil)} B，v1={need_v1} v2={need_v2} v3={need_v3}）")
        cmds += [
            f"rm {POLICY_FILE}",
            f"write {tmp} {POLICY_FILE}",
            f"ea_set {POLICY_FILE} security.selinux {POLICY_LABEL}",
        ]
    else:
        print("[ok] vendor_sepolicy.cil 已含 audio HAL 段，跳过")

    # 2) vendor_service_contexts 映射
    vsc = cat(work_img, VSC)
    if vsc is None:
        print(f"[err] 读不到 {VSC}")
        sys.exit(1)
    if b"android.hardware.audio.core.IConfig" in vsc:
        print("[ok] vendor_service_contexts 已含 audio 映射，跳过")
    else:
        append_blob = ("\n# cdh note13-5G-15: audio AIDL HAL services\n"
                       + "\n".join(AUDIO_VSC_LINES) + "\n").encode("utf-8")
        tmp = os.path.join(tmp_dir, "vendor_service_contexts")
        with open(tmp, "wb") as f:
            f.write(vsc + append_blob)
        print(f"[ok] vendor_service_contexts 追加 {len(AUDIO_VSC_LINES)} 条映射")
        cmds += [
            f"rm {VSC}",
            f"write {tmp} {VSC}",
            f"ea_set {VSC} security.selinux {VSC_LABEL}",
        ]
    return cmds


def keymint_hal_cil(vers: str) -> str:
    """Remote KeyMint 所需 CIL（v1 段）：/dev/hvc3 设备类型 + hal_keymint_default 授权。

    hal_keymint_default 域/init 换域/hal_keymint_service add-find 全部由基座
    （vendor cil + plat cil + plat_service_contexts）提供，本段只补三件事：
      1) keymaster_device 设备类型（/dev/hvc3 的 file_contexts 标签目标）；
      2) hal_keymint_default 对 hvc3 chardev 的 getattr/ioctl/open/read/write
         （posix_channel over /dev/hvc3 与 host secure_env 收发 keymaster 消息）；
      3) serialno 属性读（attestation 取 serial number）与 kmsg 写（rc 已插
         `file /dev/kmsg w`，策略侧补 chr_file write）。
    serialno_prop / kmsg_device 为 plat 公共类型，vendor cil 中须引用版本化
    影子 serialno_prop_{vers} / kmsg_device_{vers}（同 pioneer 段机制）。

    注意：plat_pub_versioned.cil 只声明了 typeattribute kmsg_device_{vers} 而无
    typeattributeset 填充（空属性），直接 (allow ... kmsg_device_{vers} ...) 授权
    落空（运行时 /dev/kmsg 标签是 kmsg_device）。因此本段必须补一条
    typeattributeset 把真实类型 kmsg_device 填入版本化属性，allow 才生效。
    """
    return (
        f"{KEYMINT_MARKER}: remote keymint hvc3 chardev + serialno/kmsg\n"
        "(type keymaster_device)\n"
        "(roletype object_r keymaster_device)\n"
        "(typeattributeset dev_type (keymaster_device))\n"
        + _allow("hal_keymint_default", "keymaster_device", "chr_file",
                 "getattr ioctl open read write".split())
        + _allow("hal_keymint_default", f"serialno_prop_{vers}", "file",
                 "read getattr map open".split())
        # 版本化属性 kmsg_device_{vers} 在 plat_pub_versioned.cil 中为空（无
        # typeattributeset 填充），把真实类型 kmsg_device 填入，授权才有效
        + f"(typeattributeset kmsg_device_{vers} (kmsg_device))\n"
        + _allow("hal_keymint_default", f"kmsg_device_{vers}", "chr_file",
                 ["write"])
    )


def keymint_hal_cil_v2(vers: str) -> str:
    """Remote KeyMint 增量段（v2）：实例22 实测补两条。

    1) `wait /dev/hvc3 10`（init.virt.rc wait_for_file → stat）连续
       812 次 avc denied getattr（vendor_init -> keymaster_device chr_file）
       -> 10s 超时 fail。补 vendor_init getattr，消除拖慢与噪音。
    2) keymint LOG 写 /dev/kmsg_debug（目标标签 tmpfs，非 kmsg_device）被拒
       -> keymint 的 LOG(FATAL)/CHECK 取证日志全丢。kmsg_debug 实际 label 是
       tmpfs；plat_pub_versioned 的 tmpfs 版本化属性可能为空，同 kmsg 机制先
       填充真实类型再授权。
    """
    return (
        f"{KEYMINT_MARKER2}: hvc3 wait stat + kmsg_debug(tmpfs) write\n"
        + _allow(f"vendor_init_{vers}", "keymaster_device", "chr_file",
                 ["getattr"])
        + f"(typeattributeset tmpfs_{vers} (tmpfs))\n"
        + _allow("hal_keymint_default", f"tmpfs_{vers}", "chr_file",
                 ["write"])
    )


def audio_hal_cil(vers: str) -> str:
    """audio AIDL HAL 注册所需 CIL 段（供 audio/keymint 两处 patch 复用）。"""
    return (
        f"{AUDIO_MARKER}: allow audio HAL service add/find\n"
        + _allow(f"hal_audio_default", f"hal_audio_service_{vers}",
                 "service_manager", "add find".split())
        # 主 servicemanager 启动时一次性加载并缓存 contexts 表（/mnt 未挂载
        # 时读到默认表），vendor_service_contexts 的映射运行时未必生效。兜底对
        # 未映射的 AIDL 服务名（default_android_service）直接授 add，与 pioneer
        # 段同机制；映射文件仍保留（对已生效场景更收敛）。
        + _allow(f"hal_audio_default", f"default_android_service_{vers}",
                 "service_manager", ["add"])
        # audioserver getService(IModule/primary 等)：fwk 侧查"未映射"实例时
        # 目标也是 default_android_service，缺 find 则 getService 返回 null，
        # audioserver 拿不到 HAL（实测 avc denied {find}）。同时授 hal_audio_service
        # 的 find，覆盖 contexts 映射生效的收敛场景。
        + _allow(f"audioserver_{vers}", f"default_android_service_{vers}",
                 "service_manager", ["find"])
        + _allow(f"audioserver_{vers}", f"hal_audio_service_{vers}",
                 "service_manager", ["find"])
    )


def audio_hal_cil_v2(vers: str) -> str:
    """audio AIDL HAL FMQ 支撑增量段（v2）：/dev/ashmem 授权。

    实例59 实测 avc denied {open} hal_audio_default ashmem_device chr_file：
    lineage 基座的音频 HAL 在 APEX 内（selinux 表无该授权），自研服务用
    AidlMessageQueue（FMQ）分配共享内存必须 open /dev/ashmem；被拒 ->
    ashmem fd -1 -> mmap EBADF 9 -> StreamDescriptor ParcelFileDescriptor=-1
    -> openOutput/openInputStream 在 framework 侧失败。
    """
    return (
        f"{AUDIO_V2_MARKER}: allow audio HAL FMQ /dev/ashmem\n"
        + _allow(f"hal_audio_default", f"ashmem_device_{vers}", "chr_file",
                 "ioctl read write getattr map open".split())
    )


def audio_hal_cil_v3(vers: str) -> str:
    """audio AIDL HAL 输出增量段（v3）：/dev/snd（QEMU intel-hda 声卡）授权。

    VdacStreamOut 通过原生 ALSA ioctl/write 直连 /dev/snd/pcmC0D0p（QEMU
    intel-hda 播放节点），把 AudioFlinger 的 burst 背压锚定到真实声卡时钟
    （修复音频时钟偏快 1.25x -> 在线播放 2 倍速快进）。hal_audio_default
    域需对 audio_device（/dev/snd/*）chr_file 具备 open/ioctl/read/write/
    getattr/mmap：open 打开节点、ioctl 配置 hw/sw params 与 drop/drain、
    write 写 PCM 数据。plat 公共类型 audio_device 以版本化影子
    audio_device_{vers} 引用（同 ashmem_device_{vers} 机制）。
    """
    return (
        f"{AUDIO_V3_MARKER}: allow audio HAL /dev/snd (QEMU intel-hda)\n"
        + _allow(f"hal_audio_default", f"audio_device_{vers}", "chr_file",
                 "ioctl read write getattr map open".split())
    )


def keymint_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """追加 Remote KeyMint 所需 CIL 段 + /dev/hvc3 文件上下文，返回 debugfs 命令。

    与 audio_hal_patch_commands 同机制：读到的仍是基座原文件，而 debugfs 命令按
    顺序回写（后者覆盖前者），故若前段（pioneer/audio）标记缺失需一并带上，保证
    三段都落在最终 vendor_sepolicy.cil。幂等：标记已存在则跳过。
    """
    vers = cat(work_img, VERS_FILE)
    if not vers:
        print(f"[err] 读不到 {VERS_FILE}")
        sys.exit(1)
    vers = vers.decode("ascii", "ignore").strip()
    tmp_dir = tmp_dir or os.path.join(TMP_DIR, "vendor")
    os.makedirs(tmp_dir, exist_ok=True)

    cmds = []
    # 1) CIL 段（pioneer/audio/keymint 三层追加）
    orig = cat(work_img, POLICY_FILE)
    if orig is None:
        print(f"[err] 读不到 {POLICY_FILE}")
        sys.exit(1)
    merged = orig.decode("utf-8", "replace")
    if KEYMINT_MARKER2 not in merged:
        # 幂等：先带全套（pioneer/audio/keymint v1，各自 marker 缺失时补），
        # v2 增量段始终追加（游离 marker 未出现在 cil 中）。
        cil = ""
        if MARKER not in merged:
            cil += pioneer_cil(vers)
        if AUDIO_MARKER not in merged:
            cil += audio_hal_cil(vers)
        if AUDIO_V2_MARKER not in merged:
            cil += audio_hal_cil_v2(vers)
        if KEYMINT_MARKER not in merged:
            cil += keymint_hal_cil(vers)
        cil += keymint_hal_cil_v2(vers)
        name_universe = _cil_names(merged) | \
            _cil_names(_read_cil(work_img, PUB_VER_FILE))
        validate_cil(cil, name_universe)
        tmp = os.path.join(tmp_dir, "vendor_sepolicy_keymint.cil")
        with open(tmp, "wb") as f:
            f.write(orig + cil.encode("utf-8"))
        print(f"[ok] vendor_sepolicy.cil 追加 keymint HAL 段（plat vers {vers}，"
              f"+{len(cil)} B）")
        cmds += [
            f"rm {POLICY_FILE}",
            f"write {tmp} {POLICY_FILE}",
            f"ea_set {POLICY_FILE} security.selinux {POLICY_LABEL}",
        ]
    else:
        print("[ok] vendor_sepolicy.cil 已含 keymint HAL v2 段，跳过")

    # 2) vendor_file_contexts：/dev/hvc3 -> keymaster_device
    vfc = cat(work_img, VFC)
    if vfc is None:
        print(f"[err] 读不到 {VFC}")
        sys.exit(1)
    if b"u:object_r:keymaster_device:s0" in vfc:
        print("[ok] vendor_file_contexts 已含 keymaster_device 标签，跳过")
    else:
        append_blob = ("\n" + "\n".join(KEYMINT_FC_LINES) + "\n").encode("utf-8")
        tmp = os.path.join(tmp_dir, "vendor_file_contexts")
        with open(tmp, "wb") as f:
            f.write(vfc + append_blob)
        print("[ok] vendor_file_contexts 追加 /dev/hvc3 -> keymaster_device")
        cmds += [
            f"rm {VFC}",
            f"write {tmp} {VFC}",
            f"ea_set {VFC} security.selinux {VFC_LABEL}",
        ]

    # 3) vendor_property_contexts：security_patch 属性 set 放行（见常量注释）
    vpc = cat(work_img, VPC)
    if vpc is None:
        print(f"[err] 读不到 {VPC}")
        sys.exit(1)
    if b"ro.vendor.boot_security_patch" in vpc:
        print("[ok] vendor_property_contexts 已含 security_patch 规则，跳过")
    else:
        append_blob = ("\n" + "\n".join(KEYMINT_VPC_LINES) + "\n").encode("utf-8")
        tmp = os.path.join(tmp_dir, "vendor_property_contexts")
        with open(tmp, "wb") as f:
            f.write(vpc + append_blob)
        print(f"[ok] vendor_property_contexts 追加 {len(KEYMINT_VPC_LINES) - 1} 条 exact 规则")
        cmds += [
            f"rm {VPC}",
            f"write {tmp} {VPC}",
            f"ea_set {VPC} security.selinux {VPC_LABEL}",
        ]
    return cmds


def radio_hal_cil(vers: str) -> str:
    """radio AIDL HAL 注册所需 CIL 段。

    复用基座 hal_radio_default 域（init 换域/hal_radio_service 属性均由基座提供），
    只补四件事：
      1) service_manager add/find：hal_radio_default 注册 7 个 android.hardware.radio.*
         服务（vendor_service_contexts 映射到 hal_radio_service），并兜底未映射名
         （default_android_service，同 audio 机制）；
      2) 框架侧 find：hal_telephony_client（plat 属性，含 phone app/system_server 等
         telephony 客户端）getService 查询；
      3) /dev/hvc4：基座 vendor_file_contexts 以正则 /dev/hvc[0-9]+ -> console_device
         覆盖全部 virtio 串口（ueventd /dev/hvc* 0666 system system，radio 用户可开），
         授权 hal_radio_default 读写该 chardev（与 host radio provider 收发帧）；
      4) 数据呼叫（ccmni2，net2 网卡改名）需 net_admin + netlink 路由套接字（对齐 pixel6）。
    """
    return (
        f"{RADIO_MARKER}: allow radio HAL service add/find + hvc4 + netlink\n"
        + _allow("hal_radio_default", f"hal_radio_service_{vers}",
                 "service_manager", "add find".split())
        # 主 servicemanager 启动时一次性缓存 contexts 表，未映射的 AIDL 服务名兜底
        + _allow("hal_radio_default", f"default_android_service_{vers}",
                 "service_manager", ["add"])
        + _allow("hal_telephony_client", f"default_android_service_{vers}",
                 "service_manager", ["find"])
        + _allow("hal_telephony_client", f"hal_radio_service_{vers}",
                 "service_manager", ["find"])
        + _allow("hal_radio_default", f"console_device_{vers}", "chr_file",
                 "getattr ioctl open read write".split())
        + _allow("hal_radio_default", "self", "capability", ["net_admin"])
        + _allow("hal_radio_default", "self", "netlink_route_socket",
                 "bind create getattr getopt nlmsg_read nlmsg_readpriv"
                 " nlmsg_write read setopt write".split())
        # MIUI/MTK RIL 早期版本还会对 HIDL vendor.mediatek.hardware.mtkradioex
        # 做 hwservice find（后台重试，非阻塞）；放开 find 避免拒绝噪音，
        # HIDL radioex 不实现（RIL 对 radioex 的 AIDL 桩调用有 RemoteException
        # 容错，基础 SIM/网络走标准 android.hardware.radio.* 接口）。
        + _allow(f"radio_{vers}", f"default_android_hwservice_{vers}",
                 "hwservice_manager", ["find"])
    )


def radio_hal_cil_v2(vers: str) -> str:
    """radio HAL v2 段：MTK radioex AIDL 桩的客户端授权。

    gold MIUI/MTK RIL（com.android.phone，radio 域）isRildReady() 对
    vendor.mediatek.hardware.mtkradioex.* AIDL 服务做 checkService；这些名字
    挂到 hal_radio_service 名前缀（vendor_service_contexts），显式授权
    radio 域 find 可达（hal_telephony_client 属性未覆盖 radio 域时兜底）。
    """
    return (
        f"{RADIO_V2_MARKER}\n"
        + _allow(f"radio_{vers}", f"hal_radio_service_{vers}",
                 "service_manager", ["find"])
        + _allow(f"radio_{vers}", f"default_android_service_{vers}",
                 "service_manager", ["find"]) +
        "\n"
    )


def radio_hal_cil_v3(vers: str) -> str:
    """radio HAL v3 段：gsm.version.baseband（vendor/build.prop）授权。

    实例 74 实测：baselines build-prop-sets 注入
    `gsm.version.baseband=MOLY...,MOLY...` 到 vendor/build.prop 后，init 加载时
    `init: Do not have permissions to set 'gsm.version.baseband' ... SELinux
    permission denied`——plat_property_contexts 前缀规则 `gsm.` 将其标注为
    radio_prop，而 vendor_init 无 radio_prop 的 property_service set 权限，
    该行被丢弃 -> getprop 读不到、基带版本回退到内置常量。
    radio_prop ∈ system_public_property_type（vendor_init 可 set，不触 neverallow），
    故补一条 allow 即可让 build.prop 生效；同时授自研 radio HAL
    （hal_radio_default，直接类型无版本化影子）read，供 getBasebandVersion
    在 host 快照未注入时回退读取该属性。
    """
    return (
        f"{RADIO_V3_MARKER}\n"
        + _allow(f"vendor_init_{vers}", f"radio_prop_{vers}",
                 "property_service", ["set"])
        + _allow("hal_radio_default", f"radio_prop_{vers}", "file",
                 ["read", "getattr", "map", "open"])
    )


def radio_hal_cil_v4(vers: str) -> str:
    """radio HAL v4 段：dsds（双卡双待）配置属性授权。

    gold 真机是 dsds：框架在开机早期用 `persist.radio.multisim.config` 决定建几条
    Phone（TelephonyManager.getPhoneCount 读该属性），RadioConfig.getPhoneCapability
    只在其后校验/更新。故该属性必须在 build.prop 里预置为 dsds，否则框架只建 1 条
    Phone（实测：只连 IRadioModem/slot1，从不查 slot2、Active modem count=1）。
    `persist.radio.multisim.config` 与 `ro.telephony.default_network` 分别是
    plat 类型 radio_control_prop / telephony_config_prop，vendor_init 需显式 set
    授权才能在加载 /vendor/build.prop 时写入（同 v3 的 radio_prop 情形）。
    """
    return (
        f"{RADIO_V4_MARKER}\n"
        + _allow(f"vendor_init_{vers}", f"radio_control_prop_{vers}",
                 "property_service", ["set"])
        + _allow(f"vendor_init_{vers}", f"telephony_config_prop_{vers}",
                 "property_service", ["set"])
    )


def radio_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """追加 radio AIDL HAL 注册所需 CIL 段 + vendor_service_contexts 映射。
    与 keymint_hal_patch_commands 同机制：debugfs 命令按顺序回写（后者覆盖前者），
    本段执行时若前段（pioneer/audio/keymint）标记缺失需一并带上，保证四段都落在
    最终 vendor_sepolicy.cil。幂等：标记已存在则跳过；v1 已部署时增量追加 v2。
    """
    vers = cat(work_img, VERS_FILE)
    if not vers:
        print(f"[err] 读不到 {VERS_FILE}")
        sys.exit(1)
    vers = vers.decode("ascii", "ignore").strip()
    tmp_dir = tmp_dir or os.path.join(TMP_DIR, "vendor")
    os.makedirs(tmp_dir, exist_ok=True)

    cmds = []
    # 1) CIL 段（pioneer/audio/keymint/radio 四层追加）
    orig = cat(work_img, POLICY_FILE)
    if orig is None:
        print(f"[err] 读不到 {POLICY_FILE}")
        sys.exit(1)
    merged = orig.decode("utf-8", "replace")
    cil = ""
    if RADIO_MARKER not in merged:
        if MARKER not in merged:
            cil += pioneer_cil(vers)
        if AUDIO_MARKER not in merged:
            cil += audio_hal_cil(vers)
        if AUDIO_V2_MARKER not in merged:
            cil += audio_hal_cil_v2(vers)
        if KEYMINT_MARKER not in merged:
            cil += keymint_hal_cil(vers)
        if KEYMINT_MARKER2 not in merged:
            cil += keymint_hal_cil_v2(vers)
        # 干净基座：v1（注册/hvc4/netlink）+ v2（radioex 桩客户端 find）
        # + v3（gsm.version.baseband 属性授权）+ v4（dsds 配置属性授权）一次性追加
        cil += (radio_hal_cil(vers) + radio_hal_cil_v2(vers) +
                radio_hal_cil_v3(vers) + radio_hal_cil_v4(vers))
    elif RADIO_V2_MARKER not in merged:
        # v1 已部署（RadioEx 桩方案之前），增量追加 v2 + v3 + v4
        cil += radio_hal_cil_v2(vers) + radio_hal_cil_v3(vers) + \
            radio_hal_cil_v4(vers)
    elif RADIO_V3_MARKER not in merged:
        # v1/v2 已部署，增量追加 v3（gsm.version.baseband 属性授权）+ v4（dsds 属性）
        cil += radio_hal_cil_v3(vers) + radio_hal_cil_v4(vers)
    elif RADIO_V4_MARKER not in merged:
        # v1/v2/v3 已部署，增量追加 v4（dsds 配置属性授权）
        cil += radio_hal_cil_v4(vers)
    if cil:
        name_universe = _cil_names(merged) | \
            _cil_names(_read_cil(work_img, PUB_VER_FILE))
        validate_cil(cil, name_universe)
        tmp = os.path.join(tmp_dir, "vendor_sepolicy_radio.cil")
        with open(tmp, "wb") as f:
            f.write(orig + cil.encode("utf-8"))
        print(f"[ok] vendor_sepolicy.cil 追加 radio HAL 段（plat vers {vers}，"
              f"+{len(cil)} B）")
        cmds += [
            f"rm {POLICY_FILE}",
            f"write {tmp} {POLICY_FILE}",
            f"ea_set {POLICY_FILE} security.selinux {POLICY_LABEL}",
        ]
    else:
        print("[ok] vendor_sepolicy.cil 已含 radio HAL 段（v1+v2），跳过")

    # 2) vendor_service_contexts：radio 接口 + mtkradioex 前缀 -> hal_radio_service。
    #    增量：只追加缺失行（已有 7 行标准接口时仍补 mtkradioex 前缀）。
    vsc = cat(work_img, VSC)
    if vsc is None:
        print(f"[err] 读不到 {VSC}")
        sys.exit(1)
    missing = [ln for ln in RADIO_VSC_LINES
               if ln.encode("utf-8") not in vsc]
    if not missing:
        print("[ok] vendor_service_contexts 已含 radio 映射，跳过")
    else:
        append_blob = ("\n# cdh note13-5G-15: radio AIDL HAL services\n"
                       + "\n".join(missing) + "\n").encode("utf-8")
        tmp = os.path.join(tmp_dir, "vendor_service_contexts")
        with open(tmp, "wb") as f:
            f.write(vsc + append_blob)
        print(f"[ok] vendor_service_contexts 追加 {len(missing)} 条映射")
        cmds += [
            f"rm {VSC}",
            f"write {tmp} {VSC}",
            f"ea_set {VSC} security.selinux {VSC_LABEL}",
        ]
    return cmds


def sensors_hal_cil(vers: str) -> str:
    """sensors HAL v1 段：hal_sensors_default 注册 ISensors/default + FMQ（ashmem）。

    参照 radio v1/v4 的做法：plat 公共类型用版本化影子（hal_sensors_service_{vers} /
    ashmem_device_{vers} / system_server_{vers}），HAL 域 hal_sensors_default 与
    vendor 自有 exec 类型 hal_sensors_default_exec 直接引用。
    ISensors.initialize 传入事件队列（FMQ）描述符，HAL 侧要 map 该共享内存，
    故需 ashmem_device 的文件权限（同 audio HAL v2 段）。
    """
    return (
        f"{SENSORS_MARKER}\n"
        + _allow("hal_sensors_default", f"hal_sensors_service_{vers}",
                 "service_manager", ["add", "find"])
        + _allow(f"system_server_{vers}", f"hal_sensors_service_{vers}",
                 "service_manager", ["find"])
        + _allow("hal_sensors_default", f"ashmem_device_{vers}", "chr_file",
                 ["ioctl", "read", "write", "getattr", "map", "open"])
    )


def camera_hal_cil(vers: str) -> str:
    """camera HAL v1 段：hal_camera_default 注册 ICameraProvider/internal/0。

    参照 radio/sensors 的做法：plat 公共类型用版本化影子（hal_camera_service_{vers} /
    ashmem_device_{vers} / system_server_{vers}），HAL 域 hal_camera_default 与
    vendor 自有 exec 类型 hal_camera_default_exec 直接引用（ancestor 规则由
    lineage 基座提供）。
    CameraService（cameraserver 进程）经 servicemanager 枚举 provider，故
    cameraserver 要有 find hal_camera_service 的授权；provider 进程侧 meterdata
    FMQ 创建 ashmem 共享内存（同 sensors 事件队列机制），需 ashmem_device 文件权限。
    """
    return (
        f"{CAMERA_MARKER}\n"
        + _allow("hal_camera_default", f"hal_camera_service_{vers}",
                 "service_manager", ["add", "find"])
        + _allow(f"cameraserver_{vers}", f"hal_camera_service_{vers}",
                 "service_manager", ["find"])
        + _allow("hal_camera_default", f"ashmem_device_{vers}", "chr_file",
                 ["ioctl", "read", "write", "getattr", "map", "open"])
    )


def thermal_hal_cil(vers: str) -> str:
    """thermal HAL v1 段：hal_thermal_default 注册 IThermal/default。

    lineage 基座无 thermal HAL（vendor 侧既无 VINTF 声明也无 service_contexts 表项），
    ThermalManagerService 报 "HAL Ready: false"。自研服务以 hal_thermal_default 域注册
    android.hardware.thermal.IThermal/default，故需 service_manager add/find
    （plat 公共类型用版本化影子 hal_thermal_service_{vers}）。thermal HAL 不使用 FMQ，
    故无 ashmem 授权。
    """
    return (
        f"{THERMAL_MARKER}\n"
        + _allow("hal_thermal_default", f"hal_thermal_service_{vers}",
                 "service_manager", ["add", "find"])
        + _allow(f"system_server_{vers}", f"hal_thermal_service_{vers}",
                 "service_manager", ["find"])
    )


def vibrator_hal_cil(vers: str) -> str:
    """vibrator HAL v1 段：hal_vibrator_default 注册 IVibrator/default 与 IVibratorManager/default。

    lineage 基座无 vibrator HAL（vendor 侧既无 VINTF 声明也无 service_contexts 表项），
    VibratorManagerService 的 Vibrators 段为空。自研服务以 hal_vibrator_default 域注册
    android.hardware.vibrator.IVibrator/default 与 IVibratorManager/default，故需
    service_manager add/find（plat 公共类型用版本化影子 hal_vibrator_service_{vers}）。
    vibrator HAL 不使用 FMQ，故无 ashmem 授权。
    """
    return (
        f"{VIBRATOR_MARKER}\n"
        + _allow("hal_vibrator_default", f"hal_vibrator_service_{vers}",
                 "service_manager", ["add", "find"])
        + _allow(f"system_server_{vers}", f"hal_vibrator_service_{vers}",
                 "service_manager", ["find"])
    )


def lights_hal_cil(vers: str) -> str:
    """lights HAL v1 段：hal_light_default 注册 ILights/default。

    lineage 基座无 lights HAL（vendor 侧既无 VINTF 声明也无 service_contexts 表项），
    LightsService 拿不到 aidl 实例、Lights 列表为空。自研服务以 hal_light_default 域注册
    android.hardware.light.ILights/default，故需 service_manager add/find
    （plat 公共类型用版本化影子 hal_light_service_{vers}）。lights HAL 不使用 FMQ，
    故无 ashmem 授权。
    """
    return (
        f"{LIGHTS_MARKER}\n"
        + _allow("hal_light_default", f"hal_light_service_{vers}",
                 "service_manager", ["add", "find"])
        + _allow(f"system_server_{vers}", f"hal_light_service_{vers}",
                 "service_manager", ["find"])
    )


def gnss_hal_cil(vers: str) -> str:
    """gnss HAL v1 段：hal_gnss_default 注册 IGnss/default。

    lineage 基座无 GNSS HAL（vendor 侧既无 VINTF 声明也无 service_contexts 表项，
    GnssLocationProvider 因此不创建）。自研服务以 hal_gnss_default 域注册
    android.hardware.gnss.IGnss/default，故需 service_manager add/find（plat 公共类型
    用版本化影子 hal_gnss_service_{vers}）；另需读 fix 注入控制文件
    /data/vendor/gps/fix —— plat_file_contexts 把 /data/vendor(/.*)? 标为
    vendor_data_file，故授 hal_gnss_default 对 vendor_data_file_{vers} 的只读权限
    （目录由本模块 rc 的 mkdir 建立：vendor rc 动作跑在 vendor_init 域，
    plat 已给 vendor_init 对 file_type-data_file_type 的 dir create 授权）。
    gnss HAL 不使用 FMQ，故无 ashmem 授权。
    """
    return (
        f"{GNSS_MARKER}\n"
        + _allow("hal_gnss_default", f"hal_gnss_service_{vers}",
                 "service_manager", ["add", "find"])
        + _allow(f"system_server_{vers}", f"hal_gnss_service_{vers}",
                 "service_manager", ["find"])
        + _allow("hal_gnss_default", f"vendor_data_file_{vers}", "dir",
                 ["search", "getattr"])
        + _allow("hal_gnss_default", f"vendor_data_file_{vers}", "file",
                 ["read", "open", "getattr"])
    )


def wifi_hal_cil(vers: str) -> str:
    """wifi HAL v1 段：hal_wifi_default 注册 IWifi/default + 档位 sysfs 标签 + 宿主通道。

    lineage 基座没有 IWifi HAL（vendor VINTF 无 android.hardware.wifi-service.xml），
    但 plat 侧已按 hal_wifi 属性备齐能力，本段只需补：
      * service_contexts 的 hal_wifi_service 映射（见 WIFI_VSC_LINES），否则
        AServiceManager_addService 无上下文而失败；
      * 内核模块参数 scan_profile 的独立 sysfs 标签（genfscon）+ 属性挂载：
        plat 已有 `hal_wifi -> sysfs_type (file 只读)`，故挂 sysfs_type 即得读权限；
        挂 file_type 则让 console 域 pioneer 的宽授权覆盖该节点，可写入扫描档位；
        另补 hal_wifi_default 的 write（rc 里 chmod 0666 放开 DAC）与 init 的
        setattr（chmod 本身）；
      * sys_module capability —— rc 声明了 NET_ADMIN NET_RAW SYS_MODULE，其中
        net_admin/net_raw 由 plat 的 `hal_wifi self capability` 覆盖，sys_module
        在本盘基座缺失（cuttlefish 基座的 hal_wifi_default.te 有这一条）；
      * 宿主 wifi 服务通道 /dev/hvc9 的独立设备标签与 HAL 读写授权（见
        WIFI_CHANNEL_TYPE；标签行由 wifi_hal_patch_commands 追加到 vendor_file_contexts）。

    服务注册本身（hal_wifi_server -> hal_wifi_service add/find）与
    hal_wifi -> hal_wifi_client binder transfer/call 都由 plat 规则覆盖，无需重复声明。
    """
    return (
        f"{WIFI_MARKER}\n"
        + _allow("hal_wifi_default", "self", "capability", ["sys_module"])
        + f"(type {WIFI_SCAN_PARAM})\n"
        + f"(roletype object_r {WIFI_SCAN_PARAM})\n"
        + f"(typeattributeset file_type ({WIFI_SCAN_PARAM}))\n"
        + f"(typeattributeset sysfs_type ({WIFI_SCAN_PARAM}))\n"
        + f'(genfscon sysfs "{WIFI_SCAN_PATH}" '
          f"(u object_r {WIFI_SCAN_PARAM} ((s0) (s0))))\n"
        + _allow("hal_wifi_default", WIFI_SCAN_PARAM, "file",
                 ["getattr", "open", "write"])
        + _allow(f"init_{vers}", WIFI_SCAN_PARAM, "file",
                 ["getattr", "setattr"])
        + f"(type {WIFI_CHANNEL_TYPE})\n"
        + f"(roletype object_r {WIFI_CHANNEL_TYPE})\n"
        + f"(typeattributeset dev_type ({WIFI_CHANNEL_TYPE}))\n"
        + _allow("hal_wifi_default", WIFI_CHANNEL_TYPE, "chr_file",
                 "getattr ioctl open read write".split())
        + f"(type {WIFI_DATA_TYPE})\n"
        + f"(roletype object_r {WIFI_DATA_TYPE})\n"
        + f"(typeattributeset dev_type ({WIFI_DATA_TYPE}))\n"
        + _allow("kernel", WIFI_DATA_TYPE, "chr_file",
                 "getattr ioctl open read write".split())
    )


def media_hal_cil(vers: str) -> str:
    """media (codec2) HAL 段：mediacodec 域注册 IComponentStore（假登记 store）。

    基座无 hal_codec2_default 域（hal_codec2 类型属性挂 mediacodec，且 mediacodec 是
    halserverdomain/halclientdomain 成员），故服务以 seclabel u:r:mediacodec:s0 运行，
    本段只补：
      * service_manager add：mediacodec 注册 android.hardware.media.c2.IComponentStore/
        default、default9（service_contexts 为精确实例名匹配，default9 映射见
        MEDIA_VSC_LINES，default 由 plat 提供；按真机形态两实例分别由
        android.hardware.media.c2-mediatek 与
        vendor.dolby.media.c2-default-service-dax 承担，rc 显式 seclabel mediacodec）；
      * mediametrics/mediaserver/mediaextractor 客户端 find：framework 侧
        Codec2Service 枚举 store 需 servicemanager find hal_codec2_service；
      * ashmem_device：FMQ 共享内存（同 audio v2；假登记 store 当前不实例化组件，
        保留授权以兼容后续真组件路径）。
    """
    return (
        f"{MEDIA_MARKER}: allow mediacodec register/find codec2 store\n"
        + f"(type hal_codec2_default_exec)\n"
        + f"(roletype object_r hal_codec2_default_exec)\n"
        + f"(typeattributeset file_type (hal_codec2_default_exec))\n"
        + f"(typeattributeset exec_type (hal_codec2_default_exec))\n"
        + _allow("init", "hal_codec2_default_exec", "file",
                 "read open getattr execute map".split())
        + _allow("init", "mediacodec", "process", ["transition"])
        + _allow("mediacodec", "hal_codec2_default_exec", "file",
                 "entrypoint read open getattr execute map".split())
        + _allow("mediacodec", f"hal_codec2_service_{vers}",
                 "service_manager", "add find".split())
        + _allow(f"mediametrics_{vers}", f"hal_codec2_service_{vers}",
                 "service_manager", ["find"])
        + _allow(f"mediaserver_{vers}", f"hal_codec2_service_{vers}",
                 "service_manager", ["find"])
        + _allow(f"mediaextractor_{vers}", f"hal_codec2_service_{vers}",
                 "service_manager", ["find"])
        + _allow("mediacodec", f"ashmem_device_{vers}", "chr_file",
                 "ioctl read write getattr map open".split())
    )


def coredomain_vendor_prop_read_cil(vers: str) -> str:
    """coredomain 服务直读 vendor_default_prop 属性区（引导修复段）。

    实例1 实测：lmkd/keystore2 读取 /dev/__properties__/u:object_r:vendor_default_prop:s0
    被拒（avc denied { read } tclass=file）后 20ms 内 SIGABRT —— keystore2 起不来导致
    vold 等 IKeystoreService 卡死、boot 永不完成。授权直读该属性区文件即可让服务正常
    启动。风险：若 plat 对 coredomain→vendor_default_prop 有 neverallow，本段会导致
    init 编译策略失败（引导极早失败，可立即发现回退）。
    """
    return (
        f"{VENDOR_PROP_READ_MARKER}\n"
        + _allow(f"lmkd_{vers}", f"vendor_default_prop_{vers}", "file",
                 ["read", "getattr", "map", "open"])
        + _allow(f"keystore_{vers}", f"vendor_default_prop_{vers}", "file",
                 ["read", "getattr", "map", "open"])
        # 诊断：crash_dump 默认被拒 ptrace/dac_override（实测 avc denied），tombstone
        # 永远写不出来，崩溃根因不可见。授权后 lmkd/keystore2 的 abort 栈可落盘定位。
        + _allow(f"crash_dump_{vers}", "domain", "process", ["ptrace"])
        + _allow(f"crash_dump_{vers}", "self", "capability", ["dac_override"])
    )


def chipinfo_cil(vers: str) -> str:
    """chipinfo（SoC 身份）CIL 段：非 coredomain 供应商域写 ro.vendor.soc.*。

    为什么需要独立域：`ro.vendor.soc.*` 的属性类型是 vendor_mtk_soc_prop，属
    vendor_restricted_property_type，plat 的 neverallow（主体 coredomain）禁止
    init / vendor_init 直接 set —— 实测 /vendor/build.prop 里 5 条被 init 丢弃
    （`init: Do not have permissions to set 'ro.vendor.soc.manufacturer' ...
    SELinux permission check failed`，scontext=u:r:vendor_init:s0）。
    真机（gold）由官方 /vendor/bin/chipinfo 在独立域 chipinfo 里 setprop 完成，
    本段即复刻该域（gold vendor_sepolicy.cil 同名规则，去掉 sysfs/device-tree 读取
    授权——云机无 MTK sysfs）。

    规则来源：gold `vendor_sepolicy.cil` 的 chipinfo 段逐条对齐
      (type chipinfo)/(type chipinfo_exec) + 属性归属；
      init -> chipinfo_exec:file execute、init -> chipinfo:process transition、
      typetransition init chipinfo_exec process chipinfo（rc 直接 exec 二进制，
      无需 seclabel）；
      chipinfo -> chipinfo_exec:file entrypoint；
      chipinfo -> property_socket 写 + -> init unix_stream_socket 连接（setprop 通道）；
      chipinfo -> vendor_mtk_soc_prop property_service set（唯一目的）。
    真机的解释器授权（chipinfo -> vendor_toolbox_exec execute_no_trans）不需要：
    本实现是原生二进制，不经 shell 解释器。
    """
    return (
        f"{CHIPINFO_MARKER}\n"
        "(type chipinfo)\n"
        "(roletype object_r chipinfo)\n"
        "(type chipinfo_exec)\n"
        "(roletype object_r chipinfo_exec)\n"
        "(typeattributeset domain (chipinfo))\n"
        "(typeattributeset exec_type (chipinfo_exec))\n"
        "(typeattributeset file_type (chipinfo_exec))\n"
        "(typeattributeset vendor_file_type (chipinfo_exec))\n"
        + _allow(f"init_{vers}", "chipinfo_exec", "file",
                 "read getattr map execute open".split())
        + _allow(f"init_{vers}", "chipinfo", "process",
                 "transition siginh rlimitinh".split())
        + f"(typetransition init_{vers} chipinfo_exec process chipinfo)\n"
        + _allow("chipinfo", "chipinfo_exec", "file",
                 "read getattr map execute open entrypoint".split())
        + _allow("chipinfo", f"property_socket_{vers}", "sock_file", ["write"])
        + _allow("chipinfo", f"init_{vers}", "unix_stream_socket", ["connectto"])
        + _allow("chipinfo", "vendor_mtk_soc_prop", "property_service", ["set"])
        + _allow("chipinfo", "vendor_mtk_soc_prop", "file",
                 "read getattr map open".split())
    )


def system_ext_policy_names(vers: str) -> set:
    """官方 system_ext 策略声明的类型/属性名全集（含 mapping/<vers>.cil 的版本化影子）。

    为什么 vendor 侧校验需要它：Pixel 专属应用域（grilservice_app、
    pixelsystemservice_app 等）由 `device/google/gs101-sepolicy/system_ext/**` 声明，
    其版本化影子 `*_202504` 只出现在官方 system_ext 分区的
    `/etc/selinux/mapping/<vers>.cil` 里；vendor 基座 cil 与 plat_pub_versioned.cil
    都看不到，validate_cil 会把这些**真实存在**的名字判成未知类型。本盘
    system_ext 未经改动（官方镜像），故直接以官方 system_ext.img 为名字来源。
    读不到（无该分区/非 Pixel 机型）时返回空集，pixel_gs101_app_cil 随之不发射。
    """
    img = os.path.join(ROM_ROOT_DIR, "system_ext.img")
    if not os.path.isfile(img):
        return set()
    names = set()
    for path in (SELINUX_DIR + "/system_ext_sepolicy.cil",
                 f"{SELINUX_DIR}/mapping/{vers}.cil"):
        blob = cat(img, path)
        if blob:
            names |= _cil_names(blob.decode("utf-8", "replace"))
    return names


def pixel_gs101_app_cil(vers: str, names: set) -> str:
    """官方 gs101 vendor 策略里、本盘缺失的 Pixel 应用域 service find（逐条对齐官方）。

    根因（2026-10-11 实测，实例 16）：本盘 vendor 换成 lineage/gold 基座后，官方
    `device/google/gs101-sepolicy/whitechapel/vendor/google/*.te` 编进
    vendor_sepolicy.cil 的那批授权随之消失，而 system_ext/product 仍是**官方**分区，
    里面的 Pixel 专属应用域于是落到「有域、无授权」的策略：

      * grilservice_app 缺官方的
          `(allow grilservice_app_202504 app_api_service (service_manager (find)))`
        → ActivityThread.attach 里 ActivityManager.getService() 返回 null →
        `NullPointerException: IActivityManager.attachApplication … on a null object
        reference`，每 ~87s 一次 FATAL 死循环（com.google.android.grilservice 由
        scone / connectivitythermalpowermanager 绑定，AMS attach 超时后反复重拉）。
        app_api_service 属性含 activity_service / wifi_service / netstats_service，
        一条即覆盖实测到的三条 find 拒绝。
      * pixelsystemservice_app 缺
          `(allow pixelsystemservice_app_202504 cameraserver_service_202504 (service_manager (find)))`
        → `avc: denied { find } … name=media.camera` 每秒一条刷屏。

    只移植目标类型在本盘策略里存在的这两条；官方其余 grilservice 规则引用的
    hal_bluetooth_btlinux / hal_radioext_default / hal_wifi_ext 等是 gs101 **vendor**
    专有类型，lineage/gold 基座里没有，引用会让 init 内嵌 secilc 编译失败。
    """
    stmts = ""
    known = plat_name_set() | names
    if f"grilservice_app_{vers}" in known and "app_api_service" in known:
        stmts += _allow(f"grilservice_app_{vers}", "app_api_service",
                        "service_manager", ["find"])
    if (f"pixelsystemservice_app_{vers}" in known
            and f"cameraserver_service_{vers}" in known):
        stmts += _allow(f"pixelsystemservice_app_{vers}",
                        f"cameraserver_service_{vers}",
                        "service_manager", ["find"])
    return (f"{PIXEL_APP_MARKER}\n" + stmts) if stmts else ""


def hal_cil_chain(merged: str, vers: str, names=None) -> str:
    """按固定顺序补齐所有缺失的 CDH HAL CIL 段
    （pioneer/audio/keymint/radio/sensors/camera/thermal/vibrator/lights/gnss/wifi）
    + chipinfo（SoC 身份）
    + 官方 gold vendor 属性类型段（gold_props，/dev/__properties__ 对齐）。

    各 *_patch_commands 都基于工作镜像里的「原始」vendor_sepolicy.cil 拼接（debugfs 命令
    此刻尚未执行），而命令序列里只有最后一次 write 生效 —— 所以最后调用的那个函数必须把
    所有前序段一并写出，否则会把它们覆盖丢失（实测：只写自己的段会让
    radio/audio/keymint/pioneer 段全部消失，开机卡在 vold/keystore2）。
    新增 HAL 段时只需在这里加一行，再由该 HAL 的 *_patch_commands 调用本函数即可。
    names = 本策略已声明的类型/属性名全集（供 gold prop types 段过滤源域不存在的授权）。
    """
    cil = ""
    if MARKER not in merged:
        cil += pioneer_cil(vers)
    if AUDIO_MARKER not in merged:
        cil += audio_hal_cil(vers)
    if AUDIO_V2_MARKER not in merged:
        cil += audio_hal_cil_v2(vers)
    if AUDIO_V3_MARKER not in merged:
        cil += audio_hal_cil_v3(vers)
    if KEYMINT_MARKER not in merged:
        cil += keymint_hal_cil(vers)
    if KEYMINT_MARKER2 not in merged:
        cil += keymint_hal_cil_v2(vers)
    if RADIO_MARKER not in merged:
        cil += (radio_hal_cil(vers) + radio_hal_cil_v2(vers) +
                radio_hal_cil_v3(vers) + radio_hal_cil_v4(vers))
    elif RADIO_V2_MARKER not in merged:
        cil += radio_hal_cil_v2(vers) + radio_hal_cil_v3(vers) + \
            radio_hal_cil_v4(vers)
    elif RADIO_V3_MARKER not in merged:
        cil += radio_hal_cil_v3(vers) + radio_hal_cil_v4(vers)
    elif RADIO_V4_MARKER not in merged:
        cil += radio_hal_cil_v4(vers)
    if SENSORS_MARKER not in merged:
        cil += sensors_hal_cil(vers)
    if CAMERA_MARKER not in merged:
        cil += camera_hal_cil(vers)
    if THERMAL_MARKER not in merged:
        cil += thermal_hal_cil(vers)
    if VIBRATOR_MARKER not in merged:
        cil += vibrator_hal_cil(vers)
    if LIGHTS_MARKER not in merged:
        cil += lights_hal_cil(vers)
    if GNSS_MARKER not in merged:
        cil += gnss_hal_cil(vers)
    if WIFI_MARKER not in merged:
        cil += wifi_hal_cil(vers)
    if MEDIA_MARKER not in merged:
        cil += media_hal_cil(vers)
    if VENDOR_PROP_READ_MARKER not in merged:
        cil += coredomain_vendor_prop_read_cil(vers)
    # Pixel(gs101) 专属：官方 vendor 策略里那批「system_ext 应用域 -> service find」
    # 授权随 vendor 换基座丢失，需按官方 cil 原样补回（见 pixel_gs101_app_cil 注释）。
    if PIXEL_APP_MARKER not in merged:
        cil += pixel_gs101_app_cil(vers, names or set())
    # chipinfo 段的目标属性类型 vendor_mtk_soc_prop 来自 gold MTK 策略；Pixel(gs101)
    # 基座/gold 策略里没有这个类型，且注入树不带 /vendor/bin/chipinfo 二进制，
    # 整段对该机型无意义 —— 按「属性类型是否存在于本策略名全集」门控。
    if CHIPINFO_MARKER not in merged and \
            "vendor_mtk_soc_prop" in (names or set()):
        cil += chipinfo_cil(vers)
    if gold_props.MARKER not in merged:
        cil += gold_props.prop_types_cil(vers, names or set())
    return cil


def hal_vsc_missing(vsc: bytes) -> list:
    """vendor_service_contexts 里尚缺的 CDH HAL 服务映射行
    （audio/radio/sensors/camera/thermal/vibrator/lights/gnss/wifi）。"""
    wanted = (list(AUDIO_VSC_LINES) + list(RADIO_VSC_LINES) +
              list(SENSORS_VSC_LINES) + list(CAMERA_VSC_LINES) +
              list(THERMAL_VSC_LINES) + list(VIBRATOR_VSC_LINES) +
              list(LIGHTS_VSC_LINES) + list(GNSS_VSC_LINES) +
              list(WIFI_VSC_LINES) + list(MEDIA_VSC_LINES))
    return [line for line in wanted if line.encode("utf-8") not in vsc]


def hal_policy_patch_commands(work_img, tmp_dir=None) -> list:
    """补齐 CDH 各 HAL 的 vendor_sepolicy.cil 段 + vendor_service_contexts 映射。

    覆盖：pioneer 自由域、audio（含 ashmem）、keymint、radio（含 dsds 属性）、
    sensors（含 ashmem）、camera（含 ashmem）、thermal、vibrator、lights、gnss、wifi、
    media（codec2 假登记 store）。
    这是 vendor_sepolicy.cil / vendor_service_contexts 的**最后一处写入者**：各
    *_patch_commands 都读取工作镜像的原始内容再拼接，命令序列里只有最后一次 write 生效，
    故由 hal_cil_chain / hal_vsc_missing 统一补齐全部缺失段（详见 hal_cil_chain 注释）。
    """
    vers = cat(work_img, VERS_FILE)
    if not vers:
        print(f"[err] 读不到 {VERS_FILE}")
        sys.exit(1)
    vers = vers.decode("ascii", "ignore").strip()
    tmp_dir = tmp_dir or os.path.join(TMP_DIR, "vendor")
    os.makedirs(tmp_dir, exist_ok=True)

    cmds = []
    orig = cat(work_img, POLICY_FILE)
    if orig is None:
        print(f"[err] 读不到 {POLICY_FILE}")
        sys.exit(1)
    merged = orig.decode("utf-8", "replace")
    name_universe = _cil_names(merged) | \
        _cil_names(_read_cil(work_img, PUB_VER_FILE)) | system_ext_policy_names(vers)
    cil = hal_cil_chain(merged, vers, name_universe)
    if cil:
        validate_cil(cil, name_universe)
        tmp = os.path.join(tmp_dir, "vendor_sepolicy_hal_chain.cil")
        with open(tmp, "wb") as f:
            f.write(orig + cil.encode("utf-8"))
        print(f"[ok] vendor_sepolicy.cil 追加 CIL 段（含前序段补齐，plat vers {vers}，"
              f"+{len(cil)} B）")
        cmds += [
            f"rm {POLICY_FILE}",
            f"write {tmp} {POLICY_FILE}",
            f"ea_set {POLICY_FILE} security.selinux {POLICY_LABEL}",
        ]
    else:
        print("[ok] vendor_sepolicy.cil 各段齐全，跳过")

    vsc = cat(work_img, VSC)
    if vsc is None:
        print(f"[err] 读不到 {VSC}")
        sys.exit(1)
    missing = hal_vsc_missing(vsc)
    if not missing:
        print("[ok] vendor_service_contexts 已含 audio/radio/sensors/camera/thermal/vibrator/lights/gnss/wifi/media 映射，跳过")
    else:
        append_blob = ("\n# cdh note13-5G-15: audio/radio/sensors/camera/thermal/vibrator/lights/gnss/wifi/media "
                       "AIDL HAL services\n" + "\n".join(missing) + "\n").encode("utf-8")
        tmp = os.path.join(tmp_dir, "vendor_service_contexts_hal_chain")
        with open(tmp, "wb") as f:
            f.write(vsc + append_blob)
        print(f"[ok] vendor_service_contexts 追加 {len(missing)} 条映射"
              f"（audio+radio+sensors+camera+thermal+vibrator+lights+gnss+wifi+media）")
        cmds += [
            f"rm {VSC}",
            f"write {tmp} {VSC}",
            f"ea_set {VSC} security.selinux {VSC_LABEL}",
        ]
    return cmds


def sensors_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """sensors AIDL HAL（自研虚拟 HAL）注册所需的策略补齐。

    内容见 hal_policy_patch_commands / hal_cil_chain：sensors 段含
    ISensors/default 的 service_contexts 表项、hal_sensors_default 的
    service_manager add/find，以及事件队列 FMQ 需要的 ashmem 授权。
    """
    return hal_policy_patch_commands(work_img, tmp_dir)


def thermal_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """thermal AIDL HAL（自研虚拟 HAL）注册所需的策略补齐。

    内容见 hal_policy_patch_commands / hal_cil_chain：thermal 段含
    IThermal/default 的 service_contexts 表项与 hal_thermal_default 的
    service_manager add/find。
    """
    return hal_policy_patch_commands(work_img, tmp_dir)


def vibrator_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """vibrator AIDL HAL（自研虚拟 HAL）注册所需的策略补齐。

    内容见 hal_policy_patch_commands / hal_cil_chain：vibrator 段含
    IVibrator/default 与 IVibratorManager/default 的 service_contexts 表项、
    hal_vibrator_default 的 service_manager add/find。
    """
    return hal_policy_patch_commands(work_img, tmp_dir)


def lights_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """lights AIDL HAL（自研虚拟 HAL）注册所需的策略补齐。

    内容见 hal_policy_patch_commands / hal_cil_chain：lights 段含
    ILights/default 的 service_contexts 表项与 hal_light_default 的
    service_manager add/find。
    """
    return hal_policy_patch_commands(work_img, tmp_dir)


def gnss_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """gnss AIDL HAL（自研虚拟 HAL）注册所需的策略补齐。

    内容见 hal_policy_patch_commands / hal_cil_chain：gnss 段含 IGnss/default 的
    service_contexts 表项、hal_gnss_default 的 service_manager add/find，以及
    fix 注入控制文件 /data/vendor/gps/fix（vendor_data_file）的只读授权。
    """
    return hal_policy_patch_commands(work_img, tmp_dir)


def media_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """media (codec2) AIDL HAL 注册所需的策略补齐。

    内容见 hal_policy_patch_commands / hal_cil_chain：media 段含 IComponentStore 的
    service_contexts 表项（MEDIA_VSC_LINES -> hal_codec2_service）与 mediacodec 域的
    service_manager add/find、ashmem_device（FMQ）授权。无额外文件上下文行。
    """
    return hal_policy_patch_commands(work_img, tmp_dir)


def vfc_missing_hal_lines(vfc: bytes) -> list:
    """vendor_file_contexts 里尚缺的 CDH 设备字面行
    （keymint hvc3 / wifi hvc9+hvc10 / FRP vda12）。

    与 vendor_sepolicy.cil 同机制：各 patch 读到的都是工作镜像里的「原始」文件，debugfs
    命令按序执行、只有最后一次 write 生效 —— wifi 是最后一个 vendor_file_contexts
    写入者，故必须把 keymint 段一并补齐，否则 hvc3 标签会丢。
    """
    missing = []
    if b"u:object_r:keymaster_device:s0" not in vfc:
        missing += KEYMINT_FC_LINES
    if f"u:object_r:{WIFI_CHANNEL_TYPE}:s0".encode() not in vfc:
        missing += WIFI_FC_LINES
    if f"u:object_r:{WIFI_DATA_TYPE}:s0".encode() not in vfc:
        missing += WIFI_DATA_FC_LINES
    if b"u:object_r:frp_block_device:s0" not in vfc:
        missing += FRP_FC_LINES
    return missing


def wifi_hal_patch_commands(work_img, tmp_dir=None) -> list:
    """wifi AIDL HAL（自研虚拟 HAL）注册所需的策略补齐。

    CIL 段与 vendor_service_contexts 映射见 hal_policy_patch_commands（wifi 段含
    IWifi/default 表项、sys_module、scan_profile sysfs 标签、宿主通道 /dev/hvc9 与
    数据面 /dev/hvc10 授权）；本函数另外补 vendor_file_contexts 的 /dev/hvc9+hvc10
    字面标签 —— wifi 是该文件的最后一个写入者，须一并补齐 keymint 的 hvc3 行
    （见 vfc_missing_hal_lines）。
    """
    cmds = hal_policy_patch_commands(work_img, tmp_dir)
    tmp_dir = tmp_dir or os.path.join(TMP_DIR, "vendor")
    os.makedirs(tmp_dir, exist_ok=True)
    vfc = cat(work_img, VFC)
    if vfc is None:
        print(f"[err] 读不到 {VFC}")
        sys.exit(1)
    missing = vfc_missing_hal_lines(vfc)
    if not missing:
        print("[ok] vendor_file_contexts 已含 keymint/wifi/FRP 设备标签，跳过")
        return cmds
    tmp = os.path.join(tmp_dir, "vendor_file_contexts_hal_chain")
    with open(tmp, "wb") as f:
        f.write(vfc + ("\n" + "\n".join(missing) + "\n").encode("utf-8"))
    print(f"[ok] vendor_file_contexts 追加 {len(missing)} 条设备标签"
          f"（keymint hvc3 + wifi hvc9/hvc10 + FRP vda12）")
    cmds += [
        f"rm {VFC}",
        f"write {tmp} {VFC}",
        f"ea_set {VFC} security.selinux {VFC_LABEL}",
    ]
    return cmds


def patch_commands(work_img, tmp_dir=None) -> list:
    """cat 出基座 vendor_sepolicy.cil 追加 pioneer 段，返回 debugfs 命令清单。

    幂等：基座副本每次从 lineage vendor_a.img 重新拷贝，正常不会已含标记；
    若已含（重复构建同一 work.img）则跳过不重复追加。
    """
    vers = cat(work_img, VERS_FILE)
    if not vers:
        print(f"[err] 读不到 {VERS_FILE}")
        sys.exit(1)
    vers = vers.decode("ascii", "ignore").strip()

    orig = cat(work_img, POLICY_FILE)
    if orig is None:
        print(f"[err] 读不到 {POLICY_FILE}")
        sys.exit(1)
    if MARKER in orig.decode("utf-8", "replace"):
        print("[ok] vendor_sepolicy.cil 已含 pioneer 段，跳过")
        return []

    cil = pioneer_cil(vers)
    # 名字全集: vendor 基座自有类型（mediacodec 等，未版本化）
    #          + plat_pub_versioned.cil（plat 公共类型的版本化 typeattribute 影子）
    #          + plat cil（plat_name_set 内部缓存）
    name_universe = _cil_names(orig.decode("utf-8", "replace")) | \
        _cil_names(_read_cil(work_img, PUB_VER_FILE))
    validate_cil(cil, name_universe)
    patched = orig + cil.encode("utf-8")
    tmp_dir = tmp_dir or os.path.join(TMP_DIR, "vendor")
    os.makedirs(tmp_dir, exist_ok=True)
    tmp = os.path.join(tmp_dir, "vendor_sepolicy.cil")
    with open(tmp, "wb") as f:
        f.write(patched)
    print(f"[ok] vendor_sepolicy.cil 追加 pioneer 段（plat vers {vers}，"
          f"+{len(patched) - len(orig)} B）")
    return [
        f"rm {POLICY_FILE}",
        f"write {tmp} {POLICY_FILE}",
        f"ea_set {POLICY_FILE} security.selinux {POLICY_LABEL}",
    ]
