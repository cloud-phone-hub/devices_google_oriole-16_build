#include <android/binder_ibinder.h>
#include <android/binder_status.h>

extern "C" {
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);
binder_status_t AServiceManager_addService(AIBinder *binder,
                                           const char *instance);
}

#include <aidl/android/hardware/sensors/BnSensors.h>
#include <aidl/android/hardware/sensors/Event.h>
#include <aidl/android/hardware/sensors/ISensors.h>
#include <aidl/android/hardware/sensors/ISensorsCallback.h>
#include <aidl/android/hardware/sensors/SensorInfo.h>
#include <fmq/AidlMessageQueue.h>

#include "cdh_sensor_event_model.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using aidl::android::hardware::common::fmq::MQDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using aidl::android::hardware::sensors::BnSensors;
using aidl::android::hardware::sensors::Event;
using aidl::android::hardware::sensors::ISensors;
using aidl::android::hardware::sensors::ISensorsCallback;
using aidl::android::hardware::sensors::SensorInfo;
using aidl::android::hardware::sensors::SensorStatus;
using aidl::android::hardware::sensors::SensorType;
using android::AidlMessageQueue;
using cdh::sensors::EventPayloadKind;
using cdh::sensors::SensorEventModel;
using cdh::sensors::SensorEventSpec;
using cdh::sensors::VirtualSensorEvent;
using ndk::ScopedAStatus;

static constexpr const char *kSensorsServiceName =
    "android.hardware.sensors.ISensors/default";

struct SensorTarget {
  int32_t handle;
  const char *name;
  const char *vendor;
  int32_t version;
  int32_t type;
  const char *typeString;
  float maximumRange;
  float resolution;
  float power;
  int32_t minDelayUs;
  int32_t fifoReservedEventCount;
  int32_t fifoMaxEventCount;
  int32_t maxDelayUs;
  bool wakeUp;
  int32_t reportingMode;
};

#ifndef CDH_SENSOR_TARGETS_INCLUDE
#define CDH_SENSOR_TARGETS_INCLUDE "note13_sensor_targets.inc"
#endif

#include CDH_SENSOR_TARGETS_INCLUDE

struct SensorState {
  bool active = false;
  int64_t samplingPeriodNs = 0;
  int64_t maxReportLatencyNs = 0;
};

static SensorInfo toSensorInfo(const SensorTarget &target) {
  SensorInfo info;
  info.sensorHandle = target.handle;
  info.name = target.name;
  info.vendor = target.vendor;
  info.version = target.version;
  info.type = SensorType(target.type);
  info.typeAsString = target.typeString;
  info.maxRange = target.maximumRange;
  info.resolution = target.resolution;
  info.power = target.power;
  info.minDelayUs = target.minDelayUs;
  info.fifoReservedEventCount = target.fifoReservedEventCount;
  info.fifoMaxEventCount = target.fifoMaxEventCount;
  info.requiredPermission = "";
  info.maxDelayUs = target.maxDelayUs;
  info.flags =
      (target.reportingMode << SensorInfo::SENSOR_FLAG_SHIFT_REPORTING_MODE) |
      (target.wakeUp ? SensorInfo::SENSOR_FLAG_BITS_WAKE_UP : 0);
  return info;
}

static std::vector<SensorInfo> buildNote13SensorList() {
  std::vector<SensorInfo> sensors;
  sensors.reserve(sizeof(kNote13SensorTargets) /
                  sizeof(kNote13SensorTargets[0]));
  for (const auto &target : kNote13SensorTargets) {
    sensors.push_back(toSensorInfo(target));
  }
  return sensors;
}

static std::vector<SensorEventSpec> buildNote13SensorEventSpecs() {
  std::vector<SensorEventSpec> specs;
  specs.reserve(sizeof(kNote13SensorTargets) /
                sizeof(kNote13SensorTargets[0]));
  for (const auto &target : kNote13SensorTargets) {
    specs.push_back(SensorEventSpec{target.handle, target.type,
                                    target.reportingMode, target.minDelayUs});
  }
  return specs;
}

