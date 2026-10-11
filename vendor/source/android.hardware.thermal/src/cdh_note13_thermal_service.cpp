// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#include <android/binder_ibinder.h>
#include <android/binder_status.h>

#include <cstdint>

extern "C" {
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);
binder_status_t AServiceManager_addService(AIBinder *binder,
                                           const char *instance);
}

#include <aidl/android/hardware/thermal/BnThermal.h>
#include <aidl/android/hardware/thermal/CoolingDevice.h>
#include <aidl/android/hardware/thermal/CoolingType.h>
#include <aidl/android/hardware/thermal/ICoolingDeviceChangedCallback.h>
#include <aidl/android/hardware/thermal/IThermal.h>
#include <aidl/android/hardware/thermal/IThermalChangedCallback.h>
#include <aidl/android/hardware/thermal/Temperature.h>
#include <aidl/android/hardware/thermal/TemperatureThreshold.h>
#include <aidl/android/hardware/thermal/TemperatureType.h>
#include <aidl/android/hardware/thermal/ThrottlingSeverity.h>

#include "cdh_thermal_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using aidl::android::hardware::thermal::BnThermal;
using aidl::android::hardware::thermal::CoolingDevice;
using aidl::android::hardware::thermal::CoolingType;
using aidl::android::hardware::thermal::ICoolingDeviceChangedCallback;
using aidl::android::hardware::thermal::IThermalChangedCallback;
using aidl::android::hardware::thermal::Temperature;
using aidl::android::hardware::thermal::TemperatureThreshold;
using aidl::android::hardware::thermal::TemperatureType;
using aidl::android::hardware::thermal::ThrottlingSeverity;
using ndk::ScopedAStatus;

static constexpr const char *kThermalServiceName =
    "android.hardware.thermal.IThermal/default";

static float nanValue() { return std::numeric_limits<float>::quiet_NaN(); }

static bool isTemperatureType(TemperatureType value, TemperatureType want) {
  return value == want;
}

static ThrottlingSeverity throttlingStatus(cdh::thermal::Level level) {
  switch (level) {
  case cdh::thermal::Level::None:
    return ThrottlingSeverity::NONE;
  case cdh::thermal::Level::Light:
    return ThrottlingSeverity::LIGHT;
  case cdh::thermal::Level::Moderate:
    return ThrottlingSeverity::MODERATE;
  case cdh::thermal::Level::Severe:
    return ThrottlingSeverity::SEVERE;
  case cdh::thermal::Level::Critical:
    return ThrottlingSeverity::CRITICAL;
  case cdh::thermal::Level::Emergency:
    return ThrottlingSeverity::EMERGENCY;
  case cdh::thermal::Level::Shutdown:
    return ThrottlingSeverity::SHUTDOWN;
  }
  return ThrottlingSeverity::NONE;
}

static Temperature makeTemperature(const char *name, int32_t type,
                                   float value) {
  Temperature temperature;
  temperature.type = static_cast<TemperatureType>(type);
  temperature.name = name;
  temperature.value = value;
  temperature.throttlingStatus = ThrottlingSeverity::NONE;
  return temperature;
}

static TemperatureThreshold makeThreshold(const char *name, int32_t type,
                                          std::vector<float> hot,
                                          std::vector<float> cold) {
  TemperatureThreshold threshold;
  threshold.type = static_cast<TemperatureType>(type);
  threshold.name = name;
  threshold.hotThrottlingThresholds = std::move(hot);
  threshold.coldThrottlingThresholds = std::move(cold);
  return threshold;
}

[[maybe_unused]] static CoolingDevice
makeCoolingDevice(const char *name, int32_t type, int64_t value) {
  CoolingDevice device;
  device.type = static_cast<CoolingType>(type);
  device.name = name;
  device.value = value;
  return device;
}

#include "thermal_targets.inc"

