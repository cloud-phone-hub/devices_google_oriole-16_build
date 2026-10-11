// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#include "cdh_wifi_host_channel.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "../include/cdh_wifi_scan_model.h"
#include "wifi_target.inc"

namespace cdh_wifi {
namespace {

constexpr char kLogTag[] = "CdhWifiHal";
// poll 切片：保持 stop() 的响应性（join 最多等一个切片），空闲累计到读超时再发保活 ping
constexpr int kPollSliceMs = 500;
constexpr size_t kDetailMax = 160;

void sleepMilliseconds(int delayMs) {
  if (delayMs <= 0) return;
  timespec remaining = {};
  remaining.tv_sec = delayMs / 1000;
  remaining.tv_nsec = static_cast<long>(delayMs % 1000) * 1000000L;
  while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR) {
  }
}

bool ReadAll(int fd, void *buffer, size_t size) {
  auto *bytes = static_cast<uint8_t *>(buffer);
  size_t done = 0;
  while (done < size) {
    const ssize_t got = read(fd, bytes + done, size - done);
    if (got > 0) {
      done += static_cast<size_t>(got);
      continue;
    }
    if (got < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool WriteAll(int fd, const void *buffer, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(buffer);
  size_t done = 0;
  while (done < size) {
    const ssize_t put = write(fd, bytes + done, size - done);
    if (put > 0) {
      done += static_cast<size_t>(put);
      continue;
    }
    if (put < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

std::string EscapeJson(const std::string &text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      escaped.push_back('\\');
      escaped.push_back(c);
    } else if (c == '\n' || c == '\r' || c == '\t') {
      escaped.push_back(' ');
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      escaped.push_back(c);
    }
  }
  if (escaped.size() > kDetailMax) escaped.resize(kDetailMax);
  return escaped;
}

// ---- 极简 JSON 字段提取 ----
// 只服务本通道 schema（顶层字符串/数字字段），不追求通用性；档位串本身是纯 ASCII
// 十六进制/分隔符，字符串值只需处理 \" \\ \/ \n \t \r \b \f 与 \uXXXX 兜底。
bool ExtractString(const std::string &json, const char *key, std::string *out) {
  const std::string pattern = std::string("\"") + key + "\"";
  size_t pos = json.find(pattern);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + pattern.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                              json[pos] == '\n' || json[pos] == '\r')) {
    ++pos;
  }
  if (pos >= json.size() || json[pos] != '"') return false;
  ++pos;
  out->clear();
  while (pos < json.size()) {
    const char c = json[pos++];
    if (c == '"') return true;
    if (c != '\\') {
      out->push_back(c);
      continue;
    }
    if (pos >= json.size()) return false;
    const char esc = json[pos++];
    switch (esc) {
      case 'n': out->push_back('\n'); break;
      case 't': out->push_back('\t'); break;
      case 'r': out->push_back('\r'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case '/': out->push_back('/'); break;
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case 'u': {
        if (pos + 4 > json.size()) return false;
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
          const char h = json[pos + i];
          unsigned digit;
          if (h >= '0' && h <= '9') {
            digit = static_cast<unsigned>(h - '0');
          } else if (h >= 'a' && h <= 'f') {
            digit = static_cast<unsigned>(h - 'a' + 10);
          } else if (h >= 'A' && h <= 'F') {
            digit = static_cast<unsigned>(h - 'A' + 10);
          } else {
            return false;
          }
          code = (code << 4) | digit;
        }
        pos += 4;
        if (code < 0x80) {
          out->push_back(static_cast<char>(code));
        } else if (code < 0x800) {
          out->push_back(static_cast<char>(0xC0 | (code >> 6)));
          out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
          out->push_back(static_cast<char>(0xE0 | (code >> 12)));
          out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        break;
      }
      default: out->push_back(esc); break;
    }
  }
  return false;
}

bool ExtractInt64(const std::string &json, const char *key, int64_t *out) {
  const std::string pattern = std::string("\"") + key + "\"";
  size_t pos = json.find(pattern);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + pattern.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  bool negative = false;
  if (pos < json.size() && json[pos] == '-') {
    negative = true;
    ++pos;
  }
  int64_t value = 0;
  bool digits = false;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    value = value * 10 + (json[pos] - '0');
    digits = true;
    ++pos;
  }
  if (!digits) return false;
  *out = negative ? -value : value;
  return true;
}

}  // namespace

HostChannel::~HostChannel() { stop(); }

void HostChannel::start() {
  if (thread_.joinable()) return;
  thread_ = std::thread([this] { loop(); });
}

void HostChannel::stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
}

bool HostChannel::sendJson(const std::string &payload) {
  const int fd = fd_.load();
  if (fd < 0) return false;
  if (payload.size() > static_cast<size_t>(kHostFrameMax)) return false;
  uint8_t header[4] = {
      static_cast<uint8_t>((payload.size() >> 24) & 0xFF),
      static_cast<uint8_t>((payload.size() >> 16) & 0xFF),
      static_cast<uint8_t>((payload.size() >> 8) & 0xFF),
      static_cast<uint8_t>(payload.size() & 0xFF),
  };
  return WriteAll(fd, header, sizeof(header)) &&
         WriteAll(fd, payload.data(), payload.size());
}

void HostChannel::sendHello() {
  char frame[96];
  snprintf(frame, sizeof(frame),
           "{\"type\":\"hello\",\"role\":\"guest\",\"version\":%d}",
           kHostProtocolVersion);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "host channel hello version=%d",
                      kHostProtocolVersion);
  sendJson(frame);
}

