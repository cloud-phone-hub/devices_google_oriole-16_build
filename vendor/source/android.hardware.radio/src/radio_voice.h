// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include "channel.h"

#include <aidl/android/hardware/radio/voice/IRadioVoice.h>

#include <cstdint>
#include <memory>

namespace cdh::radio {

std::shared_ptr<::aidl::android::hardware::radio::voice::IRadioVoice>
CreateRadioVoice(Channel &channel, StateStore &state_store, std::int32_t slot);

} // namespace cdh::radio
