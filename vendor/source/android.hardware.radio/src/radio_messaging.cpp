// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "radio_messaging.h"

#include "radio_service.h"

#include <aidl/android/hardware/radio/RadioIndicationType.h>
#include <aidl/android/hardware/radio/messaging/BnRadioMessaging.h>
#include <aidl/android/hardware/radio/messaging/IRadioMessagingIndication.h>
#include <aidl/android/hardware/radio/messaging/IRadioMessagingResponse.h>
#include <aidl/android/hardware/radio/messaging/SendSmsResult.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cdh::radio {
namespace {

namespace messaging = ::aidl::android::hardware::radio::messaging;

int HexValue(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  return -1;
}

bool DecodeHex(const std::string &input, std::vector<std::uint8_t> *output) {
  if (input.empty() || input.size() % 2 != 0)
    return false;
  output->clear();
  output->reserve(input.size() / 2);
  for (std::size_t index = 0; index < input.size(); index += 2) {
    const int high = HexValue(input[index]);
    const int low = HexValue(input[index + 1]);
    if (high < 0 || low < 0)
      return false;
    output->push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

std::string DestinationFromSubmitPdu(const std::vector<std::uint8_t> &pdu) {
  if (pdu.size() < 5)
    return {};
  const std::size_t digits = pdu[2];
  const std::size_t encoded = (digits + 1) / 2;
  if (digits == 0 || encoded > pdu.size() - 4)
    return {};
  std::string number;
  if ((pdu[3] & 0x70) == 0x10)
    number.push_back('+');
  number.reserve(number.size() + digits);
  for (std::size_t index = 0; index < digits; ++index) {
    const std::uint8_t octet = pdu[4 + index / 2];
    const std::uint8_t digit = index % 2 == 0 ? octet & 0x0f : octet >> 4;
    if (digit > 9)
      return {};
    number.push_back(static_cast<char>('0' + digit));
  }
  return number;
}

messaging::SendSmsResult SmsResult(std::int32_t reference) {
  return messaging::SendSmsResult{
      .messageRef = reference,
      .ackPDU = "",
      .errorCode = 0,
  };
}

class RadioMessaging final : public messaging::BnRadioMessaging,
                             private RadioServiceState {
public:
  RadioMessaging(Channel &channel, StateStore &state_store, std::int32_t slot)
      : RadioServiceState(channel, state_store, slot),
        sms_observer_(channel, [this](const IncomingSms &sms) {
          const auto indication = callbacks_.IndicationCallback();
          if (indication != nullptr) {
            indication->newSms(aidl_radio::RadioIndicationType::UNSOLICITED,
                               sms.pdu);
          }
        }) {}

  ::ndk::ScopedAStatus
  acknowledgeIncomingGsmSmsWithPdu(int32_t serial, bool,
                                   const std::string &) override {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         aidl_radio::RadioError::NONE,
                         &messaging::IRadioMessagingResponse::
                             acknowledgeIncomingGsmSmsWithPduResponse);
  }

  ::ndk::ScopedAStatus
  acknowledgeLastIncomingCdmaSms(int32_t serial,
                                 const messaging::CdmaSmsAck &) override {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         aidl_radio::RadioError::REQUEST_NOT_SUPPORTED,
                         &messaging::IRadioMessagingResponse::
                             acknowledgeLastIncomingCdmaSmsResponse);
  }

  ::ndk::ScopedAStatus
  acknowledgeLastIncomingGsmSms(int32_t serial, bool,
                                messaging::SmsAcknowledgeFailCause) override {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         aidl_radio::RadioError::NONE,
                         &messaging::IRadioMessagingResponse::
                             acknowledgeLastIncomingGsmSmsResponse);
  }

  ::ndk::ScopedAStatus deleteSmsOnRuim(int32_t serial, int32_t) override {
    return RespondSimple(
        callbacks_.ResponseCallback(), serial,
        aidl_radio::RadioError::REQUEST_NOT_SUPPORTED,
        &messaging::IRadioMessagingResponse::deleteSmsOnRuimResponse);
  }

  ::ndk::ScopedAStatus deleteSmsOnSim(int32_t serial, int32_t) override {
    return RespondSimple(
        callbacks_.ResponseCallback(), serial,
        aidl_radio::RadioError::REQUEST_NOT_SUPPORTED,
        &messaging::IRadioMessagingResponse::deleteSmsOnSimResponse);
  }