void HostChannel::sendAck(int64_t seq, bool ok, const std::string &detail) {
  char frame[256];
  snprintf(frame, sizeof(frame),
           "{\"type\":\"scan_profile_ack\",\"seq\":%lld,\"ok\":%s,"
           "\"detail\":\"%s\"}",
           static_cast<long long>(seq), ok ? "true" : "false",
           EscapeJson(detail).c_str());
  sendJson(frame);
}

bool HostChannel::applyScanProfile(const std::string &profile,
                                   std::string *detail) {
  ScanModel model;
  std::string error;
  if (!ParseScanModel(profile, &model, &error)) {
    *detail = "profile rejected: " + error;
    return false;
  }
  if (!WriteScanModel(kScanProfilePath, model, &error)) {
    *detail = "profile write failed: " + error;
    return false;
  }
  char summary[96];
  snprintf(summary, sizeof(summary), "applied %zu access points",
           model.access_points.size());
  *detail = summary;
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "host scan profile applied: %zu APs first=%s",
                      model.access_points.size(),
                      model.access_points.empty()
                          ? ""
                          : model.access_points.front().ssid.c_str());
  return true;
}

void HostChannel::handleFrame(const std::string &frame) {
  std::string type;
  if (!ExtractString(frame, "type", &type)) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "host frame without type: %.120s", frame.c_str());
    return;
  }
  if (type == "scan_profile") {
    int64_t seq = 0;
    ExtractInt64(frame, "seq", &seq);
    std::string profile;
    if (!ExtractString(frame, "profile", &profile)) {
      sendAck(seq, false, "missing profile field");
      return;
    }
    std::string detail;
    const bool ok = applyScanProfile(profile, &detail);
    if (!ok) {
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "host scan profile not applied: %s", detail.c_str());
    }
    sendAck(seq, ok, detail);
    return;
  }
  if (type == "ping") {
    sendJson("{\"type\":\"pong\"}");
    return;
  }
  __android_log_print(ANDROID_LOG_WARN, kLogTag,
                      "unknown host frame type=%s", type.c_str());
}

void HostChannel::loop() {
  int idleMs = 0;
  bool loggedOnce = false;
  while (!stop_.load()) {
    const int fd = open(kHostChannelPath, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      // 设备节点由 virtio_console 晚创建（且宿主服务可能未就绪）：容忍 ENOENT 并重连
      if (!loggedOnce) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "host channel %s open failed: %s (retry %dms)",
                            kHostChannelPath, strerror(errno),
                            kHostReconnectDelayMs);
        loggedOnce = true;
      }
      sleepMilliseconds(kHostReconnectDelayMs);
      continue;
    }
    loggedOnce = false;
    // /dev/hvcN 是 tty：默认 ICANON/ECHO/OPOST，host→guest 的二进制帧（无换行符）
    // 被行缓冲吞掉，poll 只在「行结束」时才可读——实测 host 下发 scan_profile 永远
    // 收不到、无 ack、sysfs 不更新（对比数据面 hvc10 的 make_raw，控制面同样必须切
    // raw；-opost 防 0x0a 扩成 0x0d 0x0a，-echo 防回显灌回 host 侧帧流）。
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
      cfmakeraw(&t);
      tcsetattr(fd, TCSANOW, &t);
    }
    fd_.store(fd);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "host channel connected: %s",
                        kHostChannelPath);
    sendHello();
    idleMs = 0;
    while (!stop_.load()) {
      pollfd pfd = {};
      pfd.fd = fd;
      pfd.events = POLLIN;
      const int ready = poll(&pfd, 1, kPollSliceMs);
      if (ready < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (ready == 0) {
        idleMs += kPollSliceMs;
        if (idleMs >= kHostReadTimeoutMs) {
          idleMs = 0;
          sendJson("{\"type\":\"ping\"}");
        }
        continue;
      }
      idleMs = 0;
      if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) break;
      uint8_t header[4];
      if (!ReadAll(fd, header, sizeof(header))) break;
      const uint32_t length = (static_cast<uint32_t>(header[0]) << 24) |
                              (static_cast<uint32_t>(header[1]) << 16) |
                              (static_cast<uint32_t>(header[2]) << 8) |
                              static_cast<uint32_t>(header[3]);
      if (length == 0) continue;
      if (length > static_cast<uint32_t>(kHostFrameMax)) {
        // 失步（超过单帧上限）：丢弃连接重连
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "host frame too large (%u), reconnecting", length);
        break;
      }
      std::string frame(length, '\0');
      if (!ReadAll(fd, frame.data(), length)) break;
      handleFrame(frame);
    }
    close(fd);
    fd_.store(-1);
    if (!stop_.load()) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "host channel disconnected, retry in %dms",
                          kHostReconnectDelayMs);
      sleepMilliseconds(kHostReconnectDelayMs);
    }
  }
}

}  // namespace cdh_wifi
