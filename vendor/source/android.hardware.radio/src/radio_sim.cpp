// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "radio_sim.h"

#include "radio_service.h"
#include "sim_file.h"

#include <aidl/android/hardware/radio/RadioIndicationType.h>
#include <aidl/android/hardware/radio/config/MultipleEnabledProfilesMode.h>
#include <aidl/android/hardware/radio/config/SlotPortMapping.h>
#include <aidl/android/hardware/radio/sim/AppStatus.h>
#include <aidl/android/hardware/radio/sim/BnRadioSim.h>
#include <aidl/android/hardware/radio/sim/CardStatus.h>
#include <aidl/android/hardware/radio/sim/CarrierRestrictions.h>
#include <aidl/android/hardware/radio/sim/IRadioSimIndication.h>
#include <aidl/android/hardware/radio/sim/IRadioSimResponse.h>
#include <aidl/android/hardware/radio/sim/IccIoResult.h>
#include <aidl/android/hardware/radio/sim/PhonebookCapacity.h>
#include <aidl/android/hardware/radio/sim/PinState.h>
#include <aidl/android/hardware/radio/sim/SimLockMultiSimPolicy.h>
#include <aidl/android/hardware/radio/sim/SimRefreshResult.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cdh::radio {
namespace {

namespace sim = ::aidl::android::hardware::radio::sim;
namespace config = ::aidl::android::hardware::radio::config;

constexpr char kUsimAid[] = "A0000000871002FF86FFFF89FFFFFFFF";

int AppState(SimState state) {
  switch (state) {
  case SimState::kPinRequired:
    return sim::AppStatus::APP_STATE_PIN;
  case SimState::kPukRequired:
    return sim::AppStatus::APP_STATE_PUK;
  case SimState::kReady:
    return sim::AppStatus::APP_STATE_READY;
  case SimState::kAbsent:
  case SimState::kUnknown:
  case SimState::kError:
    return sim::AppStatus::APP_STATE_UNKNOWN;
  }
  return sim::AppStatus::APP_STATE_UNKNOWN;
}

sim::PinState PinState(SimState state) {
  switch (state) {
  case SimState::kPinRequired:
    return sim::PinState::ENABLED_NOT_VERIFIED;
  case SimState::kPukRequired:
    return sim::PinState::ENABLED_BLOCKED;
  case SimState::kReady:
  case SimState::kAbsent:
  case SimState::kUnknown:
  case SimState::kError:
    return sim::PinState::DISABLED;
  }
  return sim::PinState::UNKNOWN;
}

sim::CardStatus ToCardStatus(const Snapshot &snapshot, bool card_power_up,
                             bool applications_enabled,
                             std::int32_t physical_slot_id) {
  const bool present = snapshot.sim.present && card_power_up;
  sim::CardStatus card{
      .cardState =
          present ? static_cast<std::int32_t>(sim::CardStatus::STATE_PRESENT)
                  : static_cast<std::int32_t>(sim::CardStatus::STATE_ABSENT),
      .universalPinState = sim::PinState::DISABLED,
      .gsmUmtsSubscriptionAppIndex = present ? 0 : -1,
      .cdmaSubscriptionAppIndex = -1,
      .imsSubscriptionAppIndex = -1,
      .applications = {},
      .atr = present ? "3B9F96801FC7AFC08031E073FE211B66D000000000000000" : "",
      .iccid = present ? snapshot.sim.identity.iccid : "",
      .eid = "",
      // slotMap.physicalSlotId 必须随槽位变化：gold 真机 phone-1（ABSENT）的
      // GET_SIM_STATUS 返回 physicalSlotIndex=1。若全部槽位都返回 0，框架
      // onGetIccCardStatusDone 会用 phone-1 的 ABSENT 状态覆盖 UiccSlot[0]，
      // 销毁刚创建的 UiccCard/UiccPort → SMSVC getIccId 取不到 iccid → 订阅不建。
      .slotMap = config::SlotPortMapping{.physicalSlotId = physical_slot_id,
                                         .portId = 0},
      .supportedMepMode = config::MultipleEnabledProfilesMode::NONE,
  };
  if (present) {
    card.applications.push_back(sim::AppStatus{
        .appType = sim::AppStatus::APP_TYPE_USIM,
        .appState = applications_enabled ? AppState(snapshot.sim.state)
                                         : sim::AppStatus::APP_STATE_DETECTED,
        .persoSubstate = sim::PersoSubstate::READY,
        .aidPtr = kUsimAid,
        .appLabelPtr = "USIM",
        .pin1Replaced = false,
        .pin1 = PinState(snapshot.sim.state),
        .pin2 = sim::PinState::DISABLED,
    });
  }
  return card;
}

class RadioSim final : public sim::BnRadioSim, private RadioServiceState {
public:
  RadioSim(Channel &channel, StateStore &state_store, std::int32_t slot)
      : RadioServiceState(channel, state_store, slot), last_msisdn_([&state_store] {
          const auto snapshot = state_store.Current();
          return snapshot.has_value() ? snapshot->sim.identity.e164
                                      : std::string{};
        }()),
        observer_(state_store, [this](const Snapshot &snapshot) {
          NotifyStateChange(snapshot);
        }) {}

