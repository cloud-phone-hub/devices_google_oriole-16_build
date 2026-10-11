# android.hardware.wifi — guest 内 Wi-Fi（v8：hwsim 内核直桥 + hvc10 数据面）

> **状态：方案定稿（讨论确认），待实施。**
>
> 本 README 是 v8 方案的权威记录：guest 的 Wi-Fi 走 **mac80211_hwsim 网卡**，
> 列表与连接由 **host 经 hvc9 控制面下发/指定**，数据面由 **hwsim 内核直桥 hvc10**
> （**mac80211 封装 802.11 之前截获以太帧**，无 802.11 转换、无用户态转发）到 host 网关。

---

## 1. 需求（用户确认）

1. **连接**：guest 能连 **host 传入的指定 wifi**（连接由 host 侧下发指定）。
2. **列表**：Settings 显示 **host 下发的模拟列表**（scan_profile 假 AP）。
3. **流量**：guest 的流量走 **wifi 网卡**（mac80211_hwsim 的 wlan0），不走 eth0。
4. **桥接**：wifi 网卡流量经 QEMU virtio-serial（hvc10）到 host。
5. **网关**：host 解析 IP，**全协议**（ARP/DHCP/DNS/TCP/UDP/ICMP）。
6. **出口**：默认走 host 网络；可指定 **socks5**。

**网卡约束**：wifi 必须走 **mac80211_hwsim**（保留 hwsim 驱动，不走 virt_wifi）。

**数据面约束**（讨论定稿）：**数据不走 802.11 封装路径**——在 mac80211 把以太帧
封装成 802.11 之前（wlan0 netdev xmit 处）截获，**以太帧直桥 hvc10**；host 回包
**以太帧注入 wlan0 接收**（`netif_receive_skb`）。纯内核闭环，无 802.11 转换、
无 guest 用户态转发。

---

## 2. 架构（v8 定稿）

```
guest 内核（mac80211_hwsim）                          QEMU              host
┌──────────────────────────────────────────┐   ┌──────────┐   ┌──────────────────────────┐
│ 列表: supplicant 扫描 wlan0               │   │ hvc9     │   │ wifi-host-service        │
│   hwsim 假扫描(scan_profile) → 假 AP      │◄─►│ 控制面    │◄─►│ ─ scan_profile 列表下发   │
│ 连接: supplicant 连 wlan0                 │   │          │   │ ─ 连接指定               │
│   hwsim 假连接(auth/assoc 响应)           │   │ hvc10    │   │ ─ 出口: 默认直连 / socks5 │
│ 数据面:                                  │   │ 数据面    │   │ ─ ARP/DHCP/DNS/TCP/UDP  │
│   wlan0 netdev xmit(以太帧, mac80211      │   │          │   │ ─ ICMP                  │
│     封装 802.11 之前)                     │──►│          │──►│                         │
│   └ hwsim 包装 ndo_start_xmit 截获 →     │   │          │   │                         │
│     [4B len][以太帧] → 写 hvc10           │   │          │   │                         │
│   host 回包 → 内核线程读 hvc10 → 以太帧    │◄──│          │◄──│                         │
│     → netif_receive_skb(wlan0) → 协议栈   │   │          │   │                         │
└──────────────────────────────────────────┘   └──────────┘   └──────────────────────────┘
```

**数据流**：
- **列表**：host → hvc9 `scan_profile` 帧 → guest wifi HAL 写 hwsim `scan_profile` sysfs →
  supplicant 扫描 wlan0 时 hwsim 注入假 AP beacon → `cfg80211_inform_bss` → Settings 列表。
- **连接**：host 经 hvc9 指定 AP → `cmd wifi connect` → supplicant 连 wlan0 →
  hwsim 模拟假 AP 的 auth/assoc 响应 → 连接立即成功。
- **数据（TX）**：应用流量 → 路由 wlan0 → **wlan0 netdev xmit（以太帧）**——
  **hwsim 包装的 `ndo_start_xmit`（mac80211 封装 802.11 之前）截获** → 组
  `[4B BE len][以太帧]` → `kernel_write(/dev/hvc10)` → host 网关 → 出口（直连/socks5）。
- **数据（RX）**：host 回包 → hvc10 → **hwsim 内核线程 `kernel_read`** → 以太帧 →
  `netif_receive_skb(wlan0)` → guest 协议栈。

> **为什么在 netdev 层截获而不是驱动层**：wlan0 是 mac80211 的 station 界面，
> 以太帧在 `ieee80211_subif_start_xmit`（netdev tx）才被封装成 802.11，到
> `mac80211_hwsim_tx`（驱动层）已是 802.11。**在封装前（netdev xmit）截获就是纯
> 以太帧**，host 网关直接全协议处理，**免去 802.11→eth 转换**。

