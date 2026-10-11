// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <android/log.h>

extern "C" {
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_startThreadPool(void);
void ABinderProcess_joinThreadPool(void);
binder_status_t AServiceManager_addService(AIBinder *binder,
                                           const char *instance);
}

#include <aidl/android/hardware/wifi/BnWifi.h>
#include <aidl/android/hardware/wifi/BnWifiChip.h>
#include <aidl/android/hardware/wifi/BnWifiStaIface.h>

#include "../include/cdh_wifi_scan_model.h"
#include "cdh_wifi_host_channel.h"

#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace wifi = aidl::android::hardware::wifi;
using ndk::ScopedAStatus;

namespace {

constexpr char kServiceName[] = "android.hardware.wifi.IWifi/default";
constexpr char kInterfaceName[] = "wlan0";
constexpr char kSecondaryInterfaceName[] = "wlan1";
constexpr char kLogTag[] = "CdhWifiHal";
constexpr int32_t kMaximumScanBuckets = 8;
constexpr int32_t kMaximumScans = 64;
constexpr int32_t kMaximumScanCacheBytes = 32768;
constexpr int32_t kScanReportMask = 7;
constexpr int32_t kScanEachScan = 1;
constexpr int32_t kScanFullResults = 2;
constexpr int32_t kScanNoBatch = 4;
constexpr int32_t kStaFeatureSet =
    static_cast<int32_t>(
        wifi::IWifiStaIface::FeatureSetMask::BACKGROUND_SCAN) |
    static_cast<int32_t>(wifi::IWifiStaIface::FeatureSetMask::RSSI_MONITOR) |
    static_cast<int32_t>(wifi::IWifiStaIface::FeatureSetMask::STA_5G) |
    static_cast<int32_t>(
        wifi::IWifiStaIface::FeatureSetMask::CACHED_SCAN_DATA);
constexpr std::chrono::milliseconds kRssiPollInterval(250);

int64_t bootTimeMicroseconds() {
  timespec value = {};
  if (clock_gettime(CLOCK_BOOTTIME, &value) != 0) return 0;
  return static_cast<int64_t>(value.tv_sec) * 1000000LL +
         value.tv_nsec / 1000;
}

bool readScanModel(cdh_wifi::ScanModel *model) {
  std::string error;
  if (cdh_wifi::ReadScanModel(cdh_wifi::kScanModelPath, model, &error)) {
    return true;
  }
  __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                      "scan profile unavailable: %s", error.c_str());
  return false;
}

bool appendNetlinkAttribute(nlmsghdr *header, size_t capacity,
                            uint16_t type, const void *data, size_t length) {
  const size_t offset = NLMSG_ALIGN(header->nlmsg_len);
  const size_t attributeSize = NLA_HDRLEN + length;
  const size_t alignedSize = NLA_ALIGN(attributeSize);
  if (offset + alignedSize > capacity) return false;
  auto *attribute =
      reinterpret_cast<nlattr *>(reinterpret_cast<uint8_t *>(header) + offset);
  std::memset(attribute, 0, alignedSize);
  attribute->nla_type = type;
  attribute->nla_len = static_cast<uint16_t>(attributeSize);
  std::memcpy(reinterpret_cast<uint8_t *>(attribute) + NLA_HDRLEN, data,
              length);
  header->nlmsg_len = static_cast<uint32_t>(offset + alignedSize);
  return true;
}

bool sendNetlinkRequest(int fd, const nlmsghdr *header) {
  sockaddr_nl destination = {};
  destination.nl_family = AF_NETLINK;
  return sendto(fd, header, header->nlmsg_len, 0,
                reinterpret_cast<const sockaddr *>(&destination),
                sizeof(destination)) ==
         static_cast<ssize_t>(header->nlmsg_len);
}

template <typename Visitor>
void visitNetlinkAttributes(const void *data, size_t length, Visitor visitor) {
  size_t offset = 0;
  while (offset + NLA_HDRLEN <= length) {
    const auto *attribute = reinterpret_cast<const nlattr *>(
        reinterpret_cast<const uint8_t *>(data) + offset);
    if (attribute->nla_len < NLA_HDRLEN ||
        attribute->nla_len > length - offset) {
      return;
    }
    visitor(attribute, reinterpret_cast<const uint8_t *>(attribute) +
                           NLA_HDRLEN,
            attribute->nla_len - NLA_HDRLEN);
    const size_t step = NLA_ALIGN(attribute->nla_len);
    if (step == 0 || step > length - offset) return;
    offset += step;
  }
}

template <typename Visitor>
void visitNetlinkMessages(uint8_t *data, size_t length, Visitor visitor) {
  size_t offset = 0;
  while (length - offset >= sizeof(nlmsghdr)) {
    auto *message = reinterpret_cast<nlmsghdr *>(data + offset);
    const size_t messageLength = message->nlmsg_len;
    if (messageLength < sizeof(nlmsghdr) ||
        messageLength > length - offset) {
      return;
    }
    if (!visitor(message)) return;
    const size_t step = NLMSG_ALIGN(messageLength);
    if (step < messageLength || step > length - offset) return;
    offset += step;
  }
}