  ~RadioSim() override = default;

  ::ndk::ScopedAStatus areUiccApplicationsEnabled(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    return response->areUiccApplicationsEnabledResponse(
        ResponseInfo(serial, snapshot.has_value()
                                 ? aidl_radio::RadioError::NONE
                                 : aidl_radio::RadioError::RADIO_NOT_AVAILABLE),
        snapshot.has_value() && snapshot->sim.present &&
            applications_enabled_.load());
  }

  ::ndk::ScopedAStatus changeIccPin2ForApp(int32_t serial, const std::string &,
                                           const std::string &,
                                           const std::string &) override {
    return RespondRetries(serial,
                          &sim::IRadioSimResponse::changeIccPin2ForAppResponse);
  }

  ::ndk::ScopedAStatus changeIccPinForApp(int32_t serial, const std::string &,
                                          const std::string &,
                                          const std::string &) override {
    return RespondRetries(serial,
                          &sim::IRadioSimResponse::changeIccPinForAppResponse);
  }

  ::ndk::ScopedAStatus enableUiccApplications(int32_t serial,
                                              bool enable) override {
    applications_enabled_.store(enable);
    return RespondLocal(
        serial, &sim::IRadioSimResponse::enableUiccApplicationsResponse);
  }

  ::ndk::ScopedAStatus getAllowedCarriers(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getAllowedCarriersResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        sim::CarrierRestrictions{},
        sim::SimLockMultiSimPolicy::NO_MULTISIM_POLICY);
  }