---

## 3. 组件与改动

### 3.1 内核：mac80211_hwsim 改造（核心，纯内核闭环）

源码：`kernel/source/drivers/net/wireless/virtual/mac80211_hwsim.c`（6721 行，
mac80211 驱动的"射频模拟器"）。改造点：

- [ ] **`scan_profile` 模块参数**：host 经 hvc9 → guest HAL 写入
  `/sys/module/mac80211_hwsim/parameters/scan_profile`（仿 cdh_virt_wifi 格式）。
- [ ] **假扫描注入**：`hw_scan_work` 扫描完成后，对 scan_profile 每个假 AP 构造
  beacon 注入 `mac80211_hwsim_rx` → mac80211 `cfg80211_inform_bss` → supplicant
  拿到假 AP，Settings 列表显示 host 下发列表。
- [ ] **假连接**：supplicant 连假 AP（auth/assoc 请求）→ hwsim 模拟假 AP 的
  auth/assoc 响应 → mac80211 连接状态机完成 → `cmd wifi status` connected。
- [ ] **数据面直桥（关键）**：
  - `add_interface` 时拿到 wlan0 netdev，**保存 mac80211 原 `ndo_start_xmit`，
    替换为 hwsim 包装函数**（仅在假连接成功后启用旁路）。
  - **TX**：包装函数收到**以太帧 skb** → 组 `[4B BE len][以太帧]` →
    `kernel_write` 写 hvc10 → 返回 `NETDEV_TX_OK`（不交 mac80211 封装 802.11）。
  - **RX**：hwsim 内核线程阻塞 `kernel_read` hvc10 → 以太帧 →
    `netif_receive_skb(wlan0)`（注入接收）→ guest 协议栈。
  - **hvc10 访问**：`filp_open("/dev/hvc10", O_RDWR)` + `kernel_read`/`kernel_write`
    （virtio_console tty；需 raw 模式——由内核 tty 设置或保留最小 daemon 只做 raw）。
  - **独占**：hvc10 数据面由 hwsim 内核独占，guest-daemon 不再读写数据面。
- [ ] **radios**：保留 `radios=2`（wlan0 station；wlan1 预留/移除，不再 hostapd）。

> 连接方式说明（v8 选型）：**假连接，无 hostapd**。hostapd/wlan1/真 AP 数据面退役，
> shim（cdhioctl）已移除。数据面为**内核直桥**（netdev 层截获以太帧），无 daemon 转发。

### 3.2 guest-daemon —— **已落地**

- [x] `core/guest/daemon/rust/src/wifi/mod.rs`：**已删除**（数据面由内核直桥接管）。
- [x] `main.rs`：移除 `mod wifi;`、`--wifi-data` 参数、`wifi::spawn()`。
- [x] `cph_daemon.rc`：`service daemon /vendor/bin/daemon run --event /dev/hvc7`（去掉 `--wifi-data`）。
- [x] `channel.rs`：新增事件通道帧 **`wifi_connect`**（`{"type":"wifi_connect","ssid":"CDH-A","security":"open"}`
  → 后台线程执行 `cmd wifi connect-network <ssid> <security>`，ack 立即返回，结果记 kmsg）。
  `make_raw` 恢复私有（不再被 wifi 模块复用）。

> **连接指定为何走 hvc7 而不是 hvc9**：框架侧连接只能由 `cmd wifi connect-network` 触发，
> 该命令要求 shell/root uid（NETWORK_SETTINGS）；wifi HAL 以 `wifi`(1010) 运行无此权限，
> 而 guest-daemon 以 root 运行 → 由 daemon 代理执行。

### 3.3 guest wifi HAL（android.hardware.wifi 模块）—— **已落地**

- [x] `scan_profile` 写入目标改为 **hwsim sysfs**：`config/wifi.json` 的 `scanProfilePath`、
  `include/cdh_wifi_scan_model.h` 的 `CDH_WIFI_SCAN_MODEL_PATH`、
  rc 的 `chmod 0666 /sys/module/mac80211_hwsim/parameters/scan_profile`、
  `build/vendor/build/sepolicy.py` 的 `WIFI_SCAN_PATH`；
  构建后二进制内嵌路径已验证为 `/sys/module/mac80211_hwsim/parameters/scan_profile`。
- [x] **连接指定不在本 HAL**（uid wifi 无 NETWORK_SETTINGS）：见 3.2 的 `wifi_connect`。

