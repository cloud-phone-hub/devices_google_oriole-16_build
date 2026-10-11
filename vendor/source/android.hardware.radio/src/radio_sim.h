// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include "channel.h"

#include <aidl/android/hardware/radio/sim/IRadioSim.h>

#include <cstdint>
#include <memory>

namespace cdh::radio {

std::shared_ptr<::aidl::android::hardware::radio::sim::IRadioSim>
CreateRadioSim(Channel &channel, StateStore &state_store, std::int32_t slot);

} // namespace cdh::radio
