// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "channel.h"

#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace cdh::radio {
namespace {

constexpr std::uint32_t kMagic = 0x4344524d;
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderSize = 64;
constexpr std::uint16_t kTypeBool = 1;
constexpr std::uint16_t kTypeUint8 = 2;
constexpr std::uint16_t kTypeUint16 = 3;
constexpr std::uint16_t kTypeUint64 = 4;
constexpr std::uint16_t kTypeInt32 = 5;
constexpr std::uint16_t kTypeString = 6;
constexpr std::uint16_t kTypeBytes = 7;
constexpr std::uint16_t kTypeObject = 8;

enum class FrameKind : std::uint16_t {
  kHello = 1,
  kHelloResponse = 2,
  kSnapshotRequest = 3,
  kSnapshot = 4,
  kCommand = 5,
  kResult = 6,
  kDelta = 7,
  kIncomingSms = 8,
  kPing = 9,
  kPong = 10,
};

struct FrameHeader {
  FrameKind kind = FrameKind::kHello;
  std::uint16_t flags = 0;
  Status status = Status::kOk;
  std::uint64_t session_id = 0;
  std::uint64_t boot_generation = 0;
  std::uint64_t provider_generation = 0;
  std::uint64_t sequence = 0;
  std::uint64_t request_id = 0;
  std::int64_t deadline_unix_milli = 0;
  std::uint32_t payload_length = 0;
};

struct Frame {
  FrameHeader header;
  std::vector<std::uint8_t> payload = {};
};

struct Record {
  std::uint16_t tag = 0;
  std::uint16_t type = 0;
  std::vector<std::uint8_t> value;
};

void PutU16(std::uint8_t *output, std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 8);
  output[1] = static_cast<std::uint8_t>(value);
}

void PutU32(std::uint8_t *output, std::uint32_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 24);
  output[1] = static_cast<std::uint8_t>(value >> 16);
  output[2] = static_cast<std::uint8_t>(value >> 8);
  output[3] = static_cast<std::uint8_t>(value);
}

void PutU64(std::uint8_t *output, std::uint64_t value) {
  for (int index = 7; index >= 0; --index) {
    output[index] = static_cast<std::uint8_t>(value);
    value >>= 8;
  }
}

std::uint16_t GetU16(const std::uint8_t *input) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(input[0]) << 8) |
      static_cast<std::uint16_t>(input[1]));
}

std::uint32_t GetU32(const std::uint8_t *input) {
  return (static_cast<std::uint32_t>(input[0]) << 24) |
         (static_cast<std::uint32_t>(input[1]) << 16) |
         (static_cast<std::uint32_t>(input[2]) << 8) |
         static_cast<std::uint32_t>(input[3]);
}

std::uint64_t GetU64(const std::uint8_t *input) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<std::uint64_t>(input[index]);
  }
  return value;
}

