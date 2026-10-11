// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "state.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <utility>

namespace cdh::radio {
namespace {

constexpr std::uint16_t kTypeBool = 1;
constexpr std::uint16_t kTypeUint8 = 2;
constexpr std::uint16_t kTypeUint64 = 4;
constexpr std::uint16_t kTypeInt32 = 5;
constexpr std::uint16_t kTypeString = 6;
constexpr std::uint16_t kTypeBytes = 7;
constexpr std::uint16_t kTypeObject = 8;
constexpr std::uint64_t kAllCapabilities = (1ULL << 17) - 1;

struct View {
  const std::uint8_t *data = nullptr;
  std::size_t size = 0;
};

struct Record {
  std::uint16_t tag = 0;
  std::uint16_t type = 0;
  View value;
};

bool Fail(std::string *error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

std::uint16_t ReadU16(const std::uint8_t *data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                    static_cast<std::uint16_t>(data[1]));
}

std::uint32_t ReadU32(const std::uint8_t *data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

std::uint64_t ReadU64(const std::uint8_t *data) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<std::uint64_t>(data[index]);
  }
  return value;
}

bool ParseRecords(View payload, std::vector<Record> *records,
                  std::string *error) {
  if (payload.size > kMaxPayloadSize) {
    return Fail(error, "TLV payload exceeds 1 MiB");
  }
  records->clear();
  std::size_t offset = 0;
  while (offset < payload.size) {
    if (payload.size - offset < 8) {
      return Fail(error, "truncated TLV header");
    }
    const std::uint16_t tag = ReadU16(payload.data + offset);
    const std::uint16_t type = ReadU16(payload.data + offset + 2);
    const std::uint32_t length = ReadU32(payload.data + offset + 4);
    if (tag == 0 || static_cast<std::uint64_t>(length) >
                        static_cast<std::uint64_t>(payload.size - offset - 8)) {
      return Fail(error, "invalid TLV tag or length");
    }
    records->push_back(Record{
        .tag = tag,
        .type = type,
        .value = View{payload.data + offset + 8, length},
    });
    offset += 8 + static_cast<std::size_t>(length);
  }
  return true;
}

const Record *FindSingle(const std::vector<Record> &records, std::uint16_t tag,
                         std::uint16_t type, bool required,
                         std::string *error) {
  const Record *result = nullptr;
  for (const Record &record : records) {
    if (record.tag != tag) {
      continue;
    }
    if (result != nullptr) {
      Fail(error, "duplicate singleton TLV field");
      return nullptr;
    }
    if (record.type != type) {
      Fail(error, "unexpected TLV field type");
      return nullptr;
    }
    result = &record;
  }
  if (required && result == nullptr) {
    Fail(error, "missing required TLV field");
  }
  return result;
}

bool ReadBool(const std::vector<Record> &records, std::uint16_t tag,
              bool *value, std::string *error) {
  const Record *record = FindSingle(records, tag, kTypeBool, true, error);
  if (record == nullptr) {
    return false;
  }
  if (record->value.size != 1 || record->value.data[0] > 1) {
    return Fail(error, "invalid TLV boolean");
  }
  *value = record->value.data[0] == 1;
  return true;
}

bool ReadUint8(const std::vector<Record> &records, std::uint16_t tag,
               std::uint8_t *value, std::string *error) {
  const Record *record = FindSingle(records, tag, kTypeUint8, true, error);
  if (record == nullptr) {
    return false;
  }
  if (record->value.size != 1) {
    return Fail(error, "invalid TLV uint8");
  }
  *value = record->value.data[0];
  return true;
}

bool ReadUint64(const std::vector<Record> &records, std::uint16_t tag,
                std::uint64_t *value, std::string *error) {
  const Record *record = FindSingle(records, tag, kTypeUint64, true, error);
  if (record == nullptr) {
    return false;
  }
  if (record->value.size != 8) {
    return Fail(error, "invalid TLV uint64");
  }
  *value = ReadU64(record->value.data);
  return true;
}

bool ReadInt32(const std::vector<Record> &records, std::uint16_t tag,
               std::int32_t *value, std::string *error) {
  const Record *record = FindSingle(records, tag, kTypeInt32, true, error);
  if (record == nullptr) {
    return false;
  }
  if (record->value.size != 4) {
    return Fail(error, "invalid TLV int32");
  }
  *value = static_cast<std::int32_t>(ReadU32(record->value.data));
  return true;
}

bool ValidUtf8(View value) {
  std::size_t index = 0;
  while (index < value.size) {
    const std::uint8_t first = value.data[index];
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if (first <= 0x7f) {
      if (first == 0) {
        return false;
      }
      ++index;
      continue;
    }
    if ((first & 0xe0) == 0xc0) {
      continuation = 1;
      codepoint = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
      continuation = 2;
      codepoint = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
      continuation = 3;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    if (continuation > value.size - index - 1) {
      return false;
    }
    for (std::size_t part = 1; part <= continuation; ++part) {
      const std::uint8_t next = value.data[index + part];
      if ((next & 0xc0) != 0x80) {
        return false;
      }
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    const std::uint32_t minimum[] = {0, 0x80, 0x800, 0x10000};
    if (codepoint < minimum[continuation] || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
      return false;
    }
    index += continuation + 1;
  }
  return true;
}

bool ReadString(const std::vector<Record> &records, std::uint16_t tag,
                std::string *value, std::string *error) {
  const Record *record = FindSingle(records, tag, kTypeString, true, error);
  if (record == nullptr) {
    return false;
  }
  if (!ValidUtf8(record->value)) {
    return Fail(error, "invalid UTF-8 TLV string");
  }
  value->assign(reinterpret_cast<const char *>(record->value.data),
                record->value.size);
  return true;
}

bool ReadBytes(const std::vector<Record> &records, std::uint16_t tag,
               std::vector<std::uint8_t> *value, std::string *error) {
  const Record *record = FindSingle(records, tag, kTypeBytes, true, error);
  if (record == nullptr)
    return false;
  value->assign(record->value.data, record->value.data + record->value.size);
  return true;
}

const Record *ReadObject(const std::vector<Record> &records, std::uint16_t tag,
                         bool required, std::string *error) {
  return FindSingle(records, tag, kTypeObject, required, error);
}

bool DecodeModem(View payload, Modem *modem, std::string *error) {
  std::vector<Record> records;
  std::uint8_t power = 0;
  std::uint8_t phone_type = 0;
  if (!ParseRecords(payload, &records, error) ||
      !ReadUint8(records, 1, &power, error) ||
      !ReadUint8(records, 2, &phone_type, error) ||
      !ReadString(records, 3, &modem->baseband_version, error)) {
    return false;
  }
  modem->power = static_cast<RadioPower>(power);
  modem->phone_type = static_cast<PhoneType>(phone_type);
  return true;
}

bool DecodeIdentity(View payload, Identity *identity, std::string *error) {
  std::vector<Record> records;
  return ParseRecords(payload, &records, error) &&
         ReadString(records, 1, &identity->country_code, error) &&
         ReadString(records, 2, &identity->national_number, error) &&
         ReadString(records, 3, &identity->e164, error) &&
         ReadString(records, 4, &identity->imsi, error) &&
         ReadString(records, 5, &identity->iccid, error) &&
         ReadString(records, 6, &identity->operator_numeric, error) &&
         ReadString(records, 7, &identity->operator_alpha, error);
}

bool DecodeSim(View payload, Sim *sim, std::string *error) {
  std::vector<Record> records;
  std::uint8_t state = 0;
  if (!ParseRecords(payload, &records, error) ||
      !ReadBool(records, 1, &sim->present, error) ||
      !ReadUint8(records, 2, &state, error)) {
    return false;
  }
  const Record *identity = ReadObject(records, 3, true, error);
  if (identity == nullptr ||
      !DecodeIdentity(identity->value, &sim->identity, error)) {
    return false;
  }
  sim->state = static_cast<SimState>(state);
  return true;
}

bool DecodeSubscription(View payload, Subscription *subscription,
                        std::string *error) {
  std::vector<Record> records;
  return ParseRecords(payload, &records, error) &&
         ReadInt32(records, 1, &subscription->id, error) &&
         ReadInt32(records, 2, &subscription->slot_id, error) &&
         ReadBool(records, 3, &subscription->active, error) &&
         ReadString(records, 4, &subscription->e164, error) &&
         ReadString(records, 5, &subscription->iccid, error);
}

bool DecodeOperator(View payload, Operator *operator_info, std::string *error) {
  std::vector<Record> records;
  return ParseRecords(payload, &records, error) &&
         ReadString(records, 1, &operator_info->numeric, error) &&
         ReadString(records, 2, &operator_info->alpha_long, error) &&
         ReadString(records, 3, &operator_info->alpha_short, error) &&
         ReadString(records, 4, &operator_info->country_iso, error);
}

bool DecodeRegistration(View payload, Registration *registration,
                        std::string *error) {
  std::vector<Record> records;
  std::uint8_t state = 0;
  std::uint8_t rat = 0;
  if (!ParseRecords(payload, &records, error) ||
      !ReadUint8(records, 1, &state, error) ||
      !ReadUint8(records, 2, &rat, error) ||
      !ReadBool(records, 3, &registration->emergency_only, error)) {
    return false;
  }
  registration->state = static_cast<RegistrationState>(state);
  registration->rat = static_cast<RadioAccessTechnology>(rat);
  return true;
}

bool DecodeSignal(View payload, Signal *signal, std::string *error) {
  std::vector<Record> records;
  return ParseRecords(payload, &records, error) &&
         ReadBool(records, 1, &signal->valid, error) &&
         ReadInt32(records, 2, &signal->rssi_dbm, error) &&
         ReadInt32(records, 3, &signal->rsrp_dbm, error) &&
         ReadInt32(records, 4, &signal->rsrq_db, error) &&
         ReadInt32(records, 5, &signal->rssnr_db, error);
}

bool DecodeNetwork(View payload, Network *network, std::string *error) {
  std::vector<Record> records;
  if (!ParseRecords(payload, &records, error)) {
    return false;
  }
  const Record *operator_info = ReadObject(records, 1, true, error);
  const Record *voice = ReadObject(records, 2, true, error);
  const Record *data = ReadObject(records, 3, true, error);
  const Record *signal = ReadObject(records, 4, true, error);
  return operator_info != nullptr && voice != nullptr && data != nullptr &&
         signal != nullptr &&
         DecodeOperator(operator_info->value, &network->operator_info, error) &&
         DecodeRegistration(voice->value, &network->voice_registration,
                            error) &&
         DecodeRegistration(data->value, &network->data_registration, error) &&
         DecodeSignal(signal->value, &network->signal, error);
}

bool DecodeDataCall(View payload, DataCall *data_call, std::string *error) {
  std::vector<Record> records;
  std::uint8_t state = 0;
  if (!ParseRecords(payload, &records, error) ||
      !ReadInt32(records, 1, &data_call->id, error) ||
      !ReadUint8(records, 2, &state, error) ||
      !ReadString(records, 3, &data_call->interface_name, error) ||
      !ReadInt32(records, 7, &data_call->mtu, error)) {
    return false;
  }
  data_call->state = static_cast<DataCallState>(state);
  for (const Record &record : records) {
    std::vector<std::string> *output = nullptr;
    if (record.tag == 4) {
      output = &data_call->addresses;
    } else if (record.tag == 5) {
      output = &data_call->dns_servers;
    } else if (record.tag == 6) {
      output = &data_call->gateways;
    } else {
      continue;
    }
    if (record.type != kTypeString || !ValidUtf8(record.value)) {
      return Fail(error, "invalid repeated data call string");
    }
    output->emplace_back(reinterpret_cast<const char *>(record.value.data),
                         record.value.size);
  }
  return true;
}

bool DecodeCall(View payload, Call *call, std::string *error) {
  std::vector<Record> records;
  std::uint8_t direction = 0;
  std::uint8_t state = 0;
  if (!ParseRecords(payload, &records, error) ||
      !ReadInt32(records, 1, &call->id, error) ||
      !ReadUint8(records, 2, &direction, error) ||
      !ReadUint8(records, 3, &state, error) ||
      !ReadString(records, 4, &call->number, error) ||
      !ReadBool(records, 5, &call->multiparty, error)) {
    return false;
  }
  call->direction = static_cast<CallDirection>(direction);
  call->state = static_cast<CallState>(state);
  return true;
}

bool DecodeDataCallList(View payload, std::vector<DataCall> *data_calls,
                        std::string *error) {
  std::vector<Record> records;
  if (!ParseRecords(payload, &records, error)) {
    return false;
  }
  data_calls->clear();
  for (const Record &record : records) {
    if (record.tag != 1) {
      continue;
    }
    if (record.type != kTypeObject) {
      return Fail(error, "invalid data call list item");
    }
    DataCall item;
    if (!DecodeDataCall(record.value, &item, error)) {
      return false;
    }
    data_calls->push_back(std::move(item));
  }
  return true;
}

bool DecodeCallList(View payload, std::vector<Call> *calls,
                    std::string *error) {
  std::vector<Record> records;
  if (!ParseRecords(payload, &records, error)) {
    return false;
  }
  calls->clear();
  for (const Record &record : records) {
    if (record.tag != 1) {
      continue;
    }
    if (record.type != kTypeObject) {
      return Fail(error, "invalid call list item");
    }
    Call item;
    if (!DecodeCall(record.value, &item, error)) {
      return false;
    }
    calls->push_back(std::move(item));
  }
  return true;
}

bool DecodeDelta(View payload, Delta *delta, std::string *error) {
  std::vector<Record> records;
  if (!ParseRecords(payload, &records, error)) {
    return false;
  }
  bool changed = false;
  for (std::uint16_t tag = 1; tag <= 6; ++tag) {
    const Record *record = ReadObject(records, tag, false, error);
    if (record == nullptr) {
      if (error != nullptr && !error->empty()) {
        return false;
      }
      continue;
    }
    changed = true;
    switch (tag) {
    case 1: {
      Modem value;
      if (!DecodeModem(record->value, &value, error))
        return false;
      delta->modem = std::move(value);
      break;
    }
    case 2: {
      Sim value;
      if (!DecodeSim(record->value, &value, error))
        return false;
      delta->sim = std::move(value);
      break;
    }
    case 3: {
      Subscription value;
      if (!DecodeSubscription(record->value, &value, error))
        return false;
      delta->subscription = std::move(value);
      break;
    }
    case 4: {
      Network value;
      if (!DecodeNetwork(record->value, &value, error))
        return false;
      delta->network = std::move(value);
      break;
    }
    case 5: {
      std::vector<DataCall> value;
      if (!DecodeDataCallList(record->value, &value, error))
        return false;
      delta->data_calls = std::move(value);
      break;
    }
    case 6: {
      std::vector<Call> value;
      if (!DecodeCallList(record->value, &value, error))
        return false;
      delta->calls = std::move(value);
      break;
    }
    default:
      break;
    }
  }
  return changed || Fail(error, "delta has no state changes");
}

bool Digits(const std::string &value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isdigit(character) != 0;
         });
}

bool ValidE164(const std::string &value) {
  return value.size() >= 5 && value.size() <= 16 && value.front() == '+' &&
         value[1] >= '1' && value[1] <= '9' && Digits(value.substr(1));
}

bool Registered(RegistrationState state) {
  return state == RegistrationState::kHome ||
         state == RegistrationState::kRoaming;
}

bool ValidateDataCall(const DataCall &data_call, std::string *error) {
  if (data_call.id <= 0 ||
      static_cast<std::uint8_t>(data_call.state) >
          static_cast<std::uint8_t>(DataCallState::kDeactivating)) {
    return Fail(error, "invalid data call identity or state");
  }
  if (data_call.state == DataCallState::kActive &&
      (data_call.interface_name.empty() || data_call.addresses.empty() ||
       data_call.dns_servers.empty() || data_call.gateways.empty() ||
       data_call.mtu < 1280 || data_call.mtu > 9000)) {
    return Fail(error, "incomplete active data call");
  }
  return true;
}

bool ValidateCall(const Call &call, std::string *error) {
  if (call.id <= 0 ||
      (call.direction != CallDirection::kIncoming &&
       call.direction != CallDirection::kOutgoing) ||
      call.state < CallState::kDialing ||
      call.state > CallState::kDisconnected ||
      (!call.number.empty() && !ValidE164(call.number))) {
    return Fail(error, "invalid call");
  }
  return true;
}

std::vector<StateStore::Listener>
CopyListeners(const std::map<std::uint64_t, StateStore::Listener> &listeners) {
  std::vector<StateStore::Listener> result;
  result.reserve(listeners.size());
  for (const auto &entry : listeners) {
    result.push_back(entry.second);
  }
  return result;
}

} // namespace

