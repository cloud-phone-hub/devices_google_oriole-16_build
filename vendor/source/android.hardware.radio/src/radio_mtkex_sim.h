// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include "channel.h"

#include <aidl/vendor/mediatek/hardware/mtkradioex/sim/IMtkRadioExSim.h>

#include <cstdint>
#include <memory>

namespace cdh::radio {

// gold MIUI/MTK 的 vendor.mediatek.hardware.mtkradioex.sim.IMtkRadioExSim 桩。
// 只实现 setResponseFunctionsMtk + 主动上报 iccidChanged：MTK 的
// MtkUiccController.mIccid[] 只有 iccidChanged 一条写入路径，而它是 SMSVC
// 订阅门禁 checkAllIccIdReady 的唯一数据源（取不到 iccid 就永不建订阅）。
std::shared_ptr<
    ::aidl::vendor::mediatek::hardware::mtkradioex::sim::IMtkRadioExSim>
CreateMtkRadioExSim(Channel &channel, StateStore &state_store,
                    std::int32_t slot);

} // namespace cdh::radio