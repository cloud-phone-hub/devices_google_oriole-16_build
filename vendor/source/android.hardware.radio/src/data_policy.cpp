// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "data_policy.h"

namespace cdh::radio {

bool DataPolicy::SupportsDefault(std::int32_t apn_types) const {
  constexpr std::int32_t kDefaultAPN = 1;
  return (apn_types & kDefaultAPN) != 0;
}

std::int32_t DataPolicy::NextCID() {
  std::int32_t current = next_cid_.load(std::memory_order_relaxed);
  for (;;) {
    const std::int32_t next =
        current == std::numeric_limits<std::int32_t>::max() ? 1 : current + 1;
    if (next_cid_.compare_exchange_weak(current, next,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
      return current;
    }
  }
}

} // namespace cdh::radio
