// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cdh::radio {

struct SimFileResponse {
  std::int32_t sw1 = 0;
  std::int32_t sw2 = 0;
  std::string payload;
};

std::optional<SimFileResponse>
ReadSimFile(std::int32_t command, std::int32_t file_id, std::int32_t p1,
            std::int32_t p2, std::int32_t p3, const std::string &e164,
            const std::string &iccid);

} // namespace cdh::radio