class CPULoadReader {
public:
  bool Read(float *out) {
    std::FILE *input = std::fopen("/proc/stat", "r");
    if (input == nullptr) {
      return false;
    }
    char label[8]{};
    unsigned long long values[10]{};
    const int count = std::fscanf(
        input, "%7s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", label,
        &values[0], &values[1], &values[2], &values[3], &values[4], &values[5],
        &values[6], &values[7], &values[8], &values[9]);
    std::fclose(input);
    if (count < 5 || std::strcmp(label, "cpu") != 0) {
      return false;
    }
    const int valueCount = count - 1;
    uint64_t total = 0;
    for (int index = 0; index < valueCount; ++index) {
      total += values[index];
    }
    uint64_t idle = values[3];
    if (valueCount > 4) {
      idle += values[4];
    }
    if (!initialized_) {
      previousTotal_ = total;
      previousIdle_ = idle;
      initialized_ = true;
      return false;
    }
    if (total <= previousTotal_ || idle < previousIdle_) {
      previousTotal_ = total;
      previousIdle_ = idle;
      return false;
    }
    const uint64_t totalDelta = total - previousTotal_;
    const uint64_t idleDelta = std::min(idle - previousIdle_, totalDelta);
    previousTotal_ = total;
    previousIdle_ = idle;
    *out = static_cast<float>(totalDelta - idleDelta) /
           static_cast<float>(totalDelta);
    return std::isfinite(*out);
  }

private:
  bool initialized_ = false;
  uint64_t previousTotal_ = 0;
  uint64_t previousIdle_ = 0;
};

class TargetThermal final : public BnThermal {
public:
  TargetThermal()
      : model_(buildThermalModelConfig()),
        sampler_(&TargetThermal::RunModel, this) {}