bool ReadFull(int file_descriptor, std::uint8_t *output, std::size_t size,
              std::string *error) {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t count = read(file_descriptor, output + offset, size - offset);
    if (count == 0) {
      *error = "Radio channel EOF";
      return false;
    }
    if (count < 0) {
      if (errno == EINTR)
        continue;
      *error = std::string("Radio channel read: ") + std::strerror(errno);
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

bool WriteFull(int file_descriptor, const std::uint8_t *input, std::size_t size,
               std::string *error) {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t count = write(file_descriptor, input + offset, size - offset);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      *error = std::string("Radio channel write: ") + std::strerror(errno);
      return false;
    }
    if (count == 0) {
      *error = "Radio channel short write";
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

bool ReadFrame(int file_descriptor, Frame *frame, std::string *error) {
  std::uint8_t bytes[kHeaderSize] = {};
  if (!ReadFull(file_descriptor, bytes, sizeof(bytes), error))
    return false;
  if (GetU32(bytes) != kMagic || GetU16(bytes + 4) != kVersion) {
    *error = "Radio frame magic or version mismatch";
    return false;
  }
  const std::uint16_t kind = GetU16(bytes + 6);
  const std::uint16_t status = GetU16(bytes + 10);
  if (kind < static_cast<std::uint16_t>(FrameKind::kHello) ||
      kind > static_cast<std::uint16_t>(FrameKind::kPong) ||
      GetU16(bytes + 8) != 0 ||
      status > static_cast<std::uint16_t>(Status::kInternalError)) {
    *error = "Radio frame header fields are invalid";
    return false;
  }
  frame->header = FrameHeader{
      .kind = static_cast<FrameKind>(kind),
      .flags = 0,
      .status = static_cast<Status>(status),
      .session_id = GetU64(bytes + 12),
      .boot_generation = GetU64(bytes + 20),
      .provider_generation = GetU64(bytes + 28),
      .sequence = GetU64(bytes + 36),
      .request_id = GetU64(bytes + 44),
      .deadline_unix_milli = static_cast<std::int64_t>(GetU64(bytes + 52)),
      .payload_length = GetU32(bytes + 60),
  };
  if (frame->header.payload_length > kMaxPayloadSize) {
    *error = "Radio frame payload exceeds 1 MiB";
    return false;
  }
  frame->payload.resize(frame->header.payload_length);
  return frame->payload.empty() ||
         ReadFull(file_descriptor, frame->payload.data(), frame->payload.size(),
                  error);
}

bool WriteFrame(int file_descriptor, const Frame &frame, std::string *error) {
  if (frame.payload.size() > kMaxPayloadSize ||
      frame.payload.size() != frame.header.payload_length) {
    *error = "Radio frame payload length mismatch";
    return false;
  }
  std::uint8_t bytes[kHeaderSize] = {};
  PutU32(bytes, kMagic);
  PutU16(bytes + 4, kVersion);
  PutU16(bytes + 6, static_cast<std::uint16_t>(frame.header.kind));
  PutU16(bytes + 8, frame.header.flags);
  PutU16(bytes + 10, static_cast<std::uint16_t>(frame.header.status));
  PutU64(bytes + 12, frame.header.session_id);
  PutU64(bytes + 20, frame.header.boot_generation);
  PutU64(bytes + 28, frame.header.provider_generation);
  PutU64(bytes + 36, frame.header.sequence);
  PutU64(bytes + 44, frame.header.request_id);
  PutU64(bytes + 52,
         static_cast<std::uint64_t>(frame.header.deadline_unix_milli));
  PutU32(bytes + 60, frame.header.payload_length);
  return WriteFull(file_descriptor, bytes, sizeof(bytes), error) &&
         (frame.payload.empty() ||
          WriteFull(file_descriptor, frame.payload.data(), frame.payload.size(),
                    error));
}

class TlvBuilder {
public:
  void Bool(std::uint16_t tag, bool value) {
    Add(tag, kTypeBool,
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(value ? 1 : 0)});
  }

  void Uint8(std::uint16_t tag, std::uint8_t value) {
    Add(tag, kTypeUint8, std::vector<std::uint8_t>{value});
  }

  void Uint16(std::uint16_t tag, std::uint16_t value) {
    std::vector<std::uint8_t> bytes(2);
    PutU16(bytes.data(), value);
    Add(tag, kTypeUint16, bytes);
  }

  void Uint64(std::uint16_t tag, std::uint64_t value) {
    std::vector<std::uint8_t> bytes(8);
    PutU64(bytes.data(), value);
    Add(tag, kTypeUint64, bytes);
  }

  void Int32(std::uint16_t tag, std::int32_t value) {
    std::vector<std::uint8_t> bytes(4);
    PutU32(bytes.data(), static_cast<std::uint32_t>(value));
    Add(tag, kTypeInt32, bytes);
  }

  void String(std::uint16_t tag, const std::string &value) {
    Add(tag, kTypeString,
        std::vector<std::uint8_t>(value.begin(), value.end()));
  }

  void Bytes(std::uint16_t tag, const std::vector<std::uint8_t> &value) {
    Add(tag, kTypeBytes, value);
  }

  void Object(std::uint16_t tag, const std::vector<std::uint8_t> &value) {
    Add(tag, kTypeObject, value);
  }

  bool Finish(std::vector<std::uint8_t> *output, std::string *error) const {
    if (!error_.empty()) {
      *error = error_;
      return false;
    }
    *output = bytes_;
    return true;
  }

private:
  void Add(std::uint16_t tag, std::uint16_t type,
           const std::vector<std::uint8_t> &value) {
    if (!error_.empty())
      return;
    if (tag == 0 || value.size() > kMaxPayloadSize - 8 ||
        bytes_.size() > kMaxPayloadSize - 8 - value.size()) {
      error_ = "command TLV exceeds protocol bounds";
      return;
    }
    const std::size_t offset = bytes_.size();
    bytes_.resize(offset + 8 + value.size());
    PutU16(bytes_.data() + offset, tag);
    PutU16(bytes_.data() + offset + 2, type);
    PutU32(bytes_.data() + offset + 4,
           static_cast<std::uint32_t>(value.size()));
    std::copy(value.begin(), value.end(), bytes_.begin() + offset + 8);
  }

  std::vector<std::uint8_t> bytes_;
  std::string error_;
};

bool ParseRecords(const std::vector<std::uint8_t> &payload,
                  std::vector<Record> *records, std::string *error) {
  if (payload.size() > kMaxPayloadSize) {
    *error = "result TLV exceeds 1 MiB";
    return false;
  }
  records->clear();
  std::size_t offset = 0;
  while (offset < payload.size()) {
    if (payload.size() - offset < 8) {
      *error = "truncated result TLV";
      return false;
    }
    const std::uint16_t tag = GetU16(payload.data() + offset);
    const std::uint16_t type = GetU16(payload.data() + offset + 2);
    const std::uint32_t length = GetU32(payload.data() + offset + 4);
    if (tag == 0 ||
        static_cast<std::uint64_t>(length) >
            static_cast<std::uint64_t>(payload.size() - offset - 8)) {
      *error = "invalid result TLV tag or length";
      return false;
    }
    records->push_back(Record{
        .tag = tag,
        .type = type,
        .value = std::vector<std::uint8_t>(
            payload.begin() + static_cast<std::ptrdiff_t>(offset + 8),
            payload.begin() + static_cast<std::ptrdiff_t>(offset + 8 + length)),
    });
    offset += 8 + static_cast<std::size_t>(length);
  }
  return true;
}

const Record *Single(const std::vector<Record> &records, std::uint16_t tag,
                     std::uint16_t type, bool required, std::string *error) {
  const Record *result = nullptr;
  for (const Record &record : records) {
    if (record.tag != tag)
      continue;
    if (result != nullptr || record.type != type) {
      *error = "duplicate or mistyped result TLV";
      return nullptr;
    }
    result = &record;
  }
  if (required && result == nullptr) {
    *error = "missing result TLV";
  }
  return result;
}

bool EncodeCommand(std::uint64_t id, std::uint64_t provider_generation,
                   const Command &command, std::vector<std::uint8_t> *payload,
                   std::string *error) {
  if (id == 0 || provider_generation == 0 ||
      command.kind < CommandKind::kGetSnapshot ||
      command.kind > CommandKind::kHangupCall) {
    *error = "invalid command identity or kind";
    return false;
  }
  TlvBuilder builder;
  builder.Uint64(1, id);
  builder.Uint64(2, provider_generation);
  builder.Uint16(3, static_cast<std::uint16_t>(command.kind));
  switch (command.kind) {
  case CommandKind::kSetRadioPower:
    if (command.radio_power != RadioPower::kOff &&
        command.radio_power != RadioPower::kOn) {
      *error = "invalid radio power command";
      return false;
    }
    builder.Uint8(4, static_cast<std::uint8_t>(command.radio_power));
    break;
  case CommandKind::kSetupDataCall:
  case CommandKind::kDeactivateDataCall: {
    TlvBuilder nested;
    nested.Int32(1, command.data_call.id);
    nested.String(2, command.data_call.apn);
    nested.String(3, command.data_call.protocol);
    std::vector<std::uint8_t> value;
    if (!nested.Finish(&value, error))
      return false;
    builder.Object(5, value);
    break;
  }
  case CommandKind::kSendSms:
  case CommandKind::kInjectSms: {
    if (command.sms.pdu.size() > 65535) {
      *error = "SMS PDU exceeds protocol bounds";
      return false;
    }
    TlvBuilder nested;
    nested.String(1, command.sms.destination);
    nested.Bytes(2, command.sms.pdu);
    std::vector<std::uint8_t> value;
    if (!nested.Finish(&value, error))
      return false;
    builder.Object(6, value);
    break;
  }
  case CommandKind::kDial:
  case CommandKind::kInjectCall:
  case CommandKind::kAnswerCall:
  case CommandKind::kHoldCall:
  case CommandKind::kRejectCall:
  case CommandKind::kHangupCall: {
    TlvBuilder nested;
    nested.Int32(1, command.call.id);
    nested.String(2, command.call.number);
    std::vector<std::uint8_t> value;
    if (!nested.Finish(&value, error))
      return false;
    builder.Object(7, value);
    break;
  }
  case CommandKind::kGetSnapshot:
    break;
  }
  return builder.Finish(payload, error);
}

bool DecodeResult(const std::vector<std::uint8_t> &payload, std::uint64_t *id,
                  std::uint64_t *provider_generation, CommandResult *result,
                  std::string *error) {
  std::vector<Record> records;
  if (!ParseRecords(payload, &records, error))
    return false;
  const Record *id_record = Single(records, 1, kTypeUint64, true, error);
  const Record *generation_record =
      Single(records, 2, kTypeUint64, true, error);
  const Record *status_record = Single(records, 3, kTypeUint16, true, error);
  if (id_record == nullptr || generation_record == nullptr ||
      status_record == nullptr || id_record->value.size() != 8 ||
      generation_record->value.size() != 8 ||
      status_record->value.size() != 2) {
    if (error->empty())
      *error = "invalid result scalar length";
    return false;
  }
  *id = GetU64(id_record->value.data());
  *provider_generation = GetU64(generation_record->value.data());
  const std::uint16_t status = GetU16(status_record->value.data());
  if (*id == 0 || *provider_generation == 0 ||
      status > static_cast<std::uint16_t>(Status::kInternalError)) {
    *error = "invalid result identity or status";
    return false;
  }
  result->status = static_cast<Status>(status);
  result->data_call.reset();
  const Record *data_call = Single(records, 4, kTypeObject, false, error);
  if (!error->empty())
    return false;
  if (data_call != nullptr) {
    if (result->status != Status::kOk) {
      *error = "failed result carries a data call";
      return false;
    }
    DataCall decoded;
    if (!DecodeDataCallPayload(data_call->value, &decoded, error)) {
      return false;
    }
    result->data_call = std::move(decoded);
  }
  return true;
}

std::int64_t DeadlineFromNow(std::chrono::milliseconds timeout) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() +
         timeout.count();
}

bool ConfigureBinaryTTY(int file_descriptor, std::string *error) {
  if (isatty(file_descriptor) == 0)
    return true;
  struct termios settings {};
  if (tcgetattr(file_descriptor, &settings) < 0) {
    *error = std::string("read Radio TTY settings: ") + std::strerror(errno);
    return false;
  }
  cfmakeraw(&settings);
  if (tcsetattr(file_descriptor, TCSANOW, &settings) < 0) {
    *error = std::string("configure Radio TTY: ") + std::strerror(errno);
    return false;
  }
  return true;
}

bool ReadGuestToken(std::uint64_t *token, std::string *error) {
  const int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    *error = std::string("open Radio token source: ") + std::strerror(errno);
    return false;
  }
  const bool read =
      ReadFull(descriptor, reinterpret_cast<std::uint8_t *>(token),
               sizeof(*token), error);
  const int close_result = close(descriptor);
  if (!read)
    return false;
  if (close_result < 0) {
    *error = std::string("close Radio token source: ") + std::strerror(errno);
    return false;
  }
  if (*token == 0) {
    *error = "Radio token source returned zero";
    return false;
  }
  return true;
}

} // namespace

