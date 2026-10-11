// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#ifndef CDH_WIFI_HOST_CHANNEL_H_
#define CDH_WIFI_HOST_CHANNEL_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace cdh_wifi {

// 宿主 wifi 服务通道（默认 /dev/hvc9，v3 顺延布局 wifi 控制面）客户端。
//
// QEMU 以 unix socket（<实例>/channels/wifi.sock）承载 hvc9，宿主侧
// wifi-host-service 按 [4B 大端长度][UTF-8 JSON] 下发 AP 档位；本类在后台线程
// 收帧、校验并写入内核模块参数 scan_profile（mac80211_hwsim 据此
// cfg80211_inform_bss 上报 AP），从而由宿主决定 guest 能扫到什么。
//
// 只依赖 libc/libc++（自写极简字段提取，不引入 JSON 库）；断线自动重连。
class HostChannel {
 public:
  HostChannel() = default;
  ~HostChannel();

  HostChannel(const HostChannel &) = delete;
  HostChannel &operator=(const HostChannel &) = delete;

  // 起后台线程（幂等）。
  void start();
  // 置停止位并 join（析构自动调用）。
  void stop();

 private:
  void loop();
  // 处理一帧 JSON：返回是否已产生应答（应答由 handleFrame 内部直接发出）。
  void handleFrame(const std::string &frame);
  bool applyScanProfile(const std::string &profile, std::string *detail);
  bool sendJson(const std::string &payload);
  void sendHello();
  void sendAck(int64_t seq, bool ok, const std::string &detail);

  std::atomic<bool> stop_{false};
  std::atomic<int> fd_{-1};
  std::thread thread_;
};

}  // namespace cdh_wifi

#endif  // CDH_WIFI_HOST_CHANNEL_H_
