// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace cdh::radio {

inline constexpr std::int64_t kBusyRetryMS = 120000;
inline constexpr std::int64_t kRetryNever =
    std::numeric_limits<std::int64_t>::max();

class DataPolicy {
public:
  bool SupportsDefault(std::int32_t apn_types) const;
  std::int32_t NextCID();

private:
  std::atomic<std::int32_t> next_cid_{1};
};

} // namespace cdh::radio
