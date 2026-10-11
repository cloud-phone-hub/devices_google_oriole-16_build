// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include "channel.h"

#include <aidl/android/hardware/radio/RadioError.h>
#include <aidl/android/hardware/radio/RadioResponseInfo.h>
#include <aidl/android/hardware/radio/RadioResponseType.h>
#include <android/binder_status.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace cdh::radio {

namespace aidl_radio = ::aidl::android::hardware::radio;

constexpr std::chrono::milliseconds kRadioRequestTimeout{2000};

// 逻辑 modem/卡槽数：gold 真机是 dsds（gsm.sim.state=LOADED,ABSENT、两槽各自
// IRadioModem/slotN），框架据此建两条 Phone 并把 gsm.version.baseband 拼成
// "槽1版本,槽2版本"。本 HAL 的宿主快照只有一张卡，槽 1 按未插卡上报（见
// RadioServiceState::SnapshotState）。
inline constexpr std::int32_t kLogicalModemCount = 2;

inline aidl_radio::RadioError ToRadioError(Status status) {
  switch (status) {
  case Status::kOk:
    return aidl_radio::RadioError::NONE;
  case Status::kUnsupported:
  case Status::kRoutingUnavailable:
    return aidl_radio::RadioError::REQUEST_NOT_SUPPORTED;
  case Status::kInvalidArgument:
    return aidl_radio::RadioError::INVALID_ARGUMENTS;
  case Status::kRadioUnavailable:
    return aidl_radio::RadioError::RADIO_NOT_AVAILABLE;
  case Status::kDeadlineExceeded:
    return aidl_radio::RadioError::INTERNAL_ERR;
  case Status::kNetworkRejected:
    return aidl_radio::RadioError::NETWORK_REJECT;
  case Status::kIndeterminate:
    return aidl_radio::RadioError::GENERIC_FAILURE;
  case Status::kInternalError:
    return aidl_radio::RadioError::INTERNAL_ERR;
  }
  return aidl_radio::RadioError::INTERNAL_ERR;
}

inline aidl_radio::RadioResponseInfo
ResponseInfo(std::int32_t serial, aidl_radio::RadioError error) {
  return aidl_radio::RadioResponseInfo{
      .type = aidl_radio::RadioResponseType::SOLICITED,
      .serial = serial,
      .error = error,
  };
}

inline ::ndk::ScopedAStatus MissingResponseFunctions() {
  return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
}

template <typename Response, typename Indication> class ResponseFunctions {
public:
  void Set(std::shared_ptr<Response> response,
           std::shared_ptr<Indication> indication) {
    std::lock_guard<std::mutex> lock(mutex_);
    response_ = std::move(response);
    indication_ = std::move(indication);
  }

  std::shared_ptr<Response> ResponseCallback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return response_;
  }

  std::shared_ptr<Indication> IndicationCallback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return indication_;
  }

private:
  mutable std::mutex mutex_;
  std::shared_ptr<Response> response_;
  std::shared_ptr<Indication> indication_;
};

class RadioServiceState {
public:
  RadioServiceState(Channel &channel, StateStore &state_store,
                    std::int32_t slot = 0)
      : channel_(channel), state_store_(state_store), slot_(slot) {}

protected:
  std::int32_t slot() const { return slot_; }

  // 槽位视图：gold 真机是 dsds 双槽，slot2 未插卡（gsm.sim.state=LOADED,ABSENT）。
  // 宿主快照只描述一张卡，故槽 1 一律按「无卡/未注册」上报：SIM 置 ABSENT、清空
  // 运营商/注册/数据/通话，仅保留 modem（含基带版本）——与真机两槽 modem 都在、
  // gsm.version.baseband 两槽同值一致。
  std::optional<Snapshot> SnapshotState() const {
    auto snapshot = state_store_.Current();
    if (snapshot.has_value() && slot_ != 0) {
      snapshot->sim = Sim{};
      snapshot->sim.state = SimState::kAbsent;
      snapshot->subscription = Subscription{};
      snapshot->network = Network{};
      snapshot->data_calls.clear();
      snapshot->calls.clear();
    }
    return snapshot;
  }

  CommandResult Execute(Command command) {
    return channel_.Request(command, kRadioRequestTimeout);
  }

private:
  Channel &channel_;
  StateStore &state_store_;
  std::int32_t slot_ = 0;
};

class StateObserver {
public:
  using Callback = std::function<void(const Snapshot &)>;

  StateObserver(StateStore &store, Callback callback)
      : store_(store), guard_(std::make_shared<Guard>()) {
    guard_->callback = std::move(callback);
    const std::shared_ptr<Guard> guard = guard_;
    listener_id_ = store_.AddListener([guard](const Snapshot &snapshot) {
      std::lock_guard<std::mutex> lock(guard->mutex);
      if (guard->active)
        guard->callback(snapshot);
    });
  }

  ~StateObserver() {
    store_.RemoveListener(listener_id_);
    std::lock_guard<std::mutex> lock(guard_->mutex);
    guard_->active = false;
    guard_->callback = {};
  }

  StateObserver(const StateObserver &) = delete;
  StateObserver &operator=(const StateObserver &) = delete;

private:
  struct Guard {
    std::mutex mutex;
    bool active = true;
    Callback callback;
  };

  StateStore &store_;
  std::shared_ptr<Guard> guard_;
  std::uint64_t listener_id_ = 0;
};

class IncomingSmsObserver {
public:
  using Callback = std::function<void(const IncomingSms &)>;

  IncomingSmsObserver(Channel &channel, Callback callback)
      : channel_(channel), guard_(std::make_shared<Guard>()) {
    guard_->callback = std::move(callback);
    const std::shared_ptr<Guard> guard = guard_;
    listener_id_ =
        channel_.AddIncomingSmsListener([guard](const IncomingSms &sms) {
          std::lock_guard<std::mutex> lock(guard->mutex);
          if (guard->active)
            guard->callback(sms);
        });
  }

  ~IncomingSmsObserver() {
    channel_.RemoveIncomingSmsListener(listener_id_);
    std::lock_guard<std::mutex> lock(guard_->mutex);
    guard_->active = false;
    guard_->callback = {};
  }

  IncomingSmsObserver(const IncomingSmsObserver &) = delete;
  IncomingSmsObserver &operator=(const IncomingSmsObserver &) = delete;

private:
  struct Guard {
    std::mutex mutex;
    bool active = true;
    Callback callback;
  };

  Channel &channel_;
  std::shared_ptr<Guard> guard_;
  std::uint64_t listener_id_ = 0;
};

template <typename Response>
::ndk::ScopedAStatus RespondSimple(const std::shared_ptr<Response> &response,
                                   std::int32_t serial,
                                   aidl_radio::RadioError error,
                                   ::ndk::ScopedAStatus (Response::*method)(
                                       const aidl_radio::RadioResponseInfo &)) {
  if (response == nullptr) {
    return MissingResponseFunctions();
  }
  return ((*response).*method)(ResponseInfo(serial, error));
}

} // namespace cdh::radio