bool ValidateSnapshot(const Snapshot &snapshot, std::string *error) {
  if ((snapshot.provider != ProviderKind::kSimulated &&
       snapshot.provider != ProviderKind::kPhysical) ||
      snapshot.generation == 0 || snapshot.sequence == 0 ||
      (snapshot.capabilities & ~kAllCapabilities) != 0) {
    return Fail(error, "invalid snapshot identity or capabilities");
  }
  if (snapshot.modem.power > RadioPower::kOn ||
      snapshot.modem.phone_type > PhoneType::kCdma ||
      (snapshot.modem.power == RadioPower::kOn &&
       snapshot.modem.phone_type != PhoneType::kGsm)) {
    return Fail(error, "invalid modem state");
  }
  if (snapshot.sim.state > SimState::kError ||
      (snapshot.sim.present && (snapshot.sim.state == SimState::kAbsent ||
                                snapshot.sim.state == SimState::kUnknown)) ||
      (!snapshot.sim.present && snapshot.sim.state != SimState::kAbsent &&
       snapshot.sim.state != SimState::kUnknown)) {
    return Fail(error, "inconsistent SIM presence and state");
  }
  if (snapshot.sim.state == SimState::kReady) {
    const Identity &identity = snapshot.sim.identity;
    if (identity.country_code != "86" ||
        identity.national_number.size() != 11 ||
        !Digits(identity.national_number) ||
        identity.e164 !=
            "+" + identity.country_code + identity.national_number ||
        identity.imsi.size() != 15 || !Digits(identity.imsi) ||
        identity.iccid.size() != 20 || !Digits(identity.iccid) ||
        (identity.operator_numeric.size() != 5 &&
         identity.operator_numeric.size() != 6) ||
        !Digits(identity.operator_numeric) || identity.operator_alpha.empty() ||
        !snapshot.subscription.active || snapshot.subscription.id <= 0 ||
        snapshot.subscription.slot_id != 0 ||
        snapshot.subscription.e164 != identity.e164 ||
        snapshot.subscription.iccid != identity.iccid) {
      return Fail(error, "invalid ready SIM identity or subscription");
    }
  } else if (snapshot.subscription.active) {
    return Fail(error, "inactive SIM has active subscription");
  }
  const Registration registrations[] = {
      snapshot.network.voice_registration,
      snapshot.network.data_registration,
  };
  bool registered = false;
  for (const Registration &registration : registrations) {
    if (registration.state > RegistrationState::kRoaming ||
        registration.rat > RadioAccessTechnology::kNr ||
        (Registered(registration.state) &&
         registration.rat == RadioAccessTechnology::kUnknown)) {
      return Fail(error, "invalid network registration");
    }
    registered = registered || Registered(registration.state);
  }
  if (registered) {
    const Operator &operator_info = snapshot.network.operator_info;
    const Signal &signal = snapshot.network.signal;
    if ((operator_info.numeric.size() != 5 &&
         operator_info.numeric.size() != 6) ||
        !Digits(operator_info.numeric) || operator_info.alpha_long.empty() ||
        operator_info.country_iso.size() != 2 || !signal.valid ||
        signal.rssi_dbm < -140 || signal.rssi_dbm > -20 ||
        signal.rsrp_dbm < -156 || signal.rsrp_dbm > -44 ||
        signal.rsrq_db < -34 || signal.rsrq_db > 3 || signal.rssnr_db < -23 ||
        signal.rssnr_db > 40) {
      return Fail(error, "invalid registered network or signal");
    }
  } else if (snapshot.network.signal.valid) {
    return Fail(error, "unregistered network has valid signal");
  }
  std::set<std::int32_t> data_call_ids;
  for (const DataCall &data_call : snapshot.data_calls) {
    if (!ValidateDataCall(data_call, error) ||
        !data_call_ids.insert(data_call.id).second) {
      return Fail(error, "invalid or duplicate data call");
    }
  }
  std::set<std::int32_t> call_ids;
  for (const Call &call : snapshot.calls) {
    if (!ValidateCall(call, error) || !call_ids.insert(call.id).second) {
      return Fail(error, "invalid or duplicate call");
    }
  }
  return true;
}