class Channel::Impl {
public:
  explicit Impl(std::string device_path)
      : device_path_(std::move(device_path)) {}

  ~Impl() { Stop(); }

  bool Start(StateStore *state_store, std::string *error) {
    const int descriptor =
        open(device_path_.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (descriptor < 0) {
      if (error != nullptr) {
        *error = std::string("open Radio channel: ") + std::strerror(errno);
      }
      return false;
    }
    if (!Start(descriptor, state_store, error)) {
      close(descriptor);
      return false;
    }
    return true;
  }

  bool Start(int owned_file_descriptor, StateStore *state_store,
             std::string *error) {
    Stop();
    if (owned_file_descriptor < 0 || state_store == nullptr) {
      if (error != nullptr)
        *error = "invalid Radio channel start input";
      return false;
    }
    if (!ConfigureBinaryTTY(owned_file_descriptor, error))
      return false;
    std::uint64_t guest_token = 0;
    if (!ReadGuestToken(&guest_token, error))
      return false;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      state_store_ = state_store;
      state_store_->Reset();
      session_id_ = 0;
      boot_generation_ = 0;
      provider_generation_ = 0;
      sequence_ = 0;
      guest_token_ = guest_token;
      ready_ = false;
      running_ = true;
      stopping_ = false;
      last_error_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      queue_.clear();
    }
    file_descriptor_.store(owned_file_descriptor);
    next_request_id_.store(1);
    next_snapshot_request_id_.store(1ULL << 63);
    dispatcher_thread_ = std::thread([this] { DispatcherLoop(); });
    reader_thread_ = std::thread([this] { ReaderLoop(); });
    return true;
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      stopping_ = true;
      ready_ = false;
    }
    CloseFileDescriptor();
    queue_cv_.notify_all();
    status_cv_.notify_all();
    if (reader_thread_.joinable())
      reader_thread_.join();
    queue_cv_.notify_all();
    if (dispatcher_thread_.joinable())
      dispatcher_thread_.join();
    FailPending(Status::kRadioUnavailable);
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      running_ = false;
      ready_ = false;
      state_store_ = nullptr;
    }
    status_cv_.notify_all();
  }

  bool WaitUntilReady(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(status_mutex_);
    status_cv_.wait_for(lock, timeout, [this] { return ready_ || !running_; });
    return ready_;
  }

  bool WaitUntilStopped(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(status_mutex_);
    return status_cv_.wait_for(lock, timeout, [this] { return !running_; });
  }

  bool IsReady() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return ready_;
  }

  bool IsRunning() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return running_;
  }

  std::string LastError() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return last_error_;
  }

  std::uint64_t SessionId() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return session_id_;
  }

  std::uint64_t BootGeneration() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return boot_generation_;
  }

  std::uint64_t ProviderGeneration() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return provider_generation_;
  }

  std::uint64_t AddIncomingSmsListener(IncomingSmsListener listener) {
    if (!listener)
      return 0;
    std::lock_guard<std::mutex> lock(sms_listener_mutex_);
    const std::uint64_t id = next_sms_listener_id_++;
    sms_listeners_.emplace(id, std::move(listener));
    return id;
  }

  void RemoveIncomingSmsListener(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(sms_listener_mutex_);
    sms_listeners_.erase(id);
  }

  CommandResult Request(const Command &command,
                        std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
      return CommandResult{.status = Status::kDeadlineExceeded,
                           .data_call = std::nullopt};
    }
    std::uint64_t session_id = 0;
    std::uint64_t boot_generation = 0;
    std::uint64_t provider_generation = 0;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      if (!running_ || !ready_) {
        return CommandResult{.status = Status::kRadioUnavailable,
                             .data_call = std::nullopt};
      }
      session_id = session_id_;
      boot_generation = boot_generation_;
      provider_generation = provider_generation_;
    }
    const std::uint64_t request_id = next_request_id_.fetch_add(1);
    std::vector<std::uint8_t> payload;
    std::string error;
    if (!EncodeCommand(request_id, provider_generation, command, &payload,
                       &error)) {
      return CommandResult{.status = Status::kInvalidArgument,
                           .data_call = std::nullopt};
    }
    auto pending = std::make_shared<Pending>();
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      pending_.emplace(request_id, pending);
    }
    const Frame frame{
        .header =
            FrameHeader{
                .kind = FrameKind::kCommand,
                .session_id = session_id,
                .boot_generation = boot_generation,
                .provider_generation = provider_generation,
                .request_id = request_id,
                .deadline_unix_milli = DeadlineFromNow(timeout),
                .payload_length = static_cast<std::uint32_t>(payload.size()),
            },
        .payload = std::move(payload),
    };
    if (!SendFrame(frame, &error)) {
      {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(request_id);
      }
      SetStopped(error);
      return CommandResult{.status = Status::kRadioUnavailable,
                           .data_call = std::nullopt};
    }
    std::unique_lock<std::mutex> lock(pending->mutex);
    if (!pending->cv.wait_for(lock, timeout,
                              [&pending] { return pending->done; })) {
      lock.unlock();
      std::lock_guard<std::mutex> pending_lock(pending_mutex_);
      pending_.erase(request_id);
      return CommandResult{.status = Status::kDeadlineExceeded,
                           .data_call = std::nullopt};
    }
    return pending->result;
  }

