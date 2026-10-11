// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "radio_voice.h"

#include "radio_service.h"

#include <aidl/android/hardware/radio/RadioIndicationType.h>
#include <aidl/android/hardware/radio/voice/AudioQuality.h>
#include <aidl/android/hardware/radio/voice/BnRadioVoice.h>
#include <aidl/android/hardware/radio/voice/Call.h>
#include <aidl/android/hardware/radio/voice/ClipStatus.h>
#include <aidl/android/hardware/radio/voice/IRadioVoiceIndication.h>
#include <aidl/android/hardware/radio/voice/IRadioVoiceResponse.h>
#include <aidl/android/hardware/radio/voice/LastCallFailCause.h>
#include <aidl/android/hardware/radio/voice/LastCallFailCauseInfo.h>
#include <aidl/android/hardware/radio/voice/TtyMode.h>

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cdh::radio {
namespace {

namespace voice = ::aidl::android::hardware::radio::voice;

std::optional<std::int32_t> FindCall(const std::optional<Snapshot> &snapshot,
                                     std::initializer_list<CallState> states) {
  if (!snapshot.has_value())
    return std::nullopt;
  for (const Call &call : snapshot->calls) {
    for (const CallState state : states) {
      if (call.state == state)
        return call.id;
    }
  }
  return std::nullopt;
}

int ToCallState(CallState state) {
  switch (state) {
  case CallState::kActive:
    return voice::Call::STATE_ACTIVE;
  case CallState::kHeld:
    return voice::Call::STATE_HOLDING;
  case CallState::kDialing:
    return voice::Call::STATE_DIALING;
  case CallState::kAlerting:
    return voice::Call::STATE_ALERTING;
  case CallState::kIncoming:
    return voice::Call::STATE_INCOMING;
  case CallState::kWaiting:
    return voice::Call::STATE_WAITING;
  case CallState::kUnknown:
  case CallState::kDisconnected:
    return voice::Call::STATE_ACTIVE;
  }
  return voice::Call::STATE_ACTIVE;
}

std::vector<voice::Call> ToCalls(const std::optional<Snapshot> &snapshot) {
  std::vector<voice::Call> output;
  if (!snapshot.has_value())
    return output;
  output.reserve(snapshot->calls.size());
  for (const Call &source : snapshot->calls) {
    if (source.state == CallState::kDisconnected)
      continue;
    output.push_back(voice::Call{
        .state = ToCallState(source.state),
        .index = source.id,
        .toa =
            !source.number.empty() && source.number.front() == '+' ? 145 : 129,
        .isMpty = source.multiparty,
        .isMT = source.direction == CallDirection::kIncoming,
        .als = 0,
        .isVoice = true,
        .isVoicePrivacy = false,
        .number = source.number,
        .numberPresentation = voice::Call::PRESENTATION_ALLOWED,
        .name = "",
        .namePresentation = voice::Call::PRESENTATION_ALLOWED,
        .uusInfo = {},
        .audioQuality = voice::AudioQuality::AMR_WB,
        .forwardedNumber = "",
    });
  }
  return output;
}

class RadioVoice final : public voice::BnRadioVoice, private RadioServiceState {
public:
  RadioVoice(Channel &channel, StateStore &state_store, std::int32_t slot)
      : RadioServiceState(channel, state_store, slot),
        observer_(state_store, [this](const Snapshot &) {
          const auto indication = callbacks_.IndicationCallback();
          if (indication != nullptr) {
            indication->callStateChanged(
                aidl_radio::RadioIndicationType::UNSOLICITED);
          }
        }) {}

  ::ndk::ScopedAStatus acceptCall(int32_t serial) override {
    return ControlFirstCall(serial, CommandKind::kAnswerCall,
                            {CallState::kIncoming, CallState::kWaiting},
                            &voice::IRadioVoiceResponse::acceptCallResponse);
  }

  ::ndk::ScopedAStatus cancelPendingUssd(int32_t serial) override {
    return RespondUnsupported(
        serial, &voice::IRadioVoiceResponse::cancelPendingUssdResponse);
  }

  ::ndk::ScopedAStatus conference(int32_t serial) override {
    return RespondUnsupported(serial,
                              &voice::IRadioVoiceResponse::conferenceResponse);
  }

  ::ndk::ScopedAStatus dial(int32_t serial,
                            const voice::Dial &dial_info) override {
    return Dial(serial, dial_info, &voice::IRadioVoiceResponse::dialResponse);
  }

  ::ndk::ScopedAStatus emergencyDial(int32_t serial,
                                     const voice::Dial &dial_info, int32_t,
                                     const std::vector<std::string> &,
                                     voice::EmergencyCallRouting, bool,
                                     bool) override {
    return Dial(serial, dial_info,
                &voice::IRadioVoiceResponse::emergencyDialResponse);
  }

  ::ndk::ScopedAStatus exitEmergencyCallbackMode(int32_t serial) override {
    return RespondUnsupported(
        serial, &voice::IRadioVoiceResponse::exitEmergencyCallbackModeResponse);
  }

  ::ndk::ScopedAStatus explicitCallTransfer(int32_t serial) override {
    return RespondUnsupported(
        serial, &voice::IRadioVoiceResponse::explicitCallTransferResponse);
  }

  ::ndk::ScopedAStatus
  getCallForwardStatus(int32_t serial,
                       const voice::CallForwardInfo &) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getCallForwardStatusResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        {});
  }

  ::ndk::ScopedAStatus getCallWaiting(int32_t serial, int32_t) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getCallWaitingResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        call_waiting_.load(), call_waiting_service_class_.load());
  }

  ::ndk::ScopedAStatus getClip(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getClipResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        voice::ClipStatus::CLIP_PROVISIONED);
  }

  ::ndk::ScopedAStatus getClir(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getClirResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE), clir_.load(), 3);
  }

  ::ndk::ScopedAStatus getCurrentCalls(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    return response->getCurrentCallsResponse(
        ResponseInfo(serial, snapshot.has_value()
                                 ? aidl_radio::RadioError::NONE
                                 : aidl_radio::RadioError::RADIO_NOT_AVAILABLE),
        ToCalls(snapshot));
  }

  ::ndk::ScopedAStatus getLastCallFailCause(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getLastCallFailCauseResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        voice::LastCallFailCauseInfo{
            .causeCode = voice::LastCallFailCause::NORMAL,
            .vendorCause = "",
        });
  }

  ::ndk::ScopedAStatus getMute(int32_t serial) override {
    return RespondBoolean(serial, muted_.load(),
                          &voice::IRadioVoiceResponse::getMuteResponse);
  }

  ::ndk::ScopedAStatus getPreferredVoicePrivacy(int32_t serial) override {
    return RespondBoolean(
        serial, voice_privacy_.load(),
        &voice::IRadioVoiceResponse::getPreferredVoicePrivacyResponse);
  }

  ::ndk::ScopedAStatus getTtyMode(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getTtyModeResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        static_cast<voice::TtyMode>(tty_mode_.load()));
  }

  ::ndk::ScopedAStatus handleStkCallSetupRequestFromSim(int32_t serial,
                                                        bool) override {
    return RespondUnsupported(
        serial,
        &voice::IRadioVoiceResponse::handleStkCallSetupRequestFromSimResponse);
  }

  ::ndk::ScopedAStatus hangup(int32_t serial, int32_t gsm_index) override {
    return ControlCall(serial, CommandKind::kHangupCall, gsm_index,
                       &voice::IRadioVoiceResponse::hangupConnectionResponse);
  }

  ::ndk::ScopedAStatus
  hangupForegroundResumeBackground(int32_t serial) override {
    return ControlFirstCall(
        serial, CommandKind::kHangupCall,
        {CallState::kActive, CallState::kDialing, CallState::kAlerting},
        &voice::IRadioVoiceResponse::hangupForegroundResumeBackgroundResponse);
  }

  ::ndk::ScopedAStatus hangupWaitingOrBackground(int32_t serial) override {
    return ControlFirstCall(
        serial, CommandKind::kHangupCall,
        {CallState::kWaiting, CallState::kIncoming, CallState::kHeld},
        &voice::IRadioVoiceResponse::hangupWaitingOrBackgroundResponse);
  }

  ::ndk::ScopedAStatus isVoNrEnabled(int32_t serial) override {
    return RespondBoolean(serial, vonr_enabled_.load(),
                          &voice::IRadioVoiceResponse::isVoNrEnabledResponse);
  }

  ::ndk::ScopedAStatus rejectCall(int32_t serial) override {
    return ControlFirstCall(serial, CommandKind::kRejectCall,
                            {CallState::kIncoming, CallState::kWaiting},
                            &voice::IRadioVoiceResponse::rejectCallResponse);
  }

  ::ndk::ScopedAStatus responseAcknowledgement() override {
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus sendBurstDtmf(int32_t serial, const std::string &,
                                     int32_t, int32_t) override {
    return RespondUnsupported(
        serial, &voice::IRadioVoiceResponse::sendBurstDtmfResponse);
  }

  ::ndk::ScopedAStatus sendCdmaFeatureCode(int32_t serial,
                                           const std::string &) override {
    return RespondUnsupported(
        serial, &voice::IRadioVoiceResponse::sendCdmaFeatureCodeResponse);
  }

  ::ndk::ScopedAStatus sendDtmf(int32_t serial, const std::string &) override {
    return RespondUnsupported(serial,
                              &voice::IRadioVoiceResponse::sendDtmfResponse);
  }

  ::ndk::ScopedAStatus sendUssd(int32_t serial, const std::string &) override {
    return RespondUnsupported(serial,
                              &voice::IRadioVoiceResponse::sendUssdResponse);
  }

  ::ndk::ScopedAStatus separateConnection(int32_t serial, int32_t) override {
    return RespondUnsupported(
        serial, &voice::IRadioVoiceResponse::separateConnectionResponse);
  }

  ::ndk::ScopedAStatus setCallForward(int32_t serial,
                                      const voice::CallForwardInfo &) override {
    return RespondUnsupported(
        serial, &voice::IRadioVoiceResponse::setCallForwardResponse);
  }

  ::ndk::ScopedAStatus setCallWaiting(int32_t serial, bool enable,
                                      int32_t service_class) override {
    call_waiting_.store(enable);
    call_waiting_service_class_.store(service_class);
    return RespondLocal(serial,
                        &voice::IRadioVoiceResponse::setCallWaitingResponse);
  }

  ::ndk::ScopedAStatus setClir(int32_t serial, int32_t status) override {
    const bool valid = status >= voice::Dial::CLIR_DEFAULT &&
                       status <= voice::Dial::CLIR_SUPPRESSION;
    if (valid)
      clir_.store(status);
    return RespondLocal(serial, &voice::IRadioVoiceResponse::setClirResponse,
                        valid);
  }

  ::ndk::ScopedAStatus setMute(int32_t serial, bool enable) override {
    muted_.store(enable);
    return RespondLocal(serial, &voice::IRadioVoiceResponse::setMuteResponse);
  }

  ::ndk::ScopedAStatus setPreferredVoicePrivacy(int32_t serial,
                                                bool enable) override {
    voice_privacy_.store(enable);
    return RespondLocal(
        serial, &voice::IRadioVoiceResponse::setPreferredVoicePrivacyResponse);
  }

  ::ndk::ScopedAStatus setResponseFunctions(
      const std::shared_ptr<voice::IRadioVoiceResponse> &response,
      const std::shared_ptr<voice::IRadioVoiceIndication> &indication)
      override {
    if (response == nullptr || indication == nullptr) {
      return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    callbacks_.Set(response, indication);
    return indication->callStateChanged(
        aidl_radio::RadioIndicationType::UNSOLICITED);
  }

  ::ndk::ScopedAStatus setTtyMode(int32_t serial,
                                  voice::TtyMode mode) override {
    tty_mode_.store(static_cast<std::int32_t>(mode));
    return RespondLocal(serial,
                        &voice::IRadioVoiceResponse::setTtyModeResponse);
  }

  ::ndk::ScopedAStatus setVoNrEnabled(int32_t serial, bool enable) override {
    vonr_enabled_.store(enable);
    return RespondLocal(serial,
                        &voice::IRadioVoiceResponse::setVoNrEnabledResponse);
  }

  ::ndk::ScopedAStatus startDtmf(int32_t serial, const std::string &) override {
    return RespondUnsupported(serial,
                              &voice::IRadioVoiceResponse::startDtmfResponse);
  }

  ::ndk::ScopedAStatus stopDtmf(int32_t serial) override {
    return RespondUnsupported(serial,
                              &voice::IRadioVoiceResponse::stopDtmfResponse);
  }

  ::ndk::ScopedAStatus
  switchWaitingOrHoldingAndActive(int32_t serial) override {
    return ControlFirstCall(
        serial, CommandKind::kHoldCall, {CallState::kActive},
        &voice::IRadioVoiceResponse::switchWaitingOrHoldingAndActiveResponse);
  }

private:
  using SimpleResponse = ::ndk::ScopedAStatus (voice::IRadioVoiceResponse::*)(
      const aidl_radio::RadioResponseInfo &);
  using BooleanResponse = ::ndk::ScopedAStatus (voice::IRadioVoiceResponse::*)(
      const aidl_radio::RadioResponseInfo &, bool);

  ::ndk::ScopedAStatus Dial(int32_t serial, const voice::Dial &dial_info,
                            SimpleResponse method) {
    Command command;
    command.kind = CommandKind::kDial;
    command.call.id = next_call_id_.fetch_add(1);
    command.call.number = dial_info.address;
    const CommandResult result = Execute(std::move(command));
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         ToRadioError(result.status), method);
  }

  ::ndk::ScopedAStatus ControlFirstCall(int32_t serial, CommandKind kind,
                                        std::initializer_list<CallState> states,
                                        SimpleResponse method) {
    const auto call_id = FindCall(SnapshotState(), states);
    if (!call_id.has_value()) {
      return RespondSimple(callbacks_.ResponseCallback(), serial,
                           aidl_radio::RadioError::INVALID_STATE, method);
    }
    return ControlCall(serial, kind, *call_id, method);
  }

  ::ndk::ScopedAStatus ControlCall(int32_t serial, CommandKind kind,
                                   std::int32_t call_id,
                                   SimpleResponse method) {
    if (call_id <= 0) {
      return RespondSimple(callbacks_.ResponseCallback(), serial,
                           aidl_radio::RadioError::INVALID_ARGUMENTS, method);
    }
    Command command;
    command.kind = kind;
    command.call.id = call_id;
    const CommandResult result = Execute(std::move(command));
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         ToRadioError(result.status), method);
  }

  ::ndk::ScopedAStatus RespondLocal(int32_t serial, SimpleResponse method,
                                    bool valid = true) {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         valid ? aidl_radio::RadioError::NONE
                               : aidl_radio::RadioError::INVALID_ARGUMENTS,
                         method);
  }

  ::ndk::ScopedAStatus RespondUnsupported(int32_t serial,
                                          SimpleResponse method) {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         aidl_radio::RadioError::REQUEST_NOT_SUPPORTED, method);
  }

  ::ndk::ScopedAStatus RespondBoolean(int32_t serial, bool value,
                                      BooleanResponse method) {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return ((*response).*
            method)(ResponseInfo(serial, aidl_radio::RadioError::NONE), value);
  }

  ResponseFunctions<voice::IRadioVoiceResponse, voice::IRadioVoiceIndication>
      callbacks_;
  std::atomic<std::int32_t> next_call_id_{1};
  std::atomic<std::int32_t> call_waiting_service_class_{1};
  std::atomic<std::int32_t> clir_{voice::Dial::CLIR_DEFAULT};
  std::atomic<std::int32_t> tty_mode_{
      static_cast<std::int32_t>(voice::TtyMode::OFF)};
  std::atomic_bool call_waiting_{true};
  std::atomic_bool muted_{false};
  std::atomic_bool voice_privacy_{false};
  std::atomic_bool vonr_enabled_{true};
  StateObserver observer_;
};

} // namespace

std::shared_ptr<voice::IRadioVoice> CreateRadioVoice(Channel &channel,
                                                     StateStore &state_store,
                                                     std::int32_t slot) {
  return ::ndk::SharedRefBase::make<RadioVoice>(channel, state_store, slot);
}

} // namespace cdh::radio