  ::ndk::ScopedAStatus getCdmaSubscription(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getCdmaSubscriptionResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED), "",
        "", "", "", "");
  }

  ::ndk::ScopedAStatus getCdmaSubscriptionSource(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getCdmaSubscriptionSourceResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        sim::CdmaSubscriptionSource::RUIM_SIM);
  }

  ::ndk::ScopedAStatus getFacilityLockForApp(int32_t serial,
                                             const std::string &facility,
                                             const std::string &, int32_t,
                                             const std::string &) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    bool locked = false;
    {
      std::lock_guard<std::mutex> lock(facility_mutex_);
      const auto iterator = facility_locks_.find(facility);
      locked = iterator != facility_locks_.end() && iterator->second;
    }
    return response->getFacilityLockForAppResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE), locked ? 1 : 0);
  }

  ::ndk::ScopedAStatus getIccCardStatus(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    return response->getIccCardStatusResponse(
        ResponseInfo(serial, snapshot.has_value()
                                 ? aidl_radio::RadioError::NONE
                                 : aidl_radio::RadioError::RADIO_NOT_AVAILABLE),
        snapshot.has_value() ? ToCardStatus(*snapshot, card_power_up_.load(),
                                            applications_enabled_.load(), slot())
                             : sim::CardStatus{});
  }

  ::ndk::ScopedAStatus getImsiForApp(int32_t serial,
                                     const std::string &) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    return response->getImsiForAppResponse(
        ResponseInfo(serial, snapshot.has_value()
                                 ? aidl_radio::RadioError::NONE
                                 : aidl_radio::RadioError::RADIO_NOT_AVAILABLE),
        snapshot.has_value() ? snapshot->sim.identity.imsi : "");
  }

  ::ndk::ScopedAStatus getSimPhonebookCapacity(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getSimPhonebookCapacityResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        sim::PhonebookCapacity{});
  }

  ::ndk::ScopedAStatus getSimPhonebookRecords(int32_t serial) override {
    return RespondUnsupported(
        serial, &sim::IRadioSimResponse::getSimPhonebookRecordsResponse);
  }

  ::ndk::ScopedAStatus iccCloseLogicalChannel(int32_t serial,
                                              int32_t) override {
    return RespondUnsupported(
        serial, &sim::IRadioSimResponse::iccCloseLogicalChannelResponse);
  }

  ::ndk::ScopedAStatus iccIoForApp(int32_t serial,
                                   const sim::IccIo &request) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    if (!snapshot.has_value()) {
      return response->iccIoForAppResponse(
          ResponseInfo(serial, aidl_radio::RadioError::RADIO_NOT_AVAILABLE),
          sim::IccIoResult{});
    }
    const auto result =
        ReadSimFile(request.command, request.fileId, request.p1, request.p2,
                    request.p3, snapshot->sim.identity.e164,
                    snapshot->sim.identity.iccid);
    if (!result.has_value()) {
      return response->iccIoForAppResponse(
          ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
          sim::IccIoResult{});
    }
    return response->iccIoForAppResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        sim::IccIoResult{.sw1 = result->sw1,
                         .sw2 = result->sw2,
                         .simResponse = result->payload});
  }

  ::ndk::ScopedAStatus
  iccOpenLogicalChannel(int32_t serial, const std::string &, int32_t) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->iccOpenLogicalChannelResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED), -1,
        {});
  }

  ::ndk::ScopedAStatus
  iccTransmitApduBasicChannel(int32_t serial, const sim::SimApdu &) override {
    return RespondIccIo(
        serial, &sim::IRadioSimResponse::iccTransmitApduBasicChannelResponse);
  }

  ::ndk::ScopedAStatus
  iccTransmitApduLogicalChannel(int32_t serial, const sim::SimApdu &) override {
    return RespondIccIo(
        serial, &sim::IRadioSimResponse::iccTransmitApduLogicalChannelResponse);
  }

  ::ndk::ScopedAStatus reportStkServiceIsRunning(int32_t serial) override {
    return RespondLocal(
        serial, &sim::IRadioSimResponse::reportStkServiceIsRunningResponse);
  }

  ::ndk::ScopedAStatus
  requestIccSimAuthentication(int32_t serial, int32_t, const std::string &,
                              const std::string &) override {
    return RespondIccIo(
        serial, &sim::IRadioSimResponse::requestIccSimAuthenticationResponse);
  }

  ::ndk::ScopedAStatus responseAcknowledgement() override {
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus sendEnvelope(int32_t serial,
                                    const std::string &) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->sendEnvelopeResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        "");
  }

  ::ndk::ScopedAStatus sendEnvelopeWithStatus(int32_t serial,
                                              const std::string &) override {
    return RespondIccIo(
        serial, &sim::IRadioSimResponse::sendEnvelopeWithStatusResponse);
  }

  ::ndk::ScopedAStatus sendTerminalResponseToSim(int32_t serial,
                                                 const std::string &) override {
    return RespondUnsupported(
        serial, &sim::IRadioSimResponse::sendTerminalResponseToSimResponse);
  }

  ::ndk::ScopedAStatus setAllowedCarriers(int32_t serial,
                                          const sim::CarrierRestrictions &,
                                          sim::SimLockMultiSimPolicy) override {
    return RespondLocal(serial,
                        &sim::IRadioSimResponse::setAllowedCarriersResponse);
  }

  ::ndk::ScopedAStatus
  setCarrierInfoForImsiEncryption(int32_t serial,
                                  const sim::ImsiEncryptionInfo &) override {
    return RespondUnsupported(
        serial,
        &sim::IRadioSimResponse::setCarrierInfoForImsiEncryptionResponse);
  }

  ::ndk::ScopedAStatus
  setCdmaSubscriptionSource(int32_t serial,
                            sim::CdmaSubscriptionSource) override {
    return RespondUnsupported(
        serial, &sim::IRadioSimResponse::setCdmaSubscriptionSourceResponse);
  }

  ::ndk::ScopedAStatus setFacilityLockForApp(int32_t serial,
                                             const std::string &facility,
                                             bool lock_state,
                                             const std::string &, int32_t,
                                             const std::string &) override {
    {
      std::lock_guard<std::mutex> lock(facility_mutex_);
      facility_locks_[facility] = lock_state;
    }
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->setFacilityLockForAppResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE), 3);
  }

  ::ndk::ScopedAStatus setResponseFunctions(
      const std::shared_ptr<sim::IRadioSimResponse> &response,
      const std::shared_ptr<sim::IRadioSimIndication> &indication) override {
    if (response == nullptr || indication == nullptr) {
      return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    callbacks_.Set(response, indication);
    const auto snapshot = SnapshotState();
    if (!snapshot.has_value())
      return ::ndk::ScopedAStatus::ok();
    const auto type = aidl_radio::RadioIndicationType::UNSOLICITED;
    auto status = indication->simStatusChanged(type);
    if (!status.isOk())
      return status;
    status = indication->subscriptionStatusChanged(
        type, snapshot->subscription.active);
    if (!status.isOk())
      return status;
    return indication->uiccApplicationsEnablementChanged(
        type, applications_enabled_.load());
  }

  ::ndk::ScopedAStatus setSimCardPower(int32_t serial,
                                       sim::CardPowerState power_up) override {
    card_power_up_.store(power_up != sim::CardPowerState::POWER_DOWN);
    return RespondLocal(serial,
                        &sim::IRadioSimResponse::setSimCardPowerResponse);
  }

  ::ndk::ScopedAStatus
  setUiccSubscription(int32_t serial, const sim::SelectUiccSub &) override {
    return RespondLocal(serial,
                        &sim::IRadioSimResponse::setUiccSubscriptionResponse);
  }

  ::ndk::ScopedAStatus supplyIccPin2ForApp(int32_t serial, const std::string &,
                                           const std::string &) override {
    return RespondRetries(serial,
                          &sim::IRadioSimResponse::supplyIccPin2ForAppResponse);
  }

  ::ndk::ScopedAStatus supplyIccPinForApp(int32_t serial, const std::string &,
                                          const std::string &) override {
    return RespondRetries(serial,
                          &sim::IRadioSimResponse::supplyIccPinForAppResponse);
  }

  ::ndk::ScopedAStatus supplyIccPuk2ForApp(int32_t serial, const std::string &,
                                           const std::string &,
                                           const std::string &) override {
    return RespondRetries(serial,
                          &sim::IRadioSimResponse::supplyIccPuk2ForAppResponse);
  }

  ::ndk::ScopedAStatus supplyIccPukForApp(int32_t serial, const std::string &,
                                          const std::string &,
                                          const std::string &) override {
    return RespondRetries(serial,
                          &sim::IRadioSimResponse::supplyIccPukForAppResponse);
  }

  ::ndk::ScopedAStatus
  supplySimDepersonalization(int32_t serial, sim::PersoSubstate perso_type,
                             const std::string &) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->supplySimDepersonalizationResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        perso_type, -1);
  }

  ::ndk::ScopedAStatus
  updateSimPhonebookRecords(int32_t serial,
                            const sim::PhonebookRecordInfo &) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->updateSimPhonebookRecordsResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        -1);
  }

  ::ndk::ScopedAStatus
  iccCloseLogicalChannelWithSessionInfo(int32_t serial,
                                        const sim::SessionInfo &) override {
    return RespondUnsupported(
        serial,
        &sim::IRadioSimResponse::iccCloseLogicalChannelWithSessionInfoResponse);
  }