private:
  struct QueueItem {
    std::optional<Frame> frame = std::nullopt;
    std::string error = {};
  };

  struct Pending {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    CommandResult result;
  };

  void ReaderLoop() {
    while (true) {
      const int descriptor = file_descriptor_.load();
      if (descriptor < 0)
        return;
      Frame frame;
      std::string error;
      if (!ReadFrame(descriptor, &frame, &error)) {
        Push(QueueItem{.frame = std::nullopt, .error = std::move(error)});
        return;
      }
      Push(QueueItem{.frame = std::move(frame), .error = {}});
    }
  }

  void DispatcherLoop() {
    while (true) {
      QueueItem item;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] {
          std::lock_guard<std::mutex> status_lock(status_mutex_);
          return !queue_.empty() || stopping_;
        });
        if (queue_.empty())
          return;
        item = std::move(queue_.front());
        queue_.pop_front();
      }
      if (!item.frame.has_value()) {
        SetStopped(item.error);
        return;
      }
      std::string error;
      if (!HandleFrame(*item.frame, &error)) {
        SetStopped(error);
        return;
      }
    }
  }

  void Push(QueueItem item) {
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      queue_.push_back(std::move(item));
    }
    queue_cv_.notify_one();
  }

  bool HandleFrame(const Frame &frame, std::string *error) {
    if (frame.header.kind == FrameKind::kHello) {
      return HandleHello(frame, error);
    }
    std::uint64_t session_id = 0;
    std::uint64_t boot_generation = 0;
    std::uint64_t provider_generation = 0;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      if (!ready_)
        return true;
      session_id = session_id_;
      boot_generation = boot_generation_;
      provider_generation = provider_generation_;
    }
    if (frame.header.session_id != session_id ||
        frame.header.boot_generation != boot_generation) {
      return true;
    }
    switch (frame.header.kind) {
    case FrameKind::kSnapshot:
      return HandleSnapshot(frame, error);
    case FrameKind::kDelta:
      return HandleDelta(frame, error);
    case FrameKind::kResult:
      if (frame.header.provider_generation != provider_generation) {
        return true;
      }
      return HandleResult(frame, error);
    case FrameKind::kPing:
      return SendPong(frame, error);
    case FrameKind::kIncomingSms:
      return HandleIncomingSms(frame, error);
    case FrameKind::kPong:
    case FrameKind::kHelloResponse:
    case FrameKind::kSnapshotRequest:
    case FrameKind::kCommand:
    case FrameKind::kHello:
      return true;
    }
    return true;
  }

  bool HandleHello(const Frame &frame, std::string *error) {
    if (frame.header.status != Status::kOk || frame.header.session_id == 0 ||
        frame.header.boot_generation == 0 ||
        frame.header.provider_generation == 0 || frame.header.sequence == 0 ||
        frame.header.request_id != 0 || frame.header.deadline_unix_milli != 0) {
      *error = "invalid Radio hello header";
      return false;
    }
    Snapshot snapshot;
    if (!DecodeSnapshot(frame.payload, &snapshot, error) ||
        snapshot.generation != frame.header.provider_generation ||
        snapshot.sequence != frame.header.sequence) {
      if (error->empty())
        *error = "Radio hello snapshot does not match header";
      return false;
    }
    StateStore *store = nullptr;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      store = state_store_;
    }
    if (store == nullptr) {
      *error = "Radio state store is unavailable";
      return false;
    }
    bool unchanged = false;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      unchanged =
          ready_ && session_id_ == frame.header.session_id &&
          boot_generation_ == frame.header.boot_generation &&
          provider_generation_ == frame.header.provider_generation &&
          sequence_ == frame.header.sequence;
    }
    if (!unchanged) {
      store->Reset();
      if (store->ApplySnapshot(snapshot, error, false) !=
          ApplyResult::kApplied) {
        if (error->empty())
          *error = "Radio hello snapshot was not applied";
        return false;
      }
    }
    const Frame response{
        .header =
            FrameHeader{
                .kind = FrameKind::kHelloResponse,
                .session_id = frame.header.session_id,
                .boot_generation = frame.header.boot_generation,
                .provider_generation = frame.header.provider_generation,
                .sequence = frame.header.sequence,
                .request_id = guest_token_,
            },
        .payload = {},
    };
    if (!SendFrame(response, error)) {
      if (!unchanged)
        store->Reset();
      return false;
    }
    if (unchanged)
      return true;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      session_id_ = frame.header.session_id;
      boot_generation_ = frame.header.boot_generation;
      provider_generation_ = frame.header.provider_generation;
      sequence_ = frame.header.sequence;
      ready_ = true;
    }
    store->NotifyCurrent();
    status_cv_.notify_all();
    return true;
  }

  bool HandleSnapshot(const Frame &frame, std::string *error) {
    Snapshot snapshot;
    if (!DecodeSnapshot(frame.payload, &snapshot, error) ||
        snapshot.generation != frame.header.provider_generation ||
        snapshot.sequence != frame.header.sequence) {
      if (error->empty())
        *error = "Radio snapshot does not match header";
      return false;
    }
    StateStore *store = nullptr;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      store = state_store_;
    }
    const ApplyResult applied = store->ApplySnapshot(snapshot, error);
    if (applied == ApplyResult::kInvalid)
      return false;
    if (applied == ApplyResult::kApplied) {
      std::lock_guard<std::mutex> lock(status_mutex_);
      provider_generation_ = snapshot.generation;
      sequence_ = snapshot.sequence;
    }
    return true;
  }

  bool HandleDelta(const Frame &frame, std::string *error) {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    Delta delta;
    if (!DecodeDeltaEvent(frame.payload, &generation, &sequence, &delta,
                          error) ||
        generation != frame.header.provider_generation ||
        sequence != frame.header.sequence) {
      if (error->empty())
        *error = "Radio delta does not match header";
      return false;
    }
    StateStore *store = nullptr;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      store = state_store_;
    }
    const ApplyResult applied =
        store->ApplyDelta(generation, sequence, delta, error);
    if (applied == ApplyResult::kInvalid)
      return false;
    if (applied == ApplyResult::kApplied) {
      std::lock_guard<std::mutex> lock(status_mutex_);
      provider_generation_ = generation;
      sequence_ = sequence;
      return true;
    }
    if (applied == ApplyResult::kNeedSnapshot) {
      return SendSnapshotRequest(error);
    }
    return true;
  }

  bool HandleResult(const Frame &frame, std::string *error) {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    CommandResult result;
    if (!DecodeResult(frame.payload, &id, &generation, &result, error) ||
        id != frame.header.request_id ||
        generation != frame.header.provider_generation ||
        result.status != frame.header.status) {
      if (error->empty())
        *error = "Radio result does not match header";
      return false;
    }
    std::shared_ptr<Pending> pending;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      const auto found = pending_.find(id);
      if (found == pending_.end())
        return true;
      pending = found->second;
      pending_.erase(found);
    }
    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      pending->result = std::move(result);
      pending->done = true;
    }
    pending->cv.notify_all();
    return true;
  }

  bool HandleIncomingSms(const Frame &frame, std::string *error) {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    IncomingSms sms;
    if (!DecodeIncomingSmsEvent(frame.payload, &generation, &sequence, &sms,
                                error) ||
        generation != frame.header.provider_generation ||
        sequence != frame.header.sequence) {
      if (error->empty())
        *error = "incoming SMS event does not match header";
      return false;
    }
    StateStore *store = nullptr;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      store = state_store_;
    }
    const ApplyResult applied =
        store->ApplyEventSequence(generation, sequence, error);
    if (applied == ApplyResult::kInvalid)
      return false;
    if (applied == ApplyResult::kNeedSnapshot)
      return SendSnapshotRequest(error);
    if (applied == ApplyResult::kIgnored)
      return true;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      provider_generation_ = generation;
      sequence_ = sequence;
    }
    std::vector<IncomingSmsListener> listeners;
    {
      std::lock_guard<std::mutex> lock(sms_listener_mutex_);
      listeners.reserve(sms_listeners_.size());
      for (const auto &entry : sms_listeners_)
        listeners.push_back(entry.second);
    }
    for (const IncomingSmsListener &listener : listeners)
      listener(sms);
    return true;
  }

  bool SendSnapshotRequest(std::string *error) {
    Frame frame;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      frame.header = FrameHeader{
          .kind = FrameKind::kSnapshotRequest,
          .session_id = session_id_,
          .boot_generation = boot_generation_,
          .provider_generation = provider_generation_,
          .sequence = sequence_,
          .request_id = next_snapshot_request_id_.fetch_add(1),
      };
    }
    return SendFrame(frame, error);
  }

  bool SendPong(const Frame &ping, std::string *error) {
    Frame pong;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      pong.header = FrameHeader{
          .kind = FrameKind::kPong,
          .session_id = session_id_,
          .boot_generation = boot_generation_,
          .provider_generation = provider_generation_,
          .sequence = sequence_,
          .request_id = ping.header.request_id,
      };
    }
    return SendFrame(pong, error);
  }

  bool SendFrame(const Frame &frame, std::string *error) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    const int descriptor = file_descriptor_.load();
    if (descriptor < 0) {
      *error = "Radio channel is closed";
      return false;
    }
    return WriteFrame(descriptor, frame, error);
  }

  void SetStopped(const std::string &error) {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      if (!error.empty())
        last_error_ = error;
      ready_ = false;
      running_ = false;
      stopping_ = true;
    }
    CloseFileDescriptor();
    FailPending(Status::kRadioUnavailable);
    status_cv_.notify_all();
    queue_cv_.notify_all();
  }

  void CloseFileDescriptor() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    const int descriptor = file_descriptor_.exchange(-1);
    if (descriptor >= 0) {
      shutdown(descriptor, SHUT_RDWR);
      close(descriptor);
    }
  }

  void FailPending(Status status) {
    std::map<std::uint64_t, std::shared_ptr<Pending>> pending;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      pending.swap(pending_);
    }
    for (const auto &entry : pending) {
      const std::shared_ptr<Pending> &request = entry.second;
      {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->result.status = status;
        request->done = true;
      }
      request->cv.notify_all();
    }
  }

  std::string device_path_;
  std::atomic<int> file_descriptor_{-1};
  StateStore *state_store_ = nullptr;

  mutable std::mutex status_mutex_;
  mutable std::condition_variable status_cv_;
  bool running_ = false;
  bool ready_ = false;
  bool stopping_ = true;
  std::string last_error_;
  std::uint64_t session_id_ = 0;
  std::uint64_t boot_generation_ = 0;
  std::uint64_t provider_generation_ = 0;
  std::uint64_t sequence_ = 0;
  std::uint64_t guest_token_ = 0;

  std::mutex write_mutex_;
  std::thread reader_thread_;
  std::thread dispatcher_thread_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<QueueItem> queue_;

  std::mutex pending_mutex_;
  std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
  std::atomic<std::uint64_t> next_request_id_{1};
  std::atomic<std::uint64_t> next_snapshot_request_id_{1ULL << 63};
  std::mutex sms_listener_mutex_;
  std::map<std::uint64_t, IncomingSmsListener> sms_listeners_;
  std::uint64_t next_sms_listener_id_ = 1;
};