bool DecodeSnapshot(const std::vector<std::uint8_t> &payload,
                    Snapshot *snapshot, std::string *error) {
  if (snapshot == nullptr) {
    return Fail(error, "snapshot output is null");
  }
  if (error != nullptr)
    error->clear();
  std::vector<Record> records;
  View view{payload.data(), payload.size()};
  std::uint8_t provider = 0;
  if (!ParseRecords(view, &records, error) ||
      !ReadUint8(records, 1, &provider, error) ||
      !ReadUint64(records, 2, &snapshot->generation, error) ||
      !ReadUint64(records, 3, &snapshot->sequence, error) ||
      !ReadUint64(records, 4, &snapshot->capabilities, error)) {
    return false;
  }
  snapshot->provider = static_cast<ProviderKind>(provider);
  const Record *modem = ReadObject(records, 5, true, error);
  const Record *sim = ReadObject(records, 6, true, error);
  const Record *subscription = ReadObject(records, 7, true, error);
  const Record *network = ReadObject(records, 8, true, error);
  if (modem == nullptr || sim == nullptr || subscription == nullptr ||
      network == nullptr ||
      !DecodeModem(modem->value, &snapshot->modem, error) ||
      !DecodeSim(sim->value, &snapshot->sim, error) ||
      !DecodeSubscription(subscription->value, &snapshot->subscription,
                          error) ||
      !DecodeNetwork(network->value, &snapshot->network, error)) {
    return false;
  }
  snapshot->data_calls.clear();
  snapshot->calls.clear();
  for (const Record &record : records) {
    if (record.tag == 9) {
      if (record.type != kTypeObject) {
        return Fail(error, "invalid snapshot data call type");
      }
      DataCall value;
      if (!DecodeDataCall(record.value, &value, error))
        return false;
      snapshot->data_calls.push_back(std::move(value));
    } else if (record.tag == 10) {
      if (record.type != kTypeObject) {
        return Fail(error, "invalid snapshot call type");
      }
      Call value;
      if (!DecodeCall(record.value, &value, error))
        return false;
      snapshot->calls.push_back(std::move(value));
    }
  }
  return ValidateSnapshot(*snapshot, error);
}

