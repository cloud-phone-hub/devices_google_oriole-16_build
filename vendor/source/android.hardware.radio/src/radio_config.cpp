// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "radio_config.h"

#include "radio_service.h"

#include <aidl/android/hardware/radio/RadioIndicationType.h>
#include <aidl/android/hardware/radio/config/BnRadioConfig.h>
#include <aidl/android/hardware/radio/config/IRadioConfigIndication.h>
#include <aidl/android/hardware/radio/config/IRadioConfigResponse.h>
#include <aidl/android/hardware/radio/config/MultipleEnabledProfilesMode.h>
#include <aidl/android/hardware/radio/config/PhoneCapability.h>
#include <aidl/android/hardware/radio/config/SimPortInfo.h>
#include <aidl/android/hardware/radio/config/SimSlotStatus.h>
#include <aidl/android/hardware/radio/config/SlotPortMapping.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace cdh::radio {
namespace {

namespace config = ::aidl::android::hardware::radio::config;

std::vector<config::SimSlotStatus>
ToSimSlotStatus(const std::optional<Snapshot> &snapshot) {
  // 两个逻辑槽：槽 0 用宿主快照，槽 1 未插卡（gold 真机 gsm.sim.state=LOADED,ABSENT）。
  // 真机对齐：gold 的 UNSOL_SIM_SLOT_STATUS_CHANGED 两槽都是
  //   slot0 {CARDSTATE_PRESENT, iccid, logicalSlotIndex=0, portActive=true}
  //   slot1 {CARDSTATE_ABSENT,  iccid=,  logicalSlotIndex=1, portActive=true}
  // portActive=true 表示物理卡位在机（与是否插卡无关）。若置 false，框架
  // onGetSlotStatusDone 不填 mPhoneIdToSlotId（初始 -1），phone1 无槽映射，
  // MTK SMSVC checkAllIccIdReady 永远等不齐 → 订阅永不创建。
  std::vector<config::SimSlotStatus> slots;
  slots.reserve(kLogicalModemCount);
  const bool present = snapshot.has_value() && snapshot->sim.present;
  slots.push_back(config::SimSlotStatus{
      .cardState = static_cast<int8_t>(present ? 1 : 0),
      .atr = present ? "3B9F96801FC7AFC08031E073FE211B66D000000000000000" : "",
      .eid = "",
      .portInfo = {config::SimPortInfo{
          .iccId = snapshot.has_value() ? snapshot->sim.identity.iccid : "",
          .logicalSlotId = 0,
          .portActive = true,
      }},
      .supportedMepMode = config::MultipleEnabledProfilesMode::NONE,
  });
  for (std::int32_t slot = 1; slot < kLogicalModemCount; ++slot) {
    slots.push_back(config::SimSlotStatus{
        .cardState = 0,
        .atr = "",
        .eid = "",
        .portInfo = {config::SimPortInfo{
            .iccId = "",
            .logicalSlotId = slot,
            .portActive = true,
        }},
        .supportedMepMode = config::MultipleEnabledProfilesMode::NONE,
    });
  }
  return slots;
}

class RadioConfig final : public config::BnRadioConfig,
                          private RadioServiceState {
public:
  RadioConfig(Channel &channel, StateStore &state_store)
      : RadioServiceState(channel, state_store),
        observer_(state_store, [this](const Snapshot &snapshot) {
          const auto indication = callbacks_.IndicationCallback();
          if (indication != nullptr) {
            indication->simSlotsStatusChanged(
                aidl_radio::RadioIndicationType::UNSOLICITED,
                ToSimSlotStatus(snapshot));
          }
        }) {}

  ::ndk::ScopedAStatus getHalDeviceCapabilities(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getHalDeviceCapabilitiesResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE), false);
  }

  ::ndk::ScopedAStatus getNumOfLiveModems(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getNumOfLiveModemsResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        static_cast<int8_t>(kLogicalModemCount));
  }

  ::ndk::ScopedAStatus getPhoneCapability(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    // dsds：两个逻辑 modem（逻辑槽 0/1），数据/语音各只允许一路活跃。
    // logicalModemIds 是 AIDL byte[]（std::vector<uint8_t>）。
    std::vector<uint8_t> logical_modem_ids;
    for (std::int32_t slot = 0; slot < kLogicalModemCount; ++slot)
      logical_modem_ids.push_back(static_cast<uint8_t>(slot));
    config::PhoneCapability capability{
        .maxActiveData = 1,
        .maxActiveInternetData = 1,
        .isInternetLingeringSupported = false,
        .logicalModemIds = std::move(logical_modem_ids),
        .maxActiveVoice = 1,
    };
    return response->getPhoneCapabilityResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE), capability);
  }

  ::ndk::ScopedAStatus getSimSlotsStatus(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    return response->getSimSlotsStatusResponse(
        ResponseInfo(serial, snapshot.has_value()
                                 ? aidl_radio::RadioError::NONE
                                 : aidl_radio::RadioError::RADIO_NOT_AVAILABLE),
        ToSimSlotStatus(snapshot));
  }

  ::ndk::ScopedAStatus setNumOfLiveModems(int32_t serial,
                                          int8_t count) override {
    const bool valid = count >= 1 && count <= kLogicalModemCount;
    return RespondSimple(
        callbacks_.ResponseCallback(), serial,
        valid ? aidl_radio::RadioError::NONE
              : aidl_radio::RadioError::INVALID_ARGUMENTS,
        &config::IRadioConfigResponse::setNumOfLiveModemsResponse);
  }

  ::ndk::ScopedAStatus setPreferredDataModem(int32_t serial,
                                             int8_t modem_id) override {
    const bool valid = modem_id >= 0 && modem_id < kLogicalModemCount;
    return RespondSimple(
        callbacks_.ResponseCallback(), serial,
        valid ? aidl_radio::RadioError::NONE
              : aidl_radio::RadioError::INVALID_ARGUMENTS,
        &config::IRadioConfigResponse::setPreferredDataModemResponse);
  }

  ::ndk::ScopedAStatus setResponseFunctions(
      const std::shared_ptr<config::IRadioConfigResponse> &response,
      const std::shared_ptr<config::IRadioConfigIndication> &indication)
      override {
    if (response == nullptr || indication == nullptr) {
      return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    callbacks_.Set(response, indication);
    const auto snapshot = SnapshotState();
    if (snapshot.has_value()) {
      auto status = indication->simSlotsStatusChanged(
          aidl_radio::RadioIndicationType::UNSOLICITED,
          ToSimSlotStatus(snapshot));
      if (!status.isOk())
        return status;
      return indication->onSimultaneousCallingSupportChanged({0});
    }
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus setSimSlotsMapping(
      int32_t serial,
      const std::vector<config::SlotPortMapping> &mapping) override {
    // 单物理卡位（portId 0）映射到 1..kLogicalModemCount 个逻辑槽。
    const bool valid =
        !mapping.empty() && mapping.size() <= kLogicalModemCount &&
        std::all_of(mapping.begin(), mapping.end(), [](const auto &entry) {
          return entry.physicalSlotId == 0 && entry.portId == 0;
        });
    return RespondSimple(
        callbacks_.ResponseCallback(), serial,
        valid ? aidl_radio::RadioError::NONE
              : aidl_radio::RadioError::INVALID_ARGUMENTS,
        &config::IRadioConfigResponse::setSimSlotsMappingResponse);
  }

  ::ndk::ScopedAStatus getSimultaneousCallingSupport(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getSimultaneousCallingSupportResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE), {0});
  }

private:
  ResponseFunctions<config::IRadioConfigResponse,
                    config::IRadioConfigIndication>
      callbacks_;
  StateObserver observer_;
};

} // namespace

std::shared_ptr<config::IRadioConfig>
CreateRadioConfig(Channel &channel, StateStore &state_store) {
  return ::ndk::SharedRefBase::make<RadioConfig>(channel, state_store);
}

} // namespace cdh::radio