static int64_t bootTimeNanos() {
  timespec now{};
  if (clock_gettime(CLOCK_BOOTTIME, &now) != 0) {
    return 0;
  }
  return static_cast<int64_t>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
}

static Event toAidlEvent(const VirtualSensorEvent &sample) {
  using Payload = Event::EventPayload;
  Event event;
  event.timestamp = sample.timestampNs;
  event.sensorHandle = sample.sensorHandle;
  event.sensorType = SensorType(sample.sensorType);
  switch (sample.payloadKind) {
  case EventPayloadKind::Vec3: {
    Payload::Vec3 value;
    value.x = sample.values[0];
    value.y = sample.values[1];
    value.z = sample.values[2];
    value.status = SensorStatus::ACCURACY_HIGH;
    event.payload.set<Payload::Tag::vec3>(value);
    break;
  }
  case EventPayloadKind::Vec4: {
    Payload::Vec4 value;
    value.x = sample.values[0];
    value.y = sample.values[1];
    value.z = sample.values[2];
    value.w = sample.values[3];
    event.payload.set<Payload::Tag::vec4>(value);
    break;
  }
  case EventPayloadKind::Uncal: {
    Payload::Uncal value;
    value.x = sample.values[0];
    value.y = sample.values[1];
    value.z = sample.values[2];
    value.xBias = sample.values[3];
    value.yBias = sample.values[4];
    value.zBias = sample.values[5];
    event.payload.set<Payload::Tag::uncal>(value);
    break;
  }
  case EventPayloadKind::Scalar:
    event.payload.set<Payload::Tag::scalar>(sample.values[0]);
    break;
  case EventPayloadKind::StepCount:
    event.payload.set<Payload::Tag::stepCount>(sample.stepCount);
    break;
  case EventPayloadKind::Data: {
    Payload::Data value;
    value.values = sample.values;
    event.payload.set<Payload::Tag::data>(value);
    break;
  }
  }
  return event;
}

template <typename T, typename Flavor>
static bool
isUsableQueueDescriptor(const MQDescriptor<T, Flavor> &descriptor) {
  if (descriptor.grantors.empty() || descriptor.handle.fds.empty() ||
      descriptor.quantum <= 0) {
    return false;
  }
  for (const auto &grantor : descriptor.grantors) {
    if (grantor.fdIndex < 0 ||
        static_cast<size_t>(grantor.fdIndex) >= descriptor.handle.fds.size() ||
        grantor.offset < 0 || grantor.extent <= 0) {
      return false;
    }
  }
  return true;
}

class Note13Sensors : public BnSensors {
public:
  Note13Sensors() : eventModel_(buildNote13SensorEventSpecs()) {
    for (const auto &target : kNote13SensorTargets) {
      sensorStates_.emplace(target.handle, SensorState{});
    }
    eventThread_ = std::thread([this] { runEventLoop(); });
  }

  ~Note13Sensors() override {
    {
      std::lock_guard<std::mutex> guard(lock_);
      stopEventThread_ = true;
      eventCondition_.notify_all();
    }
    if (eventThread_.joinable()) {
      eventThread_.join();
    }
  }

  ScopedAStatus activate(int32_t sensorHandle, bool enabled) override {
    std::lock_guard<std::mutex> guard(lock_);
    if (!isKnownSensorLocked(sensorHandle)) {
      return badValue();
    }
    sensorStates_[sensorHandle].active = enabled;
    if (!eventModel_.setActive(sensorHandle, enabled, bootTimeNanos())) {
      return badValue();
    }
    eventCondition_.notify_all();
    return ScopedAStatus::ok();
  }