  ~TargetThermal() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    wake_.notify_all();
    if (sampler_.joinable()) {
      sampler_.join();
    }
  }

  ScopedAStatus getCoolingDevices(std::vector<CoolingDevice> *out) override {
    *out = buildTargetCoolingDevices();
    return ScopedAStatus::ok();
  }

  ScopedAStatus
  getCoolingDevicesWithType(CoolingType type,
                            std::vector<CoolingDevice> *out) override {
    out->clear();
    for (const auto &device : buildTargetCoolingDevices()) {
      if (device.type == type) {
        out->push_back(device);
      }
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus getTemperatures(std::vector<Temperature> *out) override {
    *out = CurrentTemperatures();
    return ScopedAStatus::ok();
  }

  ScopedAStatus
  getTemperaturesWithType(TemperatureType type,
                          std::vector<Temperature> *out) override {
    out->clear();
    for (const auto &temperature : CurrentTemperatures()) {
      if (isTemperatureType(temperature.type, type)) {
        out->push_back(temperature);
      }
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus
  getTemperatureThresholds(std::vector<TemperatureThreshold> *out) override {
    *out = buildTargetThresholds();
    return ScopedAStatus::ok();
  }

  ScopedAStatus getTemperatureThresholdsWithType(
      TemperatureType type, std::vector<TemperatureThreshold> *out) override {
    out->clear();
    for (const auto &threshold : buildTargetThresholds()) {
      if (isTemperatureType(threshold.type, type)) {
        out->push_back(threshold);
      }
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus registerThermalChangedCallback(
      const std::shared_ptr<IThermalChangedCallback> &callback) override {
    return RegisterCallback(callback, false, static_cast<TemperatureType>(0));
  }

  ScopedAStatus registerThermalChangedCallbackWithType(
      const std::shared_ptr<IThermalChangedCallback> &callback,
      TemperatureType type) override {
    return RegisterCallback(callback, true, type);
  }

  ScopedAStatus unregisterThermalChangedCallback(
      const std::shared_ptr<IThermalChangedCallback> &callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.erase(std::remove_if(callbacks_.begin(), callbacks_.end(),
                                    [&callback](const Callback &entry) {
                                      return SameCallback(entry.callback,
                                                          callback);
                                    }),
                     callbacks_.end());
    return ScopedAStatus::ok();
  }

  ScopedAStatus registerCoolingDeviceChangedCallbackWithType(
      const std::shared_ptr<ICoolingDeviceChangedCallback> &,
      CoolingType) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus unregisterCoolingDeviceChangedCallback(
      const std::shared_ptr<ICoolingDeviceChangedCallback> &) override {
    return ScopedAStatus::ok();
  }

#if CDH_THERMAL_AIDL_VERSION >= 3
  ScopedAStatus forecastSkinTemperature(int32_t, float *out) override {
    for (const auto &temperature : CurrentTemperatures()) {
      if (temperature.type == TemperatureType::SKIN) {
        *out = temperature.value;
        return ScopedAStatus::ok();
      }
    }
    *out = nanValue();
    return ScopedAStatus::ok();
  }
#endif

private:
  struct Callback {
    std::shared_ptr<IThermalChangedCallback> callback;
    bool filtered;
    TemperatureType type;
  };

  static bool
  SameCallback(const std::shared_ptr<IThermalChangedCallback> &left,
               const std::shared_ptr<IThermalChangedCallback> &right) {
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    return left->asBinder().get() == right->asBinder().get();
  }

  static std::vector<Temperature>
  MakeTemperatures(const cdh::thermal::Sample &sample) {
    auto temperatures = buildTargetTemperatures();
    for (auto &temperature : temperatures) {
      if (temperature.type == TemperatureType::SKIN) {
        temperature.value = sample.celsius;
        temperature.throttlingStatus = throttlingStatus(sample.level);
      }
    }
    return temperatures;
  }

  std::vector<Temperature> CurrentTemperatures() {
    std::lock_guard<std::mutex> lock(mutex_);
    return MakeTemperatures(model_.Current());
  }

  ScopedAStatus
  RegisterCallback(const std::shared_ptr<IThermalChangedCallback> &callback,
                   bool filtered, TemperatureType type) {
    if (callback == nullptr) {
      return ScopedAStatus::ok();
    }
    std::vector<Temperature> temperatures;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callbacks_.push_back(Callback{callback, filtered, type});
      temperatures = MakeTemperatures(model_.Current());
    }
    Notify(Callback{callback, filtered, type}, temperatures);
    return ScopedAStatus::ok();
  }

  static void Notify(const Callback &entry,
                     const std::vector<Temperature> &temperatures) {
    for (const auto &temperature : temperatures) {
      if (!entry.filtered || temperature.type == entry.type) {
        entry.callback->notifyThrottling(temperature);
      }
    }
  }

  void RunModel() {
    CPULoadReader loadReader;
    const auto interval =
        std::chrono::milliseconds(thermalSampleIntervalMillis());
    auto previousTime = std::chrono::steady_clock::now();
    while (true) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, interval, [this] { return stopped_; })) {
          return;
        }
      }
      const auto now = std::chrono::steady_clock::now();
      float load = 0.0f;
      if (!loadReader.Read(&load)) {
        previousTime = now;
        continue;
      }
      const float elapsed =
          std::chrono::duration<float>(now - previousTime).count();
      previousTime = now;

      std::vector<Callback> callbacks;
      std::vector<Temperature> temperatures;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto previous = model_.Current();
        const auto current = model_.Update(load, elapsed);
        if (std::fabs(current.celsius - previous.celsius) < 0.001f &&
            current.level == previous.level) {
          continue;
        }
        callbacks = callbacks_;
        temperatures = MakeTemperatures(current);
      }
      for (const auto &callback : callbacks) {
        Notify(callback, temperatures);
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopped_ = false;
  cdh::thermal::Model model_;
  std::vector<Callback> callbacks_;
  std::thread sampler_;
};

static binder_status_t
registerAsService(const std::shared_ptr<ndk::ICInterface> &service,
                  const char *name) {
  return AServiceManager_addService(service->asBinder().get(), name);
}

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(2);
  auto thermal = ndk::SharedRefBase::make<TargetThermal>();
  if (registerAsService(thermal, kThermalServiceName) != STATUS_OK) {
    return 2;
  }
  ABinderProcess_joinThreadPool();
  return 0;
}
