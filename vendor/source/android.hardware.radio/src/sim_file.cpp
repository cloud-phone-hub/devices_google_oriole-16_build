// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "sim_file.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cdh::radio {
namespace {

constexpr std::int32_t kGetResponse = 0xc0;
constexpr std::int32_t kReadRecord = 0xb2;
constexpr std::int32_t kReadBinary = 0xb0;
constexpr std::int32_t kEfMsisdn = 0x6f40;
constexpr std::size_t kMsisdnRecordSize = 28;
constexpr std::size_t kAdnFooterSize = 14;
constexpr std::int32_t kEfIccid = 0x2fe2;
constexpr std::size_t kIccidSize = 10; // 20 位十六进制 ICCID
constexpr std::int32_t kEfSst = 0x6f38;
constexpr std::size_t kSstSize = 3;

constexpr char kHex[] = "0123456789ABCDEF";

std::string Hex(const std::array<std::uint8_t, kMsisdnRecordSize> &bytes) {
  std::string output(bytes.size() * 2, '0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    output[index * 2] = kHex[bytes[index] >> 4];
    output[index * 2 + 1] = kHex[bytes[index] & 0x0f];
  }
  return output;
}

// ICCID 存储为 nibble-swizzled BCD：每字节低半字节 = 前一位数字，高半字节 =
// 后一位数字（TS 31.102）。框架 SIMRecords EVENT_GET_ICCID_DONE 用
// IccUtils.bcdToString() 解码，返回的字节数组必须是该 BCD 形态，否则数字
// 会反转/乱序。
std::optional<std::string> EncodeIccidBcd(const std::string &digits) {
  if (digits.size() != kIccidSize * 2)
    return std::nullopt;
  for (const unsigned char digit : digits) {
    if (!std::isdigit(digit))
      return std::nullopt;
  }
  std::string output;
  output.reserve(kIccidSize * 2);
  for (std::size_t index = 0; index < kIccidSize; ++index) {
    const std::uint8_t low = static_cast<std::uint8_t>(digits[index * 2] - '0');
    const std::uint8_t high = static_cast<std::uint8_t>(digits[index * 2 + 1] - '0');
    const std::uint8_t byte = static_cast<std::uint8_t>((high << 4) | low);
    output.push_back(kHex[byte >> 4]);
    output.push_back(kHex[byte & 0x0f]);
  }
  return output;
}

std::optional<std::string> EncodeMsisdn(const std::string &e164) {
  if (e164.size() < 2 || e164.size() > 16 || e164.front() != '+')
    return std::nullopt;
  const std::string digits = e164.substr(1);
  for (const unsigned char digit : digits) {
    if (!std::isdigit(digit))
      return std::nullopt;
  }

  std::array<std::uint8_t, kMsisdnRecordSize> record;
  record.fill(0xff);
  const std::size_t footer = record.size() - kAdnFooterSize;
  const std::size_t bcd_size = (digits.size() + 1) / 2;
  record[footer] = static_cast<std::uint8_t>(1 + bcd_size);
  record[footer + 1] = 0x91;
  for (std::size_t index = 0; index < bcd_size; ++index) {
    const std::size_t digit = index * 2;
    const std::uint8_t low = static_cast<std::uint8_t>(digits[digit] - '0');
    const std::uint8_t high = digit + 1 < digits.size()
                                  ? static_cast<std::uint8_t>(digits[digit + 1] - '0')
                                  : 0x0f;
    record[footer + 2 + index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return Hex(record);
}

} // namespace

std::optional<SimFileResponse>
ReadSimFile(std::int32_t command, std::int32_t file_id, std::int32_t p1,
            std::int32_t p2, std::int32_t p3, const std::string &e164,
            const std::string &iccid) {
  // EF-ICCID（0x2FE2）：透明文件 10 字节。SIMRecords 用它拿卡号（mIccId），
  // 订阅创建依赖此值（"updateSubscription: No ICCID available" 即因缺失）。
  // GET_RESPONSE 必须返回框架 IccFileHandler 期望的 TS 51.011 响应数据格式
  // （与 EF-MSISDN 相同）：data[6]=TYPE_EF(4)、data[13]=EF_TYPE_TRANSPARENT(0)、
  // data[2..3]=文件大小。此前用 ISO-7816 FCP（0x62/0xA0 模板）解析必抛
  // IccFileTypeMismatch，框架因此从不发 READ_BINARY。
  if (file_id == kEfIccid) {
    if (command == kGetResponse && p1 == 0 && p2 == 0 && p3 == 0x0f) {
      return SimFileResponse{.sw1 = 0x90, .sw2 = 0,
                             .payload = "0000000A2FE20400000000000000"};
    }
    if (command != kReadBinary ||
        (p3 != 0 && p3 != static_cast<std::int32_t>(kIccidSize))) {
      return std::nullopt;
    }
    const auto encoded = EncodeIccidBcd(iccid);
    if (!encoded.has_value())
      return std::nullopt;
    return SimFileResponse{.sw1 = 0x90, .sw2 = 0, .payload = *encoded};
  }
  // EF-SST（0x6F38）：SIM 服务表，透明文件 3 字节，GET_RESPONSE 同用 TS 51.011 格式。
  if (file_id == kEfSst) {
    if (command == kGetResponse && p1 == 0 && p2 == 0 && p3 == 0x0f) {
      return SimFileResponse{.sw1 = 0x90, .sw2 = 0,
                             .payload = "000000036F380400000000000000"};
    }
    if (command != kReadBinary ||
        (p3 != 0 && p3 != static_cast<std::int32_t>(kSstSize))) {
      return std::nullopt;
    }
    return SimFileResponse{.sw1 = 0x90, .sw2 = 0, .payload = "FFFFFF"};
  }
  if (file_id != kEfMsisdn)
    return std::nullopt;
  if (command == kGetResponse && p1 == 0 && p2 == 0 && p3 == 0x0f) {
    return SimFileResponse{
        .sw1 = 0x90,
        .sw2 = 0,
        .payload = "0000001C6F4004000000000000011C",
    };
  }
  if (command != kReadRecord || p1 != 1 || p2 != 4 ||
      p3 != static_cast<std::int32_t>(kMsisdnRecordSize)) {
    return std::nullopt;
  }
  const auto encoded = EncodeMsisdn(e164);
  if (!encoded.has_value())
    return std::nullopt;
  return SimFileResponse{.sw1 = 0x90, .sw2 = 0, .payload = *encoded};
}

} // namespace cdh::radio