  ScopedAStatus batch(int32_t sensorHandle, int64_t samplingPeriodNs,
                      int64_t maxReportLatencyNs) override {
    std::lock_guard<std::mutex> guard(lock_);
    if (!isKnownSensorLocked(sensorHandle) || samplingPeriodNs < 0 ||
        maxReportLatencyNs < 0) {
      return badValue();
    }
    SensorState &state = sensorStates_[sensorHandle];
    state.samplingPeriodNs = samplingPeriodNs;
    state.maxReportLatencyNs = maxReportLatencyNs;
    if (!eventModel_.setSamplingPeriod(sensorHandle, samplingPeriodNs)) {
      return badValue();
    }
    eventCondition_.notify_all();
    return ScopedAStatus::ok();
  }

  ScopedAStatus configDirectReport(int32_t, int32_t, ISensors::RateLevel,
                                   int32_t *out) override {
    *out = EX_UNSUPPORTED_OPERATION;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
  }

  ScopedAStatus flush(int32_t sensorHandle) override {
    std::lock_guard<std::mutex> guard(lock_);
    if (!isKnownSensorLocked(sensorHandle) ||
        !sensorStates_[sensorHandle].active) {
      return badValue();
    }
    Event event;
    event.timestamp = bootTimeNanos();
    event.sensorHandle = sensorHandle;
    event.sensorType = SensorType::META_DATA;
    Event::EventPayload::MetaData meta;
    meta.what = Event::EventPayload::MetaData::MetaDataEventType::
        META_DATA_FLUSH_COMPLETE;
    event.payload.set<Event::EventPayload::Tag::meta>(meta);
    return writeEventsLocked(std::vector<Event>{event}) ? ScopedAStatus::ok()
                                                        : noMemory();
  }

  ScopedAStatus getSensorsList(std::vector<SensorInfo> *out) override {
    *out = buildNote13SensorList();
    return ScopedAStatus::ok();
  }

  ScopedAStatus initialize(
      const MQDescriptor<Event, SynchronizedReadWrite> &eventQueueDescriptor,
      const MQDescriptor<int32_t, SynchronizedReadWrite> &wakeLockDescriptor,
      const std::shared_ptr<ISensorsCallback> &sensorsCallback) override {
    if (sensorsCallback == nullptr ||
        !isUsableQueueDescriptor(eventQueueDescriptor) ||
        !isUsableQueueDescriptor(wakeLockDescriptor)) {
      return badValue();
    }
    auto eventQueue =
        std::make_unique<EventQueue>(eventQueueDescriptor, true);
    auto wakeLockQueue =
        std::make_unique<WakeLockQueue>(wakeLockDescriptor, true);
    if (!eventQueue->isValid() || eventQueue->getEventFlagWord() == nullptr ||
        !wakeLockQueue->isValid() ||
        wakeLockQueue->getEventFlagWord() == nullptr) {
      return badValue();
    }
    std::lock_guard<std::mutex> guard(lock_);
    eventQueue_ = std::move(eventQueue);
    wakeLockQueue_ = std::move(wakeLockQueue);
    callback_ = sensorsCallback;
    eventModel_.disableAll();
    for (auto &[handle, state] : sensorStates_) {
      static_cast<void>(handle);
      state.active = false;
    }
    initialized_ = true;
    eventQueueQuantum_ = eventQueueDescriptor.quantum;
    wakeLockQueueQuantum_ = wakeLockDescriptor.quantum;
    eventCondition_.notify_all();
    return ScopedAStatus::ok();
  }

  ScopedAStatus injectSensorData(const Event &event) override {
    std::lock_guard<std::mutex> guard(lock_);
    if (operationMode_ != ISensors::OperationMode::DATA_INJECTION ||
        !isKnownSensorLocked(event.sensorHandle)) {
      return badValue();
    }
    return writeEventsLocked(std::vector<Event>{event}) ? ScopedAStatus::ok()
                                                        : noMemory();
  }

  ScopedAStatus registerDirectChannel(const ISensors::SharedMemInfo &,
                                      int32_t *out) override {
    *out = EX_UNSUPPORTED_OPERATION;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
  }

