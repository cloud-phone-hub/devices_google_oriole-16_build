// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#ifndef CDH_WIFI_SCAN_MODEL_H_
#define CDH_WIFI_SCAN_MODEL_H_

#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cdh_wifi {

#ifndef CDH_WIFI_SCAN_MODEL_PATH
// v8：档位写入 mac80211_hwsim（内核据此注入假 AP beacon 供扫描、并伪响应 auth/assoc）
#define CDH_WIFI_SCAN_MODEL_PATH \
  "/sys/module/mac80211_hwsim/parameters/scan_profile"
#endif

constexpr char kScanModelPath[] = CDH_WIFI_SCAN_MODEL_PATH;
constexpr size_t kMaximumAccessPoints = 8;
constexpr size_t kMaximumProfileBytes = 4096;
constexpr size_t kMaximumCachedScans = 64;

enum class Security { kOpen, kWpa2, kWpa3 };

struct AccessPoint {
  std::string ssid;
  std::array<uint8_t, 6> bssid{};
  int frequency_mhz = 0;
  int rssi_dbm = 0;
  Security security = Security::kOpen;
};

struct ScanModel {
  std::vector<AccessPoint> access_points;
};

inline bool SetError(std::string_view message, std::string* error) {
  if (error != nullptr) *error = std::string(message);
  return false;
}

inline int HexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

inline bool ParseInteger(std::string_view text, int minimum, int maximum,
                         int* output) {
  if (text.empty() || output == nullptr) return false;
  int value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value < minimum || value > maximum) {
    return false;
  }
  *output = value;
  return true;
}