bool DecodeDeltaEvent(const std::vector<std::uint8_t> &payload,
                      std::uint64_t *generation, std::uint64_t *sequence,
                      Delta *delta, std::string *error) {
  if (generation == nullptr || sequence == nullptr || delta == nullptr) {
    return Fail(error, "delta event output is null");
  }
  if (error != nullptr)
    error->clear();
  std::vector<Record> records;
  std::uint8_t kind = 0;
  if (!ParseRecords(View{payload.data(), payload.size()}, &records, error) ||
      !ReadUint8(records, 1, &kind, error) || kind != 2 ||
      !ReadUint64(records, 2, generation, error) ||
      !ReadUint64(records, 3, sequence, error)) {
    return kind == 2 ? false : Fail(error, "event is not a delta");
  }
  for (const Record &record : records) {
    if (record.tag == 4 || record.tag == 6) {
      return Fail(error, "delta event has conflicting payload");
    }
  }
  const Record *value = ReadObject(records, 5, true, error);
  return value != nullptr && DecodeDelta(value->value, delta, error);
}

bool DecodeIncomingSmsEvent(const std::vector<std::uint8_t> &payload,
                            std::uint64_t *generation, std::uint64_t *sequence,
                            IncomingSms *sms, std::string *error) {
  if (generation == nullptr || sequence == nullptr || sms == nullptr)
    return Fail(error, "incoming SMS event output is null");
  if (error != nullptr)
    error->clear();
  std::vector<Record> records;
  std::uint8_t kind = 0;
  if (!ParseRecords(View{payload.data(), payload.size()}, &records, error) ||
      !ReadUint8(records, 1, &kind, error) || kind != 3 ||
      !ReadUint64(records, 2, generation, error) ||
      !ReadUint64(records, 3, sequence, error)) {
    return kind == 3 ? false : Fail(error, "event is not incoming SMS");
  }
  for (const Record &record : records) {
    if (record.tag == 4 || record.tag == 5)
      return Fail(error, "incoming SMS event has conflicting payload");
  }
  const Record *value = ReadObject(records, 6, true, error);
  if (value == nullptr)
    return false;
  std::vector<Record> incoming;
  if (!ParseRecords(value->value, &incoming, error) ||
      !ReadUint64(incoming, 1, &sms->id, error) ||
      !ReadBytes(incoming, 2, &sms->pdu, error)) {
    return false;
  }
  return sms->id != 0 && !sms->pdu.empty() && sms->pdu.size() <= 65535
             ? true
             : Fail(error, "invalid incoming SMS event");
}