bool resolveNl80211Family(int fd, uint16_t *familyId) {
  alignas(nlmsghdr) std::array<uint8_t, 256> requestBytes{};
  auto *request = reinterpret_cast<nlmsghdr *>(requestBytes.data());
  request->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
  request->nlmsg_type = GENL_ID_CTRL;
  request->nlmsg_flags = NLM_F_REQUEST;
  request->nlmsg_seq = 1;
  auto *generic = reinterpret_cast<genlmsghdr *>(NLMSG_DATA(request));
  generic->cmd = CTRL_CMD_GETFAMILY;
  generic->version = 1;
  constexpr char familyName[] = "nl80211";
  if (!appendNetlinkAttribute(request, requestBytes.size(),
                              CTRL_ATTR_FAMILY_NAME, familyName,
                              sizeof(familyName)) ||
      !sendNetlinkRequest(fd, request)) {
    return false;
  }

  alignas(nlmsghdr) std::array<uint8_t, 4096> response{};
  const ssize_t received = recv(fd, response.data(), response.size(), 0);
  if (received <= 0) return false;
  bool found = false;
  visitNetlinkMessages(response.data(), static_cast<size_t>(received),
                       [&](nlmsghdr *message) {
    if (message->nlmsg_type == NLMSG_ERROR ||
        message->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN)) {
      return true;
    }
    const auto *reply =
        reinterpret_cast<const genlmsghdr *>(NLMSG_DATA(message));
    const size_t payloadLength =
        message->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
    visitNetlinkAttributes(
        reinterpret_cast<const uint8_t *>(reply) + GENL_HDRLEN, payloadLength,
        [&](const nlattr *attribute, const uint8_t *payload, size_t length) {
          if ((attribute->nla_type & NLA_TYPE_MASK) ==
                  CTRL_ATTR_FAMILY_ID &&
              length >= sizeof(*familyId)) {
            std::memcpy(familyId, payload, sizeof(*familyId));
            found = true;
          }
        });
    return !found;
  });
  return found;
}

bool parseAssociatedBssid(const nlattr *bssAttribute,
                          std::array<uint8_t, 6> *bssid) {
  const uint8_t *bssPayload =
      reinterpret_cast<const uint8_t *>(bssAttribute) + NLA_HDRLEN;
  const size_t bssLength = bssAttribute->nla_len - NLA_HDRLEN;
  uint32_t status = 0;
  bool hasStatus = false;
  bool hasBssid = false;
  visitNetlinkAttributes(
      bssPayload, bssLength,
      [&](const nlattr *attribute, const uint8_t *payload, size_t length) {
        const uint16_t type = attribute->nla_type & NLA_TYPE_MASK;
        if (type == NL80211_BSS_STATUS && length >= sizeof(status)) {
          std::memcpy(&status, payload, sizeof(status));
          hasStatus = true;
        } else if (type == NL80211_BSS_BSSID &&
                   length == bssid->size()) {
          std::copy_n(payload, bssid->size(), bssid->begin());
          hasBssid = true;
        }
      });
  return hasStatus && hasBssid &&
         status == NL80211_BSS_STATUS_ASSOCIATED;
}

bool readConnectedBssid(const std::string &interfaceName,
                        std::array<uint8_t, 6> *bssid) {
  const unsigned int interfaceIndex = if_nametoindex(interfaceName.c_str());
  if (interfaceIndex == 0) return false;
  const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
  if (fd < 0) return false;
  sockaddr_nl local = {};
  local.nl_family = AF_NETLINK;
  timeval timeout = {};
  timeout.tv_usec = 200000;
  if (bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) !=
          0 ||
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) !=
          0) {
    close(fd);
    return false;
  }

  uint16_t familyId = 0;
  if (!resolveNl80211Family(fd, &familyId)) {
    close(fd);
    return false;
  }
  alignas(nlmsghdr) std::array<uint8_t, 256> requestBytes{};
  auto *request = reinterpret_cast<nlmsghdr *>(requestBytes.data());
  request->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
  request->nlmsg_type = familyId;
  request->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  request->nlmsg_seq = 2;
  auto *generic = reinterpret_cast<genlmsghdr *>(NLMSG_DATA(request));
  generic->cmd = NL80211_CMD_GET_SCAN;
  const uint32_t ifindex = interfaceIndex;
  if (!appendNetlinkAttribute(request, requestBytes.size(),
                              NL80211_ATTR_IFINDEX, &ifindex,
                              sizeof(ifindex)) ||
      !sendNetlinkRequest(fd, request)) {
    close(fd);
    return false;
  }

  alignas(nlmsghdr) std::array<uint8_t, 32768> response{};
  for (;;) {
    const ssize_t received = recv(fd, response.data(), response.size(), 0);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) {
      close(fd);
      return false;
    }
    bool done = false;
    bool associated = false;
    visitNetlinkMessages(response.data(), static_cast<size_t>(received),
                         [&](nlmsghdr *message) {
      if (message->nlmsg_type == NLMSG_DONE) {
        done = true;
        return false;
      }
      if (message->nlmsg_type == NLMSG_ERROR ||
          message->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN)) {
        return true;
      }
      const auto *reply =
          reinterpret_cast<const genlmsghdr *>(NLMSG_DATA(message));
      const size_t payloadLength =
          message->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
      visitNetlinkAttributes(
          reinterpret_cast<const uint8_t *>(reply) + GENL_HDRLEN,
          payloadLength,
          [&](const nlattr *attribute, const uint8_t *, size_t) {
            if ((attribute->nla_type & NLA_TYPE_MASK) == NL80211_ATTR_BSS &&
                parseAssociatedBssid(attribute, bssid)) {
              associated = true;
            }
          });
      return !associated;
    });
    if (associated) {
      close(fd);
      const bool allZero =
          std::all_of(bssid->begin(), bssid->end(), [](uint8_t value) {
            return value == 0;
          });
      return !allZero && ((*bssid)[0] & 1U) == 0;
    }
    if (done) {
      close(fd);
      return false;
    }
  }
}

const cdh_wifi::AccessPoint *readConnectedAccessPoint(
    const std::string &interfaceName, const cdh_wifi::ScanModel &model) {
  std::array<uint8_t, 6> bssid{};
  if (!readConnectedBssid(interfaceName, &bssid)) return nullptr;
  return cdh_wifi::FindAccessPointByBssid(model, bssid);
}

size_t scanDataBytes(const wifi::StaScanData &data) {
  size_t total = sizeof(data);
  for (const wifi::StaScanResult &result : data.results) {
    total += sizeof(result) + result.ssid.size();
    for (const wifi::WifiInformationElement &element :
         result.informationElements) {
      total += sizeof(element) + element.data.size();
    }
  }
  return total;
}

