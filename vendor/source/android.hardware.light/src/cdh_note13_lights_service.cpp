#include <android/binder_ibinder.h>
#include <android/binder_status.h>

extern "C" {
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);
binder_status_t AServiceManager_addService(AIBinder *binder,
                                           const char *instance);
}

#include <aidl/android/hardware/light/BnLights.h>
#include <aidl/android/hardware/light/BrightnessMode.h>
#include <aidl/android/hardware/light/FlashMode.h>
#include <aidl/android/hardware/light/HwLight.h>
#include <aidl/android/hardware/light/HwLightState.h>
#include <aidl/android/hardware/light/LightType.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using aidl::android::hardware::light::BnLights;
using aidl::android::hardware::light::BrightnessMode;
using aidl::android::hardware::light::FlashMode;
using aidl::android::hardware::light::HwLight;
using aidl::android::hardware::light::HwLightState;
using aidl::android::hardware::light::LightType;
using ndk::ScopedAStatus;

static constexpr const char *kLightsServiceName =
    "android.hardware.light.ILights/default";

static ScopedAStatus badValue() {
  return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

static HwLight makeLight(int32_t id, int32_t ordinal, LightType type) {
  HwLight light;
  light.id = id;
  light.ordinal = ordinal;
  light.type = type;
  return light;
}

static HwLightState defaultStateFor(LightType type) {
  HwLightState state;
  state.color = type == LightType::BATTERY ? 0xff00ff00 : 0x00000000;
  state.flashMode = FlashMode::NONE;
  state.flashOnMs = 0;
  state.flashOffMs = 0;
  state.brightnessMode = BrightnessMode::USER;
  return state;
}

class TargetLights : public BnLights {
public:
  TargetLights()
      : lights_({
#include "light_targets.inc"
        }) {
    for (const auto &light : lights_) {
      states_[light.id] = defaultStateFor(light.type);
    }
  }

  ScopedAStatus setLightState(int32_t id,
                              const HwLightState &state) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (states_.find(id) == states_.end()) {
      return badValue();
    }
    states_[id] = state;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getLights(std::vector<HwLight> *out) override {
    *out = lights_;
    return ScopedAStatus::ok();
  }

private:
  const std::vector<HwLight> lights_;
  std::mutex mutex_;
  std::unordered_map<int32_t, HwLightState> states_;
};

static binder_status_t
registerAsService(const std::shared_ptr<ndk::ICInterface> &service,
                  const char *name) {
  return AServiceManager_addService(service->asBinder().get(), name);
}

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(2);
  auto lights = ndk::SharedRefBase::make<TargetLights>();

  if (registerAsService(lights, kLightsServiceName) != STATUS_OK) {
    return 2;
  }

  ABinderProcess_joinThreadPool();
  return 0;
}