bool DecodeDataCallPayload(const std::vector<std::uint8_t> &payload,
                           DataCall *data_call, std::string *error) {
  if (data_call == nullptr) {
    return Fail(error, "data call output is null");
  }
  if (error != nullptr)
    error->clear();
  return DecodeDataCall(View{payload.data(), payload.size()}, data_call,
                        error) &&
         ValidateDataCall(*data_call, error);
}

ApplyResult StateStore::ApplySnapshot(const Snapshot &snapshot,
                                      std::string *error,
                                      bool notify_listeners) {
  if (!ValidateSnapshot(snapshot, error)) {
    return ApplyResult::kInvalid;
  }
  std::vector<Listener> listeners;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_.has_value() && (snapshot.generation < current_->generation ||
                                 (snapshot.generation == current_->generation &&
                                  snapshot.sequence <= current_->sequence))) {
      return ApplyResult::kIgnored;
    }
    current_ = snapshot;
    if (notify_listeners)
      listeners = CopyListeners(listeners_);
  }
  if (notify_listeners)
    Notify(snapshot, listeners);
  return ApplyResult::kApplied;
}

ApplyResult StateStore::ApplyDelta(std::uint64_t generation,
                                   std::uint64_t sequence, const Delta &delta,
                                   std::string *error) {
  Snapshot updated;
  std::vector<Listener> listeners;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_.has_value()) {
      return ApplyResult::kNeedSnapshot;
    }
    if (generation < current_->generation ||
        (generation == current_->generation &&
         sequence <= current_->sequence)) {
      return ApplyResult::kIgnored;
    }
    if (generation != current_->generation ||
        sequence != current_->sequence + 1) {
      return ApplyResult::kNeedSnapshot;
    }
    updated = *current_;
    updated.sequence = sequence;
    if (delta.modem.has_value())
      updated.modem = *delta.modem;
    if (delta.sim.has_value())
      updated.sim = *delta.sim;
    if (delta.subscription.has_value()) {
      updated.subscription = *delta.subscription;
    }
    if (delta.network.has_value())
      updated.network = *delta.network;
    if (delta.data_calls.has_value())
      updated.data_calls = *delta.data_calls;
    if (delta.calls.has_value())
      updated.calls = *delta.calls;
    if (!ValidateSnapshot(updated, error)) {
      return ApplyResult::kInvalid;
    }
    current_ = updated;
    listeners = CopyListeners(listeners_);
  }
  Notify(updated, listeners);
  return ApplyResult::kApplied;
}