private:
  using SimpleResponse = ::ndk::ScopedAStatus (sim::IRadioSimResponse::*)(
      const aidl_radio::RadioResponseInfo &);
  using RetriesResponse = ::ndk::ScopedAStatus (sim::IRadioSimResponse::*)(
      const aidl_radio::RadioResponseInfo &, std::int32_t);
  using IccIoResponse = ::ndk::ScopedAStatus (sim::IRadioSimResponse::*)(
      const aidl_radio::RadioResponseInfo &, const sim::IccIoResult &);

  void NotifyStateChange(const Snapshot &snapshot) {
    bool msisdn_changed = false;
    {
      std::lock_guard<std::mutex> lock(sim_identity_mutex_);
      msisdn_changed = last_msisdn_ != snapshot.sim.identity.e164;
      last_msisdn_ = snapshot.sim.identity.e164;
    }

    const auto indication = callbacks_.IndicationCallback();
    if (indication == nullptr)
      return;
    const auto type = aidl_radio::RadioIndicationType::UNSOLICITED;
    if (msisdn_changed) {
      indication->simRefresh(type,
                             sim::SimRefreshResult{
                                 .type = sim::SimRefreshResult::TYPE_SIM_INIT,
                                 .efId = 0,
                                 .aid = kUsimAid,
                             });
    }
    indication->simStatusChanged(type);
    indication->subscriptionStatusChanged(type, snapshot.subscription.active);
    indication->uiccApplicationsEnablementChanged(type,
                                                  applications_enabled_.load());
  }

  ::ndk::ScopedAStatus RespondLocal(int32_t serial, SimpleResponse method) {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         aidl_radio::RadioError::NONE, method);
  }

  ::ndk::ScopedAStatus RespondUnsupported(int32_t serial,
                                          SimpleResponse method) {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         aidl_radio::RadioError::REQUEST_NOT_SUPPORTED, method);
  }

  ::ndk::ScopedAStatus RespondRetries(int32_t serial, RetriesResponse method) {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return ((*response).*method)(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        -1);
  }

  ::ndk::ScopedAStatus RespondIccIo(int32_t serial, IccIoResponse method) {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return ((*response).*method)(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        sim::IccIoResult{});
  }

  ResponseFunctions<sim::IRadioSimResponse, sim::IRadioSimIndication>
      callbacks_;
  std::atomic_bool applications_enabled_{true};
  std::atomic_bool card_power_up_{true};
  std::mutex facility_mutex_;
  std::map<std::string, bool> facility_locks_;
  std::mutex sim_identity_mutex_;
  std::string last_msisdn_;
  StateObserver observer_;
};

} // namespace

std::shared_ptr<sim::IRadioSim> CreateRadioSim(Channel &channel,
                                               StateStore &state_store,
                                               std::int32_t slot) {
  return ::ndk::SharedRefBase::make<RadioSim>(channel, state_store, slot);
}

} // namespace cdh::radio