wifi::WifiInformationElement rsnElement(cdh_wifi::Security security) {
  wifi::WifiInformationElement element;
  element.id = 48;
  element.data = {
      0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
      0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac,
      security == cdh_wifi::Security::kWpa3 ? uint8_t{0x08} : uint8_t{0x02},
      security == cdh_wifi::Security::kWpa3 ? uint8_t{0xc0} : uint8_t{0x00},
      0x00,
  };
  return element;
}

wifi::StaScanResult scanResult(const cdh_wifi::AccessPoint &accessPoint,
                               int64_t timestamp) {
  wifi::StaScanResult result;
  result.timeStampInUs = timestamp;
  result.ssid.assign(accessPoint.ssid.begin(), accessPoint.ssid.end());
  result.bssid = accessPoint.bssid;
  result.rssi = accessPoint.rssi_dbm;
  result.frequency = accessPoint.frequency_mhz;
  result.beaconPeriodInMs = 100;
  result.capability = accessPoint.security == cdh_wifi::Security::kOpen
                          ? 0x0001
                          : 0x0011;
  if (accessPoint.security != cdh_wifi::Security::kOpen) {
    result.informationElements = {rsnElement(accessPoint.security)};
  }
  return result;
}

wifi::CachedScanResult cachedResult(const cdh_wifi::AccessPoint &accessPoint,
                                    int64_t timestamp) {
  wifi::CachedScanResult result;
  result.timeStampInUs = timestamp;
  result.ssid.assign(accessPoint.ssid.begin(), accessPoint.ssid.end());
  result.bssid = accessPoint.bssid;
  result.rssiDbm = accessPoint.rssi_dbm;
  result.frequencyMhz = accessPoint.frequency_mhz;
  result.channelWidthMhz = wifi::WifiChannelWidthInMhz::WIDTH_20;
  result.preambleType = accessPoint.frequency_mhz < 3000
                            ? wifi::WifiRatePreamble::HT
                            : wifi::WifiRatePreamble::VHT;
  return result;
}

ScopedAStatus wifiError(wifi::WifiStatusCode code) {
  return ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(code));
}

std::array<uint8_t, 6> readMacAddress(const std::string &interfaceName) {
  std::array<uint8_t, 6> mac = {0x02, 0x00, 0x00, 0x44, 0x55, 0x66};
  std::ifstream input(std::string("/sys/class/net/") + interfaceName +
                      "/address");
  unsigned int octets[6] = {};
  char separator = 0;
  for (size_t i = 0; input && i < mac.size(); ++i) {
    input >> std::hex >> octets[i];
    if (i + 1 < mac.size()) input >> separator;
  }
  if (input) {
    for (size_t i = 0; i < mac.size(); ++i) {
      mac[i] = static_cast<uint8_t>(octets[i]);
    }
  }
  return mac;
}

bool setMacAddress(const std::string &interfaceName,
                   const std::array<uint8_t, 6> &mac) {
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  ifreq request = {};
  std::strncpy(request.ifr_name, interfaceName.c_str(), IFNAMSIZ - 1);

  if (ioctl(fd, SIOCGIFHWADDR, &request) == 0 &&
      std::memcmp(request.ifr_hwaddr.sa_data, mac.data(), mac.size()) == 0) {
    close(fd);
    return true;
  }

  request = {};
  std::strncpy(request.ifr_name, interfaceName.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFFLAGS, &request) != 0) {
    close(fd);
    return false;
  }
  const short originalFlags = request.ifr_flags;
  if ((originalFlags & IFF_UP) != 0) {
    request.ifr_flags = static_cast<short>(originalFlags & ~IFF_UP);
    if (ioctl(fd, SIOCSIFFLAGS, &request) != 0) {
      close(fd);
      return false;
    }
  }

  request = {};
  std::strncpy(request.ifr_name, interfaceName.c_str(), IFNAMSIZ - 1);
  request.ifr_hwaddr.sa_family = ARPHRD_ETHER;
  std::memcpy(request.ifr_hwaddr.sa_data, mac.data(), mac.size());
  bool ok = ioctl(fd, SIOCSIFHWADDR, &request) == 0;
  if ((originalFlags & IFF_UP) != 0) {
    request = {};
    std::strncpy(request.ifr_name, interfaceName.c_str(), IFNAMSIZ - 1);
    request.ifr_flags = originalFlags;
    ok = ioctl(fd, SIOCSIFFLAGS, &request) == 0 && ok;
  }
  close(fd);
  return ok;
}

class WifiSta final : public wifi::IWifiStaIfaceDefault {
 public:
  explicit WifiSta(std::string name) : name_(std::move(name)) {}

  ~WifiSta() override { shutdown(); }

  void shutdown() {
    stopScanWorker();
    stopRssiWorker();
    std::lock_guard<std::mutex> lock(state_mutex_);
    callback_.reset();
  }

