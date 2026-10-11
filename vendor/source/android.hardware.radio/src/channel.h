// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include "state.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cdh::radio {

enum class Status : std::uint16_t {
  kOk = 0,
  kUnsupported = 1,
  kInvalidArgument = 2,
  kRadioUnavailable = 3,
  kDeadlineExceeded = 4,
  kNetworkRejected = 5,
  kRoutingUnavailable = 6,
  kIndeterminate = 7,
  kInternalError = 8,
};

enum class CommandKind : std::uint16_t {
  kGetSnapshot = 1,
  kSetRadioPower = 2,
  kSetupDataCall = 3,
  kDeactivateDataCall = 4,
  kSendSms = 5,
  kInjectSms = 6,
  kDial = 7,
  kInjectCall = 8,
  kAnswerCall = 9,
  kHoldCall = 10,
  kRejectCall = 11,
  kHangupCall = 12,
};

struct DataCallRequest {
  std::int32_t id = 0;
  std::string apn;
  std::string protocol;
};

struct SmsRequest {
  std::string destination;
  std::vector<std::uint8_t> pdu;
};

struct CallRequest {
  std::int32_t id = 0;
  std::string number;
};

struct Command {
  CommandKind kind = CommandKind::kGetSnapshot;
  RadioPower radio_power = RadioPower::kUnavailable;
  DataCallRequest data_call;
  SmsRequest sms;
  CallRequest call;
};

struct CommandResult {
  Status status = Status::kInternalError;
  std::optional<DataCall> data_call = std::nullopt;
};

class Channel {
public:
  using IncomingSmsListener = std::function<void(const IncomingSms &)>;

  explicit Channel(std::string device_path = "/dev/hvc4");
  ~Channel();

  Channel(const Channel &) = delete;
  Channel &operator=(const Channel &) = delete;

  bool Start(StateStore *state_store, std::string *error);
  bool Start(int owned_file_descriptor, StateStore *state_store,
             std::string *error);
  void Stop();

  bool WaitUntilReady(std::chrono::milliseconds timeout) const;
  bool WaitUntilStopped(std::chrono::milliseconds timeout) const;
  bool IsReady() const;
  bool IsRunning() const;
  std::string LastError() const;
  std::uint64_t SessionId() const;
  std::uint64_t BootGeneration() const;
  std::uint64_t ProviderGeneration() const;
  std::uint64_t AddIncomingSmsListener(IncomingSmsListener listener);
  void RemoveIncomingSmsListener(std::uint64_t id);

  CommandResult Request(const Command &command,
                        std::chrono::milliseconds timeout);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cdh::radio