inline bool ParseSsid(std::string_view text, std::string* output) {
  if (output == nullptr || text.size() < 2 || text.size() > 64 ||
      text.size() % 2 != 0) {
    return false;
  }
  output->clear();
  output->reserve(text.size() / 2);
  for (size_t index = 0; index < text.size(); index += 2) {
    const int high = HexDigit(text[index]);
    const int low = HexDigit(text[index + 1]);
    if (high < 0 || low < 0) return false;
    output->push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

inline bool ParseBssid(std::string_view text,
                       std::array<uint8_t, 6>* output) {
  if (output == nullptr || text.size() != 17) return false;
  std::array<uint8_t, 6> value{};
  bool all_zero = true;
  bool all_ff = true;
  for (size_t index = 0; index < value.size(); ++index) {
    const size_t offset = index * 3;
    if (index != 0 && text[offset - 1] != ':') return false;
    const int high = HexDigit(text[offset]);
    const int low = HexDigit(text[offset + 1]);
    if (high < 0 || low < 0) return false;
    value[index] = static_cast<uint8_t>((high << 4) | low);
    all_zero = all_zero && value[index] == 0;
    all_ff = all_ff && value[index] == 0xff;
  }
  if ((value[0] & 1U) != 0 || all_zero || all_ff) return false;
  *output = value;
  return true;
}

inline bool IsFrequencySupported(int frequency_mhz) {
  constexpr std::array<int, 11> frequencies = {
      2412, 2437, 2462, 5180, 5200, 5220,
      5240, 5745, 5765, 5785, 5805,
  };
  for (const int frequency : frequencies) {
    if (frequency == frequency_mhz) return true;
  }
  return false;
}

inline bool IsScanDue(uint64_t iteration, int base_period_ms,
                      int bucket_period_ms) {
  if (base_period_ms <= 0 || bucket_period_ms <= 0 ||
      bucket_period_ms % base_period_ms != 0) {
    return false;
  }
  return iteration % static_cast<uint64_t>(bucket_period_ms / base_period_ms) ==
         0;
}

inline bool IsBandSupported(int band) {
  switch (band) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 6:
    case 7:
      return true;
    default:
      return false;
  }
}

inline bool IsDfsFrequency(int frequency_mhz) {
  return frequency_mhz >= 5260 && frequency_mhz <= 5700;
}

inline bool FrequencyMatchesBand(int frequency_mhz, int band) {
  if (!IsBandSupported(band)) return false;
  if (frequency_mhz >= 2400 && frequency_mhz < 2500) {
    return (band & 1) != 0;
  }
  if (frequency_mhz >= 5000 && frequency_mhz < 5900) {
    return IsDfsFrequency(frequency_mhz) ? (band & 4) != 0
                                         : (band & 2) != 0;
  }
  return false;
}

inline bool ShouldReportBatch(size_t cached_scans, size_t cached_bytes,
                              size_t maximum_cache_bytes,
                              int threshold_percent,
                              int threshold_scans, bool report_each_scan) {
  if (report_each_scan || cached_scans >= kMaximumCachedScans) return true;
  if (threshold_scans > 0 &&
      cached_scans >= static_cast<size_t>(threshold_scans)) {
    return true;
  }
  if (maximum_cache_bytes > 0 && cached_bytes >= maximum_cache_bytes) {
    return true;
  }
  if (threshold_percent > 0 && threshold_percent <= 100 &&
      maximum_cache_bytes > 0) {
    const size_t threshold =
        (maximum_cache_bytes * static_cast<size_t>(threshold_percent) + 99) /
        100;
    return cached_bytes >= threshold;
  }
  return false;
}

inline bool ParseSecurity(std::string_view text, Security* output) {
  if (output == nullptr) return false;
  if (text == "open") {
    *output = Security::kOpen;
    return true;
  }
  if (text == "wpa2") {
    *output = Security::kWpa2;
    return true;
  }
  if (text == "wpa3") {
    *output = Security::kWpa3;
    return true;
  }
  return false;
}

inline std::vector<std::string_view> Split(std::string_view text,
                                           char delimiter) {
  std::vector<std::string_view> values;
  size_t offset = 0;
  for (;;) {
    const size_t next = text.find(delimiter, offset);
    values.push_back(text.substr(offset, next - offset));
    if (next == std::string_view::npos) break;
    offset = next + 1;
  }
  return values;
}

inline bool IsDuplicate(const ScanModel& model, const AccessPoint& candidate) {
  for (const AccessPoint& access_point : model.access_points) {
    if (access_point.ssid == candidate.ssid ||
        access_point.bssid == candidate.bssid) {
      return true;
    }
  }
  return false;
}

inline bool ParseScanModel(std::string_view text, ScanModel* output,
                           std::string* error) {
  if (error != nullptr) error->clear();
  if (output == nullptr) return SetError("output is null", error);
  if (text.empty() || text.size() > kMaximumProfileBytes) {
    return SetError("profile size is invalid", error);
  }

  ScanModel parsed;
  for (const std::string_view record : Split(text, ';')) {
    if (record.empty() || parsed.access_points.size() >= kMaximumAccessPoints) {
      return SetError("access point count is invalid", error);
    }
    const std::vector<std::string_view> fields = Split(record, ',');
    if (fields.size() != 5) return SetError("field count is invalid", error);

    AccessPoint access_point;
    if (!ParseSsid(fields[0], &access_point.ssid)) {
      return SetError("SSID is invalid", error);
    }
    if (!ParseBssid(fields[1], &access_point.bssid)) {
      return SetError("BSSID is invalid", error);
    }
    if (!ParseInteger(fields[2], 2400, 5900, &access_point.frequency_mhz) ||
        !IsFrequencySupported(access_point.frequency_mhz)) {
      return SetError("frequency is invalid", error);
    }
    if (!ParseInteger(fields[3], -127, -1, &access_point.rssi_dbm)) {
      return SetError("RSSI is invalid", error);
    }
    if (!ParseSecurity(fields[4], &access_point.security)) {
      return SetError("security is invalid", error);
    }
    if (IsDuplicate(parsed, access_point)) {
      return SetError("access point is duplicated", error);
    }
    parsed.access_points.push_back(std::move(access_point));
  }

  const AccessPoint& primary = parsed.access_points.front();
  if (primary.ssid != "VirtWifi" || primary.security != Security::kOpen) {
    return SetError("primary access point is invalid", error);
  }
  *output = std::move(parsed);
  return true;
}

inline char HexCharacter(uint8_t value) {
  constexpr char digits[] = "0123456789abcdef";
  return digits[value & 0x0fU];
}

inline std::string SecurityName(Security security) {
  switch (security) {
    case Security::kOpen:
      return "open";
    case Security::kWpa2:
      return "wpa2";
    case Security::kWpa3:
      return "wpa3";
  }
  return "open";
}

inline std::string EncodeScanModel(const ScanModel& model) {
  std::string encoded;
  for (size_t index = 0; index < model.access_points.size(); ++index) {
    if (index != 0) encoded.push_back(';');
    const AccessPoint& access_point = model.access_points[index];
    for (const unsigned char value : access_point.ssid) {
      encoded.push_back(HexCharacter(value >> 4));
      encoded.push_back(HexCharacter(value));
    }
    encoded.push_back(',');
    for (size_t octet = 0; octet < access_point.bssid.size(); ++octet) {
      if (octet != 0) encoded.push_back(':');
      encoded.push_back(HexCharacter(access_point.bssid[octet] >> 4));
      encoded.push_back(HexCharacter(access_point.bssid[octet]));
    }
    encoded.push_back(',');
    encoded += std::to_string(access_point.frequency_mhz);
    encoded.push_back(',');
    encoded += std::to_string(access_point.rssi_dbm);
    encoded.push_back(',');
    encoded += SecurityName(access_point.security);
  }
  return encoded;
}

inline const AccessPoint* FindAccessPointByBssid(
    const ScanModel& model, const std::array<uint8_t, 6>& bssid) {
  for (const AccessPoint& access_point : model.access_points) {
    if (access_point.bssid == bssid) return &access_point;
  }
  return nullptr;
}

inline bool ReadScanModel(const std::string& path, ScanModel* output,
                          std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return SetError("profile cannot be opened", error);
  std::string encoded((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  if (input.bad() || encoded.size() > kMaximumProfileBytes) {
    return SetError("profile cannot be read", error);
  }
  while (!encoded.empty() &&
         (encoded.back() == '\n' || encoded.back() == '\r')) {
    encoded.pop_back();
  }
  return ParseScanModel(encoded, output, error);
}

inline bool WriteScanModel(const std::string& path, const ScanModel& model,
                           std::string* error) {
  const std::string encoded = EncodeScanModel(model);
  ScanModel verified;
  if (!ParseScanModel(encoded, &verified, error)) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return SetError("profile cannot be opened for write", error);
  output << encoded;
  output.flush();
  if (!output) return SetError("profile cannot be written", error);
  return true;
}

}  // namespace cdh_wifi

#endif  // CDH_WIFI_SCAN_MODEL_H_