  ::ndk::ScopedAStatus getCdmaBroadcastConfig(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getCdmaBroadcastConfigResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        {});
  }

  ::ndk::ScopedAStatus getGsmBroadcastConfig(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getGsmBroadcastConfigResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        {});
  }

  ::ndk::ScopedAStatus getSmscAddress(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    std::string address;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      address = smsc_address_;
    }
    return response->getSmscAddressResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE), address);
  }

  ::ndk::ScopedAStatus reportSmsMemoryStatus(int32_t serial, bool) override {
    return RespondSimple(
        callbacks_.ResponseCallback(), serial, aidl_radio::RadioError::NONE,
        &messaging::IRadioMessagingResponse::reportSmsMemoryStatusResponse);
  }

  ::ndk::ScopedAStatus responseAcknowledgement() override {
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus sendCdmaSms(int32_t serial,
                                   const messaging::CdmaSmsMessage &) override {
    return RespondUnsupportedSms(
        serial, &messaging::IRadioMessagingResponse::sendCdmaSmsResponse);
  }

  ::ndk::ScopedAStatus
  sendCdmaSmsExpectMore(int32_t serial,
                        const messaging::CdmaSmsMessage &) override {
    return RespondUnsupportedSms(
        serial,
        &messaging::IRadioMessagingResponse::sendCdmaSmsExpectMoreResponse);
  }

  ::ndk::ScopedAStatus sendImsSms(int32_t serial,
                                  const messaging::ImsSmsMessage &) override {
    return RespondUnsupportedSms(
        serial, &messaging::IRadioMessagingResponse::sendImsSmsResponse);
  }

  ::ndk::ScopedAStatus
  sendSms(int32_t serial, const messaging::GsmSmsMessage &message) override {
    return SendGsmSms(serial, message,
                      &messaging::IRadioMessagingResponse::sendSmsResponse);
  }

  ::ndk::ScopedAStatus
  sendSmsExpectMore(int32_t serial,
                    const messaging::GsmSmsMessage &message) override {
    return SendGsmSms(
        serial, message,
        &messaging::IRadioMessagingResponse::sendSmsExpectMoreResponse);
  }

  ::ndk::ScopedAStatus setCdmaBroadcastActivation(int32_t serial,
                                                  bool) override {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         aidl_radio::RadioError::REQUEST_NOT_SUPPORTED,
                         &messaging::IRadioMessagingResponse::
                             setCdmaBroadcastActivationResponse);
  }

  ::ndk::ScopedAStatus setCdmaBroadcastConfig(
      int32_t serial,
      const std::vector<messaging::CdmaBroadcastSmsConfigInfo> &) override {
    return RespondSimple(
        callbacks_.ResponseCallback(), serial,
        aidl_radio::RadioError::REQUEST_NOT_SUPPORTED,
        &messaging::IRadioMessagingResponse::setCdmaBroadcastConfigResponse);
  }

  ::ndk::ScopedAStatus setGsmBroadcastActivation(int32_t serial,
                                                 bool) override {
    return RespondSimple(
        callbacks_.ResponseCallback(), serial,
        aidl_radio::RadioError::REQUEST_NOT_SUPPORTED,
        &messaging::IRadioMessagingResponse::setGsmBroadcastActivationResponse);
  }

  ::ndk::ScopedAStatus setGsmBroadcastConfig(
      int32_t serial,
      const std::vector<messaging::GsmBroadcastSmsConfigInfo> &) override {
    return RespondSimple(
        callbacks_.ResponseCallback(), serial,
        aidl_radio::RadioError::REQUEST_NOT_SUPPORTED,
        &messaging::IRadioMessagingResponse::setGsmBroadcastConfigResponse);
  }

  ::ndk::ScopedAStatus setResponseFunctions(
      const std::shared_ptr<messaging::IRadioMessagingResponse> &response,
      const std::shared_ptr<messaging::IRadioMessagingIndication> &indication)
      override {
    if (response == nullptr || indication == nullptr) {
      return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    callbacks_.Set(response, indication);
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus setSmscAddress(int32_t serial,
                                      const std::string &smsc) override {
    if (smsc.empty()) {
      return RespondSimple(
          callbacks_.ResponseCallback(), serial,
          aidl_radio::RadioError::INVALID_SMSC_ADDRESS,
          &messaging::IRadioMessagingResponse::setSmscAddressResponse);
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      smsc_address_ = smsc;
    }
    return RespondSimple(
        callbacks_.ResponseCallback(), serial, aidl_radio::RadioError::NONE,
        &messaging::IRadioMessagingResponse::setSmscAddressResponse);
  }

  ::ndk::ScopedAStatus
  writeSmsToRuim(int32_t serial, const messaging::CdmaSmsWriteArgs &) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->writeSmsToRuimResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        -1);
  }

  ::ndk::ScopedAStatus writeSmsToSim(int32_t serial,
                                     const messaging::SmsWriteArgs &) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->writeSmsToSimResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        -1);
  }

private:
  using SmsResponseMethod = ::ndk::ScopedAStatus (
      messaging::IRadioMessagingResponse::*)(
      const aidl_radio::RadioResponseInfo &, const messaging::SendSmsResult &);

  ::ndk::ScopedAStatus RespondUnsupportedSms(int32_t serial,
                                             SmsResponseMethod method) {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return ((*response).*method)(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        SmsResult(-1));
  }

  ::ndk::ScopedAStatus SendGsmSms(int32_t serial,
                                  const messaging::GsmSmsMessage &message,
                                  SmsResponseMethod method) {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    std::vector<std::uint8_t> pdu;
    if (!DecodeHex(message.pdu, &pdu)) {
      return ((*response).*method)(
          ResponseInfo(serial, aidl_radio::RadioError::INVALID_SMS_FORMAT),
          SmsResult(-1));
    }
    const std::string destination = DestinationFromSubmitPdu(pdu);
    if (destination.empty()) {
      return ((*response).*method)(
          ResponseInfo(serial, aidl_radio::RadioError::INVALID_SMS_FORMAT),
          SmsResult(-1));
    }
    Command command;
    command.kind = CommandKind::kSendSms;
    command.sms.destination = destination;
    command.sms.pdu = std::move(pdu);
    const CommandResult result = Execute(std::move(command));
    const std::int32_t reference = next_message_reference_.fetch_add(1);
    return ((*response).*
            method)(ResponseInfo(serial, ToRadioError(result.status)),
                    SmsResult(reference));
  }

  ResponseFunctions<messaging::IRadioMessagingResponse,
                    messaging::IRadioMessagingIndication>
      callbacks_;
  std::mutex state_mutex_;
  std::string smsc_address_ = "+8613010769500";
  std::atomic<std::int32_t> next_message_reference_{1};
  IncomingSmsObserver sms_observer_;
};

} // namespace

std::shared_ptr<messaging::IRadioMessaging>
CreateRadioMessaging(Channel &channel, StateStore &state_store,
                     std::int32_t slot) {
  return ::ndk::SharedRefBase::make<RadioMessaging>(channel, state_store, slot);
}

} // namespace cdh::radio
