// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cdh::radio {

constexpr std::size_t kMaxPayloadSize = 1U << 20;

enum class ProviderKind : std::uint8_t {
  kUnknown = 0,
  kSimulated = 1,
  kPhysical = 2,
};

enum class RadioPower : std::uint8_t {
  kUnavailable = 0,
  kOff = 1,
  kOn = 2,
};

enum class PhoneType : std::uint8_t {
  kNone = 0,
  kGsm = 1,
  kCdma = 2,
};

enum class SimState : std::uint8_t {
  kUnknown = 0,
  kAbsent = 1,
  kPinRequired = 2,
  kPukRequired = 3,
  kReady = 4,
  kError = 5,
};

enum class RegistrationState : std::uint8_t {
  kUnavailable = 0,
  kNotRegistered = 1,
  kSearching = 2,
  kDenied = 3,
  kHome = 4,
  kRoaming = 5,
};

enum class RadioAccessTechnology : std::uint8_t {
  kUnknown = 0,
  kGprs = 1,
  kEdge = 2,
  kUmts = 3,
  kLte = 4,
  kNr = 5,
};

enum class DataCallState : std::uint8_t {
  kInactive = 0,
  kActivating = 1,
  kActive = 2,
  kDeactivating = 3,
};

enum class CallDirection : std::uint8_t {
  kUnknown = 0,
  kIncoming = 1,
  kOutgoing = 2,
};

enum class CallState : std::uint8_t {
  kUnknown = 0,
  kDialing = 1,
  kAlerting = 2,
  kIncoming = 3,
  kWaiting = 4,
  kActive = 5,
  kHeld = 6,
  kDisconnected = 7,
};

struct Identity {
  std::string country_code;
  std::string national_number;
  std::string e164;
  std::string imsi;
  std::string iccid;
  std::string operator_numeric;
  std::string operator_alpha;
};

struct Modem {
  RadioPower power = RadioPower::kUnavailable;
  PhoneType phone_type = PhoneType::kNone;
  std::string baseband_version;
};

struct Sim {
  bool present = false;
  SimState state = SimState::kUnknown;
  Identity identity;
};

struct Subscription {
  std::int32_t id = 0;
  std::int32_t slot_id = 0;
  bool active = false;
  std::string e164;
  std::string iccid;
};

struct Operator {
  std::string numeric;
  std::string alpha_long;
  std::string alpha_short;
  std::string country_iso;
};

struct Registration {
  RegistrationState state = RegistrationState::kUnavailable;
  RadioAccessTechnology rat = RadioAccessTechnology::kUnknown;
  bool emergency_only = false;
};

struct Signal {
  bool valid = false;
  std::int32_t rssi_dbm = 0;
  std::int32_t rsrp_dbm = 0;
  std::int32_t rsrq_db = 0;
  std::int32_t rssnr_db = 0;
};

struct Network {
  Operator operator_info;
  Registration voice_registration;
  Registration data_registration;
  Signal signal;
};

struct DataCall {
  std::int32_t id = 0;
  DataCallState state = DataCallState::kInactive;
  std::string interface_name;
  std::vector<std::string> addresses;
  std::vector<std::string> dns_servers;
  std::vector<std::string> gateways;
  std::int32_t mtu = 0;
};

struct Call {
  std::int32_t id = 0;
  CallDirection direction = CallDirection::kUnknown;
  CallState state = CallState::kUnknown;
  std::string number;
  bool multiparty = false;
};

struct IncomingSms {
  std::uint64_t id = 0;
  std::vector<std::uint8_t> pdu;
};

struct Snapshot {
  ProviderKind provider = ProviderKind::kUnknown;
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  std::uint64_t capabilities = 0;
  Modem modem;
  Sim sim;
  Subscription subscription;
  Network network;
  std::vector<DataCall> data_calls;
  std::vector<Call> calls;
};

struct Delta {
  std::optional<Modem> modem;
  std::optional<Sim> sim;
  std::optional<Subscription> subscription;
  std::optional<Network> network;
  std::optional<std::vector<DataCall>> data_calls;
  std::optional<std::vector<Call>> calls;
};

enum class ApplyResult {
  kApplied,
  kIgnored,
  kNeedSnapshot,
  kInvalid,
};

class StateStore {
public:
  using Listener = std::function<void(const Snapshot &)>;

  ApplyResult ApplySnapshot(const Snapshot &snapshot, std::string *error,
                            bool notify_listeners = true);
  ApplyResult ApplyDelta(std::uint64_t generation, std::uint64_t sequence,
                         const Delta &delta, std::string *error);
  ApplyResult ApplyEventSequence(std::uint64_t generation,
                                 std::uint64_t sequence, std::string *error);
  std::optional<Snapshot> Current() const;
  std::uint64_t AddListener(Listener listener);
  void RemoveListener(std::uint64_t id);
  void NotifyCurrent();
  void Reset();

private:
  void Notify(const Snapshot &snapshot,
              const std::vector<Listener> &listeners) const;

  mutable std::mutex mutex_;
  std::optional<Snapshot> current_;
  std::map<std::uint64_t, Listener> listeners_;
  std::uint64_t next_listener_id_ = 1;
};

bool DecodeSnapshot(const std::vector<std::uint8_t> &payload,
                    Snapshot *snapshot, std::string *error);
bool DecodeDeltaEvent(const std::vector<std::uint8_t> &payload,
                      std::uint64_t *generation, std::uint64_t *sequence,
                      Delta *delta, std::string *error);
bool DecodeIncomingSmsEvent(const std::vector<std::uint8_t> &payload,
                            std::uint64_t *generation, std::uint64_t *sequence,
                            IncomingSms *sms, std::string *error);
bool DecodeDataCallPayload(const std::vector<std::uint8_t> &payload,
                           DataCall *data_call, std::string *error);
bool ValidateSnapshot(const Snapshot &snapshot, std::string *error);

} // namespace cdh::radio
