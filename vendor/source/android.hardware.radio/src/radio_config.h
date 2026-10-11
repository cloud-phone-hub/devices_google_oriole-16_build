// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include "channel.h"

#include <aidl/android/hardware/radio/config/IRadioConfig.h>

#include <memory>

namespace cdh::radio {

std::shared_ptr<::aidl::android::hardware::radio::config::IRadioConfig>
CreateRadioConfig(Channel &channel, StateStore &state_store);

} // namespace cdh::radio