### 3.4 host

- [ ] `wifi-host-service`（hvc9 控制面 + hvc10 数据面）**全部保留**（v7 已实现并验收）。
- [ ] hvc9：下发 `scan_profile`（已有）+ 新增**连接指定**帧。
- [ ] `wifi-aps.json`：出口配置（`backend` 空 = 直连 host 网络 / `socks5://...`）——已有，
    核对与 scan_profile 假 AP 的 SSID 对齐。
- [ ] 网关能力（ARP/DHCP/DNS/captive/TCP/UDP/ICMP + per-AP 出口）——已有，无需改动。

---

## 4. 待办清单（按序）

1. [x] 内核改造 `mac80211_hwsim`（scan_profile 假扫描 + 假连接 + 数据面直桥：
   ndo_start_xmit 包装、hvc10 内核读写、内核线程）→ `out/kernel/modules/mac80211_hwsim.ko`
   构建通过（`build-recipe` v18；内核仓本地提交）。
2. [x] guest HAL：scan_profile 写 hwsim sysfs（连接指定移至 daemon，见 3.2）。
3. [x] guest-daemon：移除 wifi 模块与 `--wifi-data`；新增 `wifi_connect` 帧代理连接。
4. [x] host：连接指定复用既有 `POST /cmd`（`run/channel.go` 的 `sendEventJSON` 原样转发到
   hvc7），下发 `{"type":"wifi_connect","ssid":"CDH-A","security":"open"}` 即可，无需新增 host 代码；
   `wifi-aps.json` 核对并调整出口：**CDH-A（APs[0]，默认出口）= 空（直连 host 网络）**，
   CDH-B/C 保留 socks5 示例（`socks5://127.0.0.1:7891` / `:1080`）；`security` 缺省 open
   与内核假连接（仅开放网络）一致。
5. [ ] 构建 vendor/vendor_dlkm（打包新 hwsim.ko）/patch 镜像 → 起新实例验收：
   - `cmd wifi list-scan-results` 显示 host 下发列表（VirtWifi + CDH-A/B/C 等）
   - `cmd wifi status` connected（host 指定）
   - 外网 TCP（默认直连）+ socks5 出口各验一次
   - host `wifi-data.sock.log` 有流量

---

## 5. 已知耦合（改动时注意）

- **数据旁路启用时机**：仅假连接成功后启用 wlan0 的 ndo_start_xmit 包装，mac80211
  的扫描/管理帧仍走原路径，避免干扰 supplicant 扫描与连接状态机。
- **scan_profile 假 AP 的 SSID/BSSID** 与 host `wifi-aps.json` 的 AP 对齐——host 网关按
  客户端上报 AP 身份选出口（未上报按 `aps[0]`）。
- **hvc10 独占与 raw**：数据面由 hwsim 内核独占；/dev/hvc10 是 tty，需 raw 模式
  （内核 tty 设置或保留最小 daemon 仅做 raw 初始化）。
- **hvc9 控制面 raw**：guest HAL 打开 /dev/hvc9 必须 cfmakeraw（v7 已修，hvc9 是 tty）。
- **模块加载**：`mac80211_hwsim` 需带 `scan_profile` 参数支持；`cdh_virt_wifi` 模块
  退役（不再用于列表）。
- **帧格式**：hvc10 双向均为 `[4B BE len][以太帧]`（host 网关协议，v7 一致）。

---

## 附录：v7 历史（hostapd + hvc10 真 AP，已退役）

- 架构：hwsim 双 radio，wlan1 = hostapd（CDH-A 真 AP），wlan0 连 wlan1，
  wlan1 明文帧经 guest-daemon（AF_PACKET 抓 wlan1 netdev）→ hvc10 到 host 网关；
  shim `libcdhioctl.so`（cdhioctl.c）解决 pioneer 域 ioctl EACCES。
- **退役原因**：Settings 列表来自 supplicant 对 wlan0 的真扫描（只显示 wlan1 广播的
  CDH-A），**scan_profile 假 AP 进不了列表**（framework 只认 supplicant 真扫描）；
  且数据面依赖 hostapd + 用户态转发（daemon）。
- v8 改走 **hwsim 假扫描 + 假连接**（内核注入 scan_profile），解决列表/连接；
  **数据面内核直桥**（netdev 层截获以太帧 → hvc10），去掉 hostapd 与 daemon 转发。
- 已移除：`build/vendor/source/daemon/src/cdhioctl.c`、`libcdhioctl.so` 标签与产物、
  `wifi/mod.rs` 的 LD_PRELOAD。