  ScopedAStatus setOperationMode(ISensors::OperationMode mode) override {
    std::lock_guard<std::mutex> guard(lock_);
    if (mode != ISensors::OperationMode::NORMAL &&
        mode != ISensors::OperationMode::DATA_INJECTION) {
      return badValue();
    }
    operationMode_ = mode;
    eventCondition_.notify_all();
    return ScopedAStatus::ok();
  }

  ScopedAStatus unregisterDirectChannel(int32_t) override {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
  }

private:
  static ScopedAStatus badValue() {
    return ScopedAStatus::fromServiceSpecificError(ISensors::ERROR_BAD_VALUE);
  }

  static ScopedAStatus noMemory() {
    return ScopedAStatus::fromServiceSpecificError(ISensors::ERROR_NO_MEMORY);
  }

  bool isKnownSensorLocked(int32_t sensorHandle) const {
    return sensorStates_.find(sensorHandle) != sensorStates_.end();
  }

  bool writeEventsLocked(const std::vector<Event> &events) {
    if (!initialized_ || eventQueue_ == nullptr || events.empty()) {
      return false;
    }
    constexpr int64_t kWriteTimeoutNs = 20'000'000;
    return eventQueue_->writeBlocking(
        events.data(), events.size(),
        static_cast<uint32_t>(ISensors::EVENT_QUEUE_FLAG_BITS_EVENTS_READ),
        static_cast<uint32_t>(
            ISensors::EVENT_QUEUE_FLAG_BITS_READ_AND_PROCESS),
        kWriteTimeoutNs);
  }

  void drainWakeLockQueueLocked() {
    if (wakeLockQueue_ == nullptr) {
      return;
    }
    int32_t handled = 0;
    while (wakeLockQueue_->availableToRead() > 0) {
      if (!wakeLockQueue_->read(&handled)) {
        return;
      }
    }
  }

  void runEventLoop() {
    std::unique_lock<std::mutex> lock(lock_);
    constexpr auto kIdleWait = std::chrono::milliseconds(5);
    while (!stopEventThread_) {
      eventCondition_.wait_for(lock, kIdleWait);
      if (stopEventThread_) {
        break;
      }
      drainWakeLockQueueLocked();
      if (!initialized_ || operationMode_ != ISensors::OperationMode::NORMAL) {
        continue;
      }
      const auto samples = eventModel_.collectDue(bootTimeNanos());
      if (samples.empty()) {
        continue;
      }
      std::vector<Event> events;
      events.reserve(samples.size());
      for (const auto &sample : samples) {
        events.push_back(toAidlEvent(sample));
      }
      static_cast<void>(writeEventsLocked(events));
    }
  }

  using EventQueue = AidlMessageQueue<Event, SynchronizedReadWrite>;
  using WakeLockQueue = AidlMessageQueue<int32_t, SynchronizedReadWrite>;

  std::mutex lock_;
  std::condition_variable eventCondition_;
  std::unordered_map<int32_t, SensorState> sensorStates_;
  std::shared_ptr<ISensorsCallback> callback_;
  SensorEventModel eventModel_;
  std::unique_ptr<EventQueue> eventQueue_;
  std::unique_ptr<WakeLockQueue> wakeLockQueue_;
  std::thread eventThread_;
  ISensors::OperationMode operationMode_ = ISensors::OperationMode::NORMAL;
  bool stopEventThread_ = false;
  bool initialized_ = false;
  int32_t eventQueueQuantum_ = 0;
  int32_t wakeLockQueueQuantum_ = 0;
};

static binder_status_t
registerAsService(const std::shared_ptr<Note13Sensors> &service) {
  const std::string instance = kSensorsServiceName;
  return AServiceManager_addService(service->asBinder().get(),
                                    instance.c_str());
}

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(1);
  auto service = ndk::SharedRefBase::make<Note13Sensors>();
  binder_status_t status = registerAsService(service);
  if (status != STATUS_OK) {
    return 2;
  }
  ABinderProcess_joinThreadPool();
  return 0;
}