ApplyResult StateStore::ApplyEventSequence(std::uint64_t generation,
                                           std::uint64_t sequence,
                                           std::string *error) {
  if (generation == 0 || sequence == 0) {
    Fail(error, "invalid event sequence");
    return ApplyResult::kInvalid;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!current_.has_value())
    return ApplyResult::kNeedSnapshot;
  if (generation < current_->generation ||
      (generation == current_->generation && sequence <= current_->sequence)) {
    return ApplyResult::kIgnored;
  }
  if (generation != current_->generation ||
      sequence != current_->sequence + 1) {
    return ApplyResult::kNeedSnapshot;
  }
  current_->sequence = sequence;
  return ApplyResult::kApplied;
}

std::optional<Snapshot> StateStore::Current() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_;
}

std::uint64_t StateStore::AddListener(Listener listener) {
  if (!listener)
    return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  const std::uint64_t id = next_listener_id_++;
  listeners_.emplace(id, std::move(listener));
  return id;
}

void StateStore::NotifyCurrent() {
  std::optional<Snapshot> snapshot;
  std::vector<Listener> listeners;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = current_;
    if (snapshot.has_value())
      listeners = CopyListeners(listeners_);
  }
  if (snapshot.has_value())
    Notify(*snapshot, listeners);
}

void StateStore::RemoveListener(std::uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  listeners_.erase(id);
}

void StateStore::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  current_.reset();
}

void StateStore::Notify(const Snapshot &snapshot,
                        const std::vector<Listener> &listeners) const {
  for (const Listener &listener : listeners) {
    listener(snapshot);
  }
}

} // namespace cdh::radio