Channel::Channel(std::string device_path)
    : impl_(std::make_unique<Impl>(std::move(device_path))) {}

Channel::~Channel() = default;

bool Channel::Start(StateStore *state_store, std::string *error) {
  std::string local_error;
  return impl_->Start(state_store, error == nullptr ? &local_error : error);
}

bool Channel::Start(int owned_file_descriptor, StateStore *state_store,
                    std::string *error) {
  std::string local_error;
  return impl_->Start(owned_file_descriptor, state_store,
                      error == nullptr ? &local_error : error);
}

void Channel::Stop() { impl_->Stop(); }

bool Channel::WaitUntilReady(std::chrono::milliseconds timeout) const {
  return impl_->WaitUntilReady(timeout);
}

bool Channel::WaitUntilStopped(std::chrono::milliseconds timeout) const {
  return impl_->WaitUntilStopped(timeout);
}

bool Channel::IsReady() const { return impl_->IsReady(); }

bool Channel::IsRunning() const { return impl_->IsRunning(); }

std::string Channel::LastError() const { return impl_->LastError(); }

std::uint64_t Channel::SessionId() const { return impl_->SessionId(); }

std::uint64_t Channel::BootGeneration() const {
  return impl_->BootGeneration();
}

std::uint64_t Channel::ProviderGeneration() const {
  return impl_->ProviderGeneration();
}

std::uint64_t Channel::AddIncomingSmsListener(IncomingSmsListener listener) {
  return impl_->AddIncomingSmsListener(std::move(listener));
}

void Channel::RemoveIncomingSmsListener(std::uint64_t id) {
  impl_->RemoveIncomingSmsListener(id);
}

CommandResult Channel::Request(const Command &command,
                               std::chrono::milliseconds timeout) {
  return impl_->Request(command, timeout);
}

} // namespace cdh::radio