  ScopedAStatus getName(std::string *out) override {
    *out = name_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getFeatureSet(int32_t *out) override {
    *out = kStaFeatureSet;
    return ScopedAStatus::ok();
  }

  ScopedAStatus registerEventCallback(
      const std::shared_ptr<wifi::IWifiStaIfaceEventCallback> &callback) override {
    if (!callback) return wifiError(wifi::WifiStatusCode::ERROR_INVALID_ARGS);
    std::lock_guard<std::mutex> lock(state_mutex_);
    callback_ = callback;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getFactoryMacAddress(std::array<uint8_t, 6> *out) override {
    *out = readMacAddress(name_);
    return ScopedAStatus::ok();
  }

  ScopedAStatus setMacAddress(const std::array<uint8_t, 6> &mac) override {
    return ::setMacAddress(name_, mac)
               ? ScopedAStatus::ok()
               : wifiError(wifi::WifiStatusCode::ERROR_UNKNOWN);
  }

  ScopedAStatus getApfPacketFilterCapabilities(
      wifi::StaApfPacketFilterCapabilities *out) override {
    *out = {};
    return ScopedAStatus::ok();
  }

  ScopedAStatus getBackgroundScanCapabilities(
      wifi::StaBackgroundScanCapabilities *out) override {
    out->maxCacheSize = kMaximumScanCacheBytes;
    out->maxBuckets = kMaximumScanBuckets;
    out->maxApCachePerScan = cdh_wifi::kMaximumAccessPoints;
    out->maxReportingThreshold = kMaximumScans;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getRoamingCapabilities(
      wifi::StaRoamingCapabilities *out) override {
    *out = {};
    return ScopedAStatus::ok();
  }

  ScopedAStatus configureRoaming(const wifi::StaRoamingConfig &) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus getDebugRxPacketFates(
      std::vector<wifi::WifiDebugRxPacketFateReport> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }

  ScopedAStatus getDebugTxPacketFates(
      std::vector<wifi::WifiDebugTxPacketFateReport> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }

  ScopedAStatus getLinkLayerStats(wifi::StaLinkLayerStats *out) override {
    *out = {};
    return ScopedAStatus::ok();
  }

  ScopedAStatus installApfPacketFilter(
      const std::vector<uint8_t> &) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus readApfPacketFilterData(
      std::vector<uint8_t> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }

  ScopedAStatus setRoamingState(wifi::StaRoamingState) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus startBackgroundScan(
      int32_t commandId,
      const wifi::StaBackgroundScanParameters &parameters) override {
    if (!validScanParameters(commandId, parameters)) {
      return wifiError(wifi::WifiStatusCode::ERROR_INVALID_ARGS);
    }
    cdh_wifi::ScanModel model;
    if (!readScanModel(&model)) {
      return wifiError(wifi::WifiStatusCode::ERROR_NOT_AVAILABLE);
    }
    reapFinishedScanWorker();
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (scan_active_ || scan_thread_.joinable()) {
      return wifiError(wifi::WifiStatusCode::ERROR_NOT_AVAILABLE);
    }
    if (!callback_) {
      return wifiError(wifi::WifiStatusCode::ERROR_NOT_AVAILABLE);
    }
    scan_command_id_ = commandId;
    scan_parameters_ = parameters;
    scan_active_ = true;
    try {
      scan_thread_ = std::thread(&WifiSta::scanWorker, this);
    } catch (const std::system_error &) {
      scan_active_ = false;
      return wifiError(wifi::WifiStatusCode::ERROR_UNKNOWN);
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus stopBackgroundScan(int32_t commandId) override {
    return stopScanWorker(commandId)
               ? ScopedAStatus::ok()
               : wifiError(wifi::WifiStatusCode::ERROR_INVALID_ARGS);
  }

  ScopedAStatus startDebugPacketFateMonitoring() override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus startRssiMonitoring(int32_t commandId, int32_t maximumRssi,
                                    int32_t minimumRssi) override {
    if (commandId <= 0 || maximumRssi < -126 || maximumRssi > -1 ||
        minimumRssi < -127 || minimumRssi > -2 ||
        maximumRssi <= minimumRssi) {
      return wifiError(wifi::WifiStatusCode::ERROR_INVALID_ARGS);
    }
    cdh_wifi::ScanModel model;
    if (!readScanModel(&model) ||
        readConnectedAccessPoint(name_, model) == nullptr) {
      return wifiError(wifi::WifiStatusCode::ERROR_NOT_AVAILABLE);
    }
    reapFinishedRssiWorker();
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (rssi_active_ || rssi_thread_.joinable()) {
      return wifiError(wifi::WifiStatusCode::ERROR_NOT_AVAILABLE);
    }
    if (!callback_) {
      return wifiError(wifi::WifiStatusCode::ERROR_NOT_AVAILABLE);
    }
    rssi_command_id_ = commandId;
    maximum_rssi_ = maximumRssi;
    minimum_rssi_ = minimumRssi;
    rssi_zone_ = 0;
    rssi_active_ = true;
    try {
      rssi_thread_ = std::thread(&WifiSta::rssiWorker, this);
    } catch (const std::system_error &) {
      rssi_active_ = false;
      return wifiError(wifi::WifiStatusCode::ERROR_UNKNOWN);
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus stopRssiMonitoring(int32_t commandId) override {
    return stopRssiWorker(commandId)
               ? ScopedAStatus::ok()
               : wifiError(wifi::WifiStatusCode::ERROR_INVALID_ARGS);
  }

  ScopedAStatus startSendingKeepAlivePackets(
      int32_t, const std::vector<uint8_t> &, char16_t,
      const std::array<uint8_t, 6> &, const std::array<uint8_t, 6> &,
      int32_t) override {
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus stopSendingKeepAlivePackets(int32_t) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus getCachedScanData(wifi::CachedScanData *out) override {
    cdh_wifi::ScanModel model;
    if (!readScanModel(&model)) {
      *out = {};
      return wifiError(wifi::WifiStatusCode::ERROR_NOT_AVAILABLE);
    }
    const int64_t timestamp = bootTimeMicroseconds();
    out->scannedFrequenciesMhz.clear();
    out->cachedScanResults.clear();
    for (const cdh_wifi::AccessPoint &accessPoint : model.access_points) {
      if (std::find(out->scannedFrequenciesMhz.begin(),
                    out->scannedFrequenciesMhz.end(),
                    accessPoint.frequency_mhz) ==
          out->scannedFrequenciesMhz.end()) {
        out->scannedFrequenciesMhz.push_back(accessPoint.frequency_mhz);
      }
      out->cachedScanResults.push_back(cachedResult(accessPoint, timestamp));
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus twtGetCapabilities(wifi::TwtCapabilities *out) override {
    *out = {};
    return ScopedAStatus::ok();
  }

  ScopedAStatus twtSessionSetup(int32_t, const wifi::TwtRequest &) override {
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus twtSessionUpdate(int32_t, int32_t,
                                 const wifi::TwtRequest &) override {
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus twtSessionSuspend(int32_t, int32_t) override {
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus twtSessionResume(int32_t, int32_t) override {
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus twtSessionTeardown(int32_t, int32_t) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus twtSessionGetStats(int32_t, int32_t) override {
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus enableNdOffload(bool) override { return ScopedAStatus::ok(); }
  ScopedAStatus setScanMode(bool) override { return ScopedAStatus::ok(); }
  ScopedAStatus setDtimMultiplier(int32_t) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus enableLinkLayerStatsCollection(bool) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus disableLinkLayerStatsCollection() override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus getInterfaceVersion(int32_t *out) override {
    *out = wifi::IWifiStaIface::version;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getInterfaceHash(std::string *out) override {
    *out = wifi::IWifiStaIface::hash;
    return ScopedAStatus::ok();
  }

 private:
  static bool validScanParameters(
      int32_t commandId,
      const wifi::StaBackgroundScanParameters &parameters) {
    if (commandId <= 0 || parameters.basePeriodInMs < 100 ||
        parameters.basePeriodInMs > 3600000 || parameters.maxApPerScan <= 0 ||
        parameters.maxApPerScan >
            static_cast<int32_t>(cdh_wifi::kMaximumAccessPoints) ||
        parameters.reportThresholdPercent < 0 ||
        parameters.reportThresholdPercent > 100 ||
        parameters.reportThresholdNumScans < 0 ||
        parameters.reportThresholdNumScans > kMaximumScans ||
        parameters.buckets.empty() ||
        parameters.buckets.size() > kMaximumScanBuckets) {
      return false;
    }
    std::array<bool, 32> bucketIds{};
    for (const auto &bucket : parameters.buckets) {
      const int32_t band = static_cast<int32_t>(bucket.band);
      if (bucket.bucketIdx < 0 || bucket.bucketIdx >= 32 ||
          bucketIds[bucket.bucketIdx] || bucket.periodInMs <= 0 ||
          bucket.periodInMs % parameters.basePeriodInMs != 0 ||
          (bucket.eventReportScheme & ~kScanReportMask) != 0 ||
          bucket.exponentialMaxPeriodInMs != 0 ||
          bucket.exponentialBase != 0 || bucket.exponentialStepCount != 0) {
        return false;
      }
      bucketIds[bucket.bucketIdx] = true;
      if (bucket.band == wifi::WifiBand::BAND_UNSPECIFIED) {
        if (bucket.frequencies.empty()) return false;
        for (const int32_t frequency : bucket.frequencies) {
          if (!cdh_wifi::IsFrequencySupported(frequency)) return false;
        }
      } else if (!cdh_wifi::IsBandSupported(band) ||
                 !bucket.frequencies.empty()) {
        return false;
      }
    }
    return true;
  }

  static bool bucketMatches(
      const wifi::StaBackgroundScanBucketParameters &bucket,
      const cdh_wifi::AccessPoint &accessPoint) {
    if (bucket.band == wifi::WifiBand::BAND_UNSPECIFIED) {
      return std::find(bucket.frequencies.begin(), bucket.frequencies.end(),
                       accessPoint.frequency_mhz) != bucket.frequencies.end();
    }
    return cdh_wifi::FrequencyMatchesBand(
        accessPoint.frequency_mhz, static_cast<int32_t>(bucket.band));
  }

  static void reportScanCache(
      int32_t commandId,
      const std::shared_ptr<wifi::IWifiStaIfaceEventCallback> &callback,
      std::vector<wifi::StaScanData> *cache, size_t *cacheBytes) {
    if (cache->empty()) return;
    if (!callback->onBackgroundScanResults(commandId, *cache).isOk()) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
                          "background scan callback failed");
    }
    cache->clear();
    *cacheBytes = 0;
  }

  static bool emitScan(
      int32_t commandId,
      const wifi::StaBackgroundScanParameters &parameters,
      uint64_t iteration,
      const std::shared_ptr<wifi::IWifiStaIfaceEventCallback> &callback,
      std::vector<wifi::StaScanData> *cache, size_t *cacheBytes) {
    cdh_wifi::ScanModel model;
    if (!readScanModel(&model)) {
      if (!callback->onBackgroundScanFailure(commandId).isOk()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "background scan failure callback failed");
      }
      return false;
    }
    const int64_t timestamp = bootTimeMicroseconds();
    uint32_t bucketMask = 0;
    bool batchEnabled = false;
    bool reportEachScan = false;
    std::vector<wifi::StaScanResult> batchResults;
    std::vector<wifi::StaScanResult> fullResults;
    for (const auto &bucket : parameters.buckets) {
      if (!cdh_wifi::IsScanDue(iteration, parameters.basePeriodInMs,
                               bucket.periodInMs)) {
        continue;
      }
      bucketMask |= uint32_t{1} << bucket.bucketIdx;
      if ((bucket.eventReportScheme & kScanNoBatch) == 0) {
        batchEnabled = true;
        reportEachScan =
            reportEachScan ||
            (bucket.eventReportScheme & kScanEachScan) != 0;
      }
    }
    if (bucketMask == 0) return true;
    for (const cdh_wifi::AccessPoint &accessPoint : model.access_points) {
      bool addToBatch = false;
      bool addFullResult = false;
      for (const auto &bucket : parameters.buckets) {
        if ((bucketMask & (uint32_t{1} << bucket.bucketIdx)) != 0 &&
            bucketMatches(bucket, accessPoint)) {
          addToBatch = addToBatch ||
                       (bucket.eventReportScheme & kScanNoBatch) == 0;
          addFullResult = addFullResult ||
                          (bucket.eventReportScheme & kScanFullResults) != 0;
        }
      }
      if (addToBatch) {
        batchResults.push_back(scanResult(accessPoint, timestamp));
      }
      if (addFullResult) {
        fullResults.push_back(scanResult(accessPoint, timestamp));
      }
    }
    const auto strongestFirst = [](const wifi::StaScanResult &left,
                                   const wifi::StaScanResult &right) {
      return left.rssi > right.rssi;
    };
    std::sort(batchResults.begin(), batchResults.end(), strongestFirst);
    std::sort(fullResults.begin(), fullResults.end(), strongestFirst);
    const size_t resultLimit = static_cast<size_t>(parameters.maxApPerScan);
    if (batchResults.size() > resultLimit) batchResults.resize(resultLimit);
    if (fullResults.size() > resultLimit) fullResults.resize(resultLimit);
    const int32_t bucketsScanned = static_cast<int32_t>(bucketMask);
    for (const wifi::StaScanResult &result : fullResults) {
      if (!callback
               ->onBackgroundFullScanResult(commandId, bucketsScanned, result)
               .isOk()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "background full scan callback failed");
      }
    }
    if (!batchEnabled) return true;
    wifi::StaScanData data;
    data.flags = 0;
    data.bucketsScanned = bucketsScanned;
    data.results = std::move(batchResults);
    const size_t dataBytes = scanDataBytes(data);
    if (!cache->empty() && *cacheBytes + dataBytes > kMaximumScanCacheBytes) {
      reportScanCache(commandId, callback, cache, cacheBytes);
    }
    *cacheBytes += dataBytes;
    cache->push_back(std::move(data));
    if (cdh_wifi::ShouldReportBatch(
            cache->size(), *cacheBytes, kMaximumScanCacheBytes,
            parameters.reportThresholdPercent,
            parameters.reportThresholdNumScans, reportEachScan)) {
      reportScanCache(commandId, callback, cache, cacheBytes);
    }
    return true;
  }

  void scanWorker() {
    uint64_t iteration = 0;
    size_t cacheBytes = 0;
    std::vector<wifi::StaScanData> cache;
    std::unique_lock<std::mutex> lock(state_mutex_);
    while (scan_active_) {
      const int32_t commandId = scan_command_id_;
      const wifi::StaBackgroundScanParameters parameters = scan_parameters_;
      const auto callback = callback_;
      lock.unlock();
      const bool ok =
          callback &&
          emitScan(commandId, parameters, iteration, callback, &cache,
                   &cacheBytes);
      lock.lock();
      if (!ok) {
        scan_active_ = false;
        break;
      }
      ++iteration;
      const auto period =
          std::chrono::milliseconds(scan_parameters_.basePeriodInMs);
      if (scan_condition_.wait_for(lock, period,
                                   [this] { return !scan_active_; })) {
        break;
      }
    }
  }

  void reapFinishedScanWorker() {
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!scan_active_ && scan_thread_.joinable()) {
        worker = std::move(scan_thread_);
      }
    }
    if (worker.joinable()) worker.join();
  }

  bool stopScanWorker(int32_t commandId = 0) {
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (commandId != 0 && commandId != scan_command_id_) {
        return false;
      }
      if (commandId != 0 && !scan_active_ && !scan_thread_.joinable()) {
        return false;
      }
      scan_active_ = false;
      scan_condition_.notify_all();
      if (scan_thread_.joinable()) worker = std::move(scan_thread_);
    }
    if (worker.joinable()) worker.join();
    return true;
  }

  void rssiWorker() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    while (rssi_active_) {
      if (rssi_condition_.wait_for(lock, kRssiPollInterval,
                                   [this] { return !rssi_active_; })) {
        break;
      }
      const int32_t commandId = rssi_command_id_;
      const int32_t maximumRssi = maximum_rssi_;
      const int32_t minimumRssi = minimum_rssi_;
      const auto callback = callback_;
      lock.unlock();

      cdh_wifi::ScanModel model;
      const cdh_wifi::AccessPoint *connected = nullptr;
      if (readScanModel(&model)) {
        connected = readConnectedAccessPoint(name_, model);
      }
      if (connected == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "RSSI monitoring stopped: link unavailable");
        lock.lock();
        rssi_active_ = false;
        break;
      }
      const cdh_wifi::AccessPoint accessPoint = *connected;
      const int zone = accessPoint.rssi_dbm < minimumRssi
                           ? -1
                           : (accessPoint.rssi_dbm > maximumRssi ? 1 : 0);
      lock.lock();
      if (!rssi_active_) break;
      const bool notify = zone != 0 && zone != rssi_zone_;
      rssi_zone_ = zone;
      lock.unlock();
      if (notify && callback &&
          !callback
               ->onRssiThresholdBreached(commandId, accessPoint.bssid,
                                          accessPoint.rssi_dbm)
               .isOk()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "RSSI threshold callback failed");
      }
      lock.lock();
    }
  }

  void reapFinishedRssiWorker() {
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!rssi_active_ && rssi_thread_.joinable()) {
        worker = std::move(rssi_thread_);
      }
    }
    if (worker.joinable()) worker.join();
  }

  bool stopRssiWorker(int32_t commandId = 0) {
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (commandId != 0 && commandId != rssi_command_id_) {
        return false;
      }
      if (commandId != 0 && !rssi_active_ && !rssi_thread_.joinable()) {
        return false;
      }
      rssi_active_ = false;
      rssi_condition_.notify_all();
      if (rssi_thread_.joinable()) worker = std::move(rssi_thread_);
    }
    if (worker.joinable()) worker.join();
    return true;
  }

  std::string name_;
  std::mutex state_mutex_;
  std::condition_variable scan_condition_;
  std::condition_variable rssi_condition_;
  std::thread scan_thread_;
  std::thread rssi_thread_;
  std::shared_ptr<wifi::IWifiStaIfaceEventCallback> callback_;
  wifi::StaBackgroundScanParameters scan_parameters_;
  int32_t scan_command_id_ = 0;
  int32_t rssi_command_id_ = 0;
  int32_t maximum_rssi_ = -1;
  int32_t minimum_rssi_ = -127;
  int rssi_zone_ = 0;
  bool scan_active_ = false;
  bool rssi_active_ = false;
};

class WifiChip final : public wifi::IWifiChipDefault {
 public:
  WifiChip() {
    sta_impl_[0] = ndk::SharedRefBase::make<WifiSta>(kInterfaceName);
    sta_impl_[1] =
        ndk::SharedRefBase::make<WifiSta>(kSecondaryInterfaceName);
    sta_[0] = ndk::SharedRefBase::make<wifi::IWifiStaIfaceDelegator>(
        sta_impl_[0]);
    sta_[1] = ndk::SharedRefBase::make<wifi::IWifiStaIfaceDelegator>(
        sta_impl_[1]);
  }

  ScopedAStatus getId(int32_t *out) override {
    *out = 0;
    return ScopedAStatus::ok();
  }

  void resetStaInterface() {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "resetStaInterface created=%d,%d", sta_created_[0],
                        sta_created_[1]);
    for (const auto &implementation : sta_impl_) implementation->shutdown();
    sta_created_.fill(false);
  }

  ScopedAStatus getFeatureSet(int32_t *out) override {
    *out = 0;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getAvailableModes(
      std::vector<wifi::IWifiChip::ChipMode> *out) override {
    wifi::IWifiChip::ChipConcurrencyCombinationLimit limit;
    limit.types = {wifi::IfaceConcurrencyType::STA};
    limit.maxIfaces = 2;
    wifi::IWifiChip::ChipConcurrencyCombination combination;
    combination.limits = {limit};
    wifi::IWifiChip::ChipMode mode;
    mode.id = 0;
    mode.availableCombinations = {combination};
    *out = {mode};
    return ScopedAStatus::ok();
  }

  ScopedAStatus configureChip(int32_t mode) override {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "configureChip mode=%d created=%d", mode,
                        sta_created_[0] || sta_created_[1]);
    if (mode != 0) return wifiError(wifi::WifiStatusCode::ERROR_INVALID_ARGS);
    mode_ = mode;
    if (callback_) callback_->onChipReconfigured(mode);
    return ScopedAStatus::ok();
  }

  ScopedAStatus getMode(int32_t *out) override {
    *out = mode_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus registerEventCallback(
      const std::shared_ptr<wifi::IWifiChipEventCallback> &callback) override {
    callback_ = callback;
    return ScopedAStatus::ok();
  }

  ScopedAStatus createStaIface(
      std::shared_ptr<wifi::IWifiStaIface> *out) override {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "createStaIface created=%d,%d", sta_created_[0],
                        sta_created_[1]);
    for (size_t index = 0; index < sta_.size(); ++index) {
      if (sta_created_[index]) continue;
      sta_created_[index] = true;
      *out = sta_[index];
      if (callback_) {
        callback_->onIfaceAdded(wifi::IfaceType::STA, interfaceNames()[index]);
      }
      return ScopedAStatus::ok();
    }
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_AVAILABLE);
  }

  ScopedAStatus getStaIfaceNames(std::vector<std::string> *out) override {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "getStaIfaceNames created=%d,%d", sta_created_[0],
                        sta_created_[1]);
    out->clear();
    for (size_t index = 0; index < sta_.size(); ++index) {
      if (sta_created_[index]) out->push_back(interfaceNames()[index]);
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus getStaIface(const std::string &name,
                            std::shared_ptr<wifi::IWifiStaIface> *out) override {
    for (size_t index = 0; index < sta_.size(); ++index) {
      if (name == interfaceNames()[index] && sta_created_[index]) {
        *out = sta_[index];
        return ScopedAStatus::ok();
      }
    }
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_WIFI_IFACE_INVALID);
  }

  ScopedAStatus removeStaIface(const std::string &name) override {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "removeStaIface name=%s created=%d,%d", name.c_str(),
                        sta_created_[0], sta_created_[1]);
    for (size_t index = 0; index < sta_.size(); ++index) {
      if (name != interfaceNames()[index] || !sta_created_[index]) continue;
      sta_impl_[index]->shutdown();
      sta_created_[index] = false;
      if (callback_) callback_->onIfaceRemoved(wifi::IfaceType::STA, name);
      return ScopedAStatus::ok();
    }
    return wifiError(wifi::WifiStatusCode::ERROR_WIFI_IFACE_INVALID);
  }

  ScopedAStatus getApIfaceNames(std::vector<std::string> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }

  ScopedAStatus createApIface(std::shared_ptr<wifi::IWifiApIface> *out) override {
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus createBridgedApIface(
      std::shared_ptr<wifi::IWifiApIface> *out) override {
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus createNanIface(
      std::shared_ptr<wifi::IWifiNanIface> *out) override {
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus createP2pIface(
      std::shared_ptr<wifi::IWifiP2pIface> *out) override {
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus createRttController(
      const std::shared_ptr<wifi::IWifiStaIface> &,
      std::shared_ptr<wifi::IWifiRttController> *out) override {
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus getApIface(
      const std::string &, std::shared_ptr<wifi::IWifiApIface> *out) override {
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_WIFI_IFACE_INVALID);
  }

  ScopedAStatus getNanIface(
      const std::string &, std::shared_ptr<wifi::IWifiNanIface> *out) override {
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_WIFI_IFACE_INVALID);
  }

  ScopedAStatus getP2pIface(
      const std::string &, std::shared_ptr<wifi::IWifiP2pIface> *out) override {
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_WIFI_IFACE_INVALID);
  }
  ScopedAStatus getNanIfaceNames(std::vector<std::string> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }
  ScopedAStatus getP2pIfaceNames(std::vector<std::string> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }
  ScopedAStatus getUsableChannels(
      wifi::WifiBand, int32_t, int32_t,
      std::vector<wifi::WifiUsableChannel> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }
  ScopedAStatus getSupportedRadioCombinations(
      std::vector<wifi::WifiRadioCombination> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }
  ScopedAStatus getWifiChipCapabilities(
      wifi::WifiChipCapabilities *out) override {
    *out = {};
    return ScopedAStatus::ok();
  }
  ScopedAStatus getDebugHostWakeReasonStats(
      wifi::WifiDebugHostWakeReasonStats *out) override {
    *out = {};
    return ScopedAStatus::ok();
  }
  ScopedAStatus getDebugRingBuffersStatus(
      std::vector<wifi::WifiDebugRingBufferStatus> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }
  ScopedAStatus requestChipDebugInfo(
      wifi::IWifiChip::ChipDebugInfo *out) override {
    out->driverDescription.clear();
    out->firmwareDescription.clear();
    return ScopedAStatus::ok();
  }
  ScopedAStatus requestDriverDebugDump(std::vector<uint8_t> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }
  ScopedAStatus requestFirmwareDebugDump(std::vector<uint8_t> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }
  ScopedAStatus enableDebugErrorAlerts(bool) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus flushRingBufferToFile() override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus forceDumpToDebugRingBuffer(const std::string &) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus resetTxPowerScenario() override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus selectTxPowerScenario(
      wifi::IWifiChip::TxPowerScenario) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus setCoexUnsafeChannels(
      const std::vector<wifi::IWifiChip::CoexUnsafeChannel> &,
      int32_t) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus setCountryCode(const std::array<uint8_t, 2> &) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus setLatencyMode(wifi::IWifiChip::LatencyMode) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus setMultiStaPrimaryConnection(
      const std::string &name) override {
    for (size_t index = 0; index < sta_.size(); ++index) {
      if (name == interfaceNames()[index] && sta_created_[index]) {
        return ScopedAStatus::ok();
      }
    }
    return wifiError(wifi::WifiStatusCode::ERROR_WIFI_IFACE_INVALID);
  }

  ScopedAStatus setMultiStaUseCase(
      wifi::IWifiChip::MultiStaUseCase) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus startLoggingToDebugRingBuffer(
      const std::string &, wifi::WifiDebugRingBufferVerboseLevel, int32_t,
      int32_t) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus stopLoggingToDebugRingBuffer() override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus triggerSubsystemRestart() override {
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus enableStaChannelForPeerNetwork(int32_t) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus setAfcChannelAllowance(
      const wifi::AfcChannelAllowance &) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus setMloMode(wifi::IWifiChip::ChipMloMode) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus createApOrBridgedApIface(
      wifi::IfaceConcurrencyType,
      const std::vector<wifi::common::OuiKeyedData> &,
      std::shared_ptr<wifi::IWifiApIface> *out) override {
    out->reset();
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus setVoipMode(wifi::IWifiChip::VoipMode) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus removeApIface(const std::string &) override {
    return wifiError(wifi::WifiStatusCode::ERROR_WIFI_IFACE_INVALID);
  }

  ScopedAStatus removeNanIface(const std::string &) override {
    return wifiError(wifi::WifiStatusCode::ERROR_WIFI_IFACE_INVALID);
  }

  ScopedAStatus removeP2pIface(const std::string &) override {
    return wifiError(wifi::WifiStatusCode::ERROR_WIFI_IFACE_INVALID);
  }

  ScopedAStatus removeIfaceInstanceFromBridgedApIface(
      const std::string &, const std::string &) override {
    return wifiError(wifi::WifiStatusCode::ERROR_NOT_SUPPORTED);
  }

  ScopedAStatus getInterfaceVersion(int32_t *out) override {
    *out = wifi::IWifiChip::version;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getInterfaceHash(std::string *out) override {
    *out = wifi::IWifiChip::hash;
    return ScopedAStatus::ok();
  }

 private:
  static const std::array<std::string, 2> &interfaceNames() {
    static const std::array<std::string, 2> names = {
        kInterfaceName, kSecondaryInterfaceName};
    return names;
  }

  int32_t mode_ = 0;
  std::array<bool, 2> sta_created_ = {false, false};
  std::shared_ptr<wifi::IWifiChipEventCallback> callback_;
  std::array<std::shared_ptr<WifiSta>, 2> sta_impl_;
  std::array<std::shared_ptr<wifi::IWifiStaIface>, 2> sta_;
};

class WifiService final : public wifi::BnWifi {
 public:
  WifiService() {
    chip_impl_ = ndk::SharedRefBase::make<WifiChip>();
    chip_ = ndk::SharedRefBase::make<wifi::IWifiChipDelegator>(
        chip_impl_);
  }

  ScopedAStatus getChip(int32_t id,
                        std::shared_ptr<wifi::IWifiChip> *out) override {
    if (id != 0) {
      out->reset();
      return wifiError(wifi::WifiStatusCode::ERROR_WIFI_CHIP_INVALID);
    }
    *out = chip_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getChipIds(std::vector<int32_t> *out) override {
    *out = {0};
    return ScopedAStatus::ok();
  }

  ScopedAStatus isStarted(bool *out) override {
    *out = started_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus registerEventCallback(
      const std::shared_ptr<wifi::IWifiEventCallback> &callback) override {
    callback_ = callback;
    return ScopedAStatus::ok();
  }

  ScopedAStatus start() override {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "start started=%d",
                        started_);
    if (!started_) chip_impl_->resetStaInterface();
    started_ = true;
    if (callback_) callback_->onStart();
    return ScopedAStatus::ok();
  }

  ScopedAStatus stop() override {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "stop started=%d",
                        started_);
    started_ = false;
    chip_impl_->resetStaInterface();
    if (callback_) callback_->onStop();
    return ScopedAStatus::ok();
  }

 private:
  bool started_ = false;
  std::shared_ptr<wifi::IWifiEventCallback> callback_;
  std::shared_ptr<WifiChip> chip_impl_;
  std::shared_ptr<wifi::IWifiChip> chip_;
};

}  // namespace

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(1);
  auto service = ndk::SharedRefBase::make<WifiService>();
  if (AServiceManager_addService(service->asBinder().get(), kServiceName) !=
      STATUS_OK) {
    return 1;
  }
  // 宿主 wifi 通道（/dev/hvc9，v3 顺延布局 wifi 控制面）：后台线程收 host 下发的 AP 档位并写入 scan_profile
  cdh_wifi::HostChannel host_channel;
  host_channel.start();
  ABinderProcess_startThreadPool();
  ABinderProcess_joinThreadPool();
  return 0;
}
