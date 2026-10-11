// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#include "cdh_sensor_event_model.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace cdh::sensors {
namespace {

constexpr int64_t kNanosecondsPerMicrosecond = 1000;
constexpr int64_t kDefaultSamplingPeriodNs = 20'000'000;
constexpr int32_t kDevicePrivateBase = 0x10000;

float motionWave(int64_t timestampNs, float scale) {
  const double seconds = static_cast<double>(timestampNs % 10'000'000'000LL) /
                         1'000'000'000.0;
  return static_cast<float>(std::sin(seconds * 1.7) * scale);
}

EventPayloadKind payloadKindForType(int32_t type) {
  switch (type) {
  case 1:
  case 2:
  case 3:
  case 4:
  case 9:
  case 10:
    return EventPayloadKind::Vec3;
  case 15:
    return EventPayloadKind::Vec4;
  case 14:
  case 16:
  case 35:
    return EventPayloadKind::Uncal;
  case 11:
  case 20:
    return EventPayloadKind::Data;
  case 19:
    return EventPayloadKind::StepCount;
  default:
    return type >= kDevicePrivateBase ? EventPayloadKind::Data
                                      : EventPayloadKind::Scalar;
  }
}

void fillVec3(VirtualSensorEvent &event) {
  const float wave = motionWave(event.timestampNs, 1.0f);
  switch (event.sensorType) {
  case 1:
  case 35:
    event.values[0] = wave * 0.035f;
    event.values[1] = -0.025f + wave * 0.015f;
    event.values[2] = 9.80665f + wave * 0.02f;
    break;
  case 2:
  case 14:
    event.values[0] = 24.8f + wave * 0.3f;
    event.values[1] = -4.7f + wave * 0.2f;
    event.values[2] = -39.5f + wave * 0.25f;
    break;
  case 3:
    event.values[0] = 181.0f + wave * 0.2f;
    event.values[1] = 0.2f + wave * 0.05f;
    event.values[2] = -0.3f + wave * 0.05f;
    break;
  case 4:
  case 16:
    event.values[0] = wave * 0.002f;
    event.values[1] = -wave * 0.0015f;
    event.values[2] = wave * 0.001f;
    break;
  case 9:
    event.values[0] = wave * 0.01f;
    event.values[1] = -wave * 0.008f;
    event.values[2] = 9.80665f;
    break;
  case 10:
    event.values[0] = wave * 0.025f;
    event.values[1] = -wave * 0.02f;
    event.values[2] = wave * 0.015f;
    break;
  default:
    break;
  }
  event.values[3] = 3.0f;
}

void fillScalar(VirtualSensorEvent &event) {
  const float wave = motionWave(event.timestampNs, 1.0f);
  switch (event.sensorType) {
  case 5:
    event.values[0] = 120.0f + wave * 2.0f;
    break;
  case 6:
    event.values[0] = 1008.25f + wave * 0.08f;
    break;
  case 8:
    event.values[0] = 5.0f;
    break;
  case 27:
    event.values[0] = 0.0f;
    break;
  default:
    event.values[0] = 0.0f;
    break;
  }
}

void fillData(VirtualSensorEvent &event) {
  const float wave = motionWave(event.timestampNs, 1.0f);
  if (event.sensorType == 11 || event.sensorType == 20) {
    event.values[0] = wave * 0.001f;
    event.values[1] = -wave * 0.001f;
    event.values[2] = 0.0f;
    event.values[3] = 1.0f;
    event.values[4] = 0.01f;
    return;
  }
  switch (event.sensorHandle) {
  case 23:
    event.values[0] = 31.25f + wave * 0.08f;
    break;
  case 24:
    event.values[0] = 34.5f + wave * 0.08f;
    break;
  case 29:
  case 31:
  case 35:
    event.values[0] = 120.0f + wave * 2.0f;
    break;
  default:
    event.values[0] = 0.0f;
    break;
  }
}

} // namespace

SensorEventModel::SensorEventModel(std::vector<SensorEventSpec> specs) {
  handles_.reserve(specs.size());
  states_.reserve(specs.size());
  for (const auto &spec : specs) {
    State state;
    state.spec = spec;
    state.samplingPeriodNs = defaultSamplingPeriodNs(spec);
    if (states_.emplace(spec.handle, state).second) {
      handles_.push_back(spec.handle);
    }
  }
}

bool SensorEventModel::setActive(int32_t sensorHandle, bool enabled,
                                 int64_t nowNs) {
  auto found = states_.find(sensorHandle);
  if (found == states_.end()) {
    return false;
  }
  State &state = found->second;
  if (state.active == enabled) {
    return true;
  }
  state.active = enabled;
  state.initialSamplePublished = false;
  state.nextSampleNs = enabled ? nowNs : 0;
  return true;
}

bool SensorEventModel::setSamplingPeriod(int32_t sensorHandle,
                                         int64_t samplingPeriodNs) {
  auto found = states_.find(sensorHandle);
  if (found == states_.end() || samplingPeriodNs < 0) {
    return false;
  }
  const int64_t minimum = defaultSamplingPeriodNs(found->second.spec);
  found->second.samplingPeriodNs = std::max(samplingPeriodNs, minimum);
  return true;
}

int64_t SensorEventModel::samplingPeriodNs(int32_t sensorHandle) const {
  const auto found = states_.find(sensorHandle);
  return found == states_.end() ? -1 : found->second.samplingPeriodNs;
}

void SensorEventModel::disableAll() {
  for (auto &[handle, state] : states_) {
    static_cast<void>(handle);
    state.active = false;
    state.initialSamplePublished = false;
    state.nextSampleNs = 0;
  }
}

std::vector<VirtualSensorEvent>
SensorEventModel::collectDue(int64_t nowNs) {
  std::vector<VirtualSensorEvent> events;
  events.reserve(handles_.size());
  for (const int32_t handle : handles_) {
    State &state = states_.at(handle);
    if (!state.active || state.spec.reportingMode == 2 ||
        state.spec.reportingMode == 3 || nowNs < state.nextSampleNs) {
      continue;
    }
    if (state.spec.reportingMode == 1 && state.initialSamplePublished) {
      continue;
    }
    events.push_back(buildEvent(state.spec, nowNs));
    state.initialSamplePublished = true;
    if (state.spec.reportingMode == 0) {
      state.nextSampleNs = nowNs + state.samplingPeriodNs;
    } else {
      state.nextSampleNs = std::numeric_limits<int64_t>::max();
    }
  }
  return events;
}

int64_t
SensorEventModel::defaultSamplingPeriodNs(const SensorEventSpec &spec) {
  if (spec.minDelayUs <= 0) {
    return kDefaultSamplingPeriodNs;
  }
  return static_cast<int64_t>(spec.minDelayUs) * kNanosecondsPerMicrosecond;
}

VirtualSensorEvent
SensorEventModel::buildEvent(const SensorEventSpec &spec, int64_t nowNs) {
  VirtualSensorEvent event;
  event.timestampNs = nowNs;
  event.sensorHandle = spec.handle;
  event.sensorType = spec.type;
  event.payloadKind = payloadKindForType(spec.type);
  switch (event.payloadKind) {
  case EventPayloadKind::Vec3:
    fillVec3(event);
    break;
  case EventPayloadKind::Vec4:
    event.values[0] = motionWave(nowNs, 0.001f);
    event.values[1] = -event.values[0];
    event.values[2] = 0.0f;
    event.values[3] = 1.0f;
    break;
  case EventPayloadKind::Uncal:
    fillVec3(event);
    event.values[3] = 0.001f;
    event.values[4] = -0.001f;
    event.values[5] = 0.0005f;
    break;
  case EventPayloadKind::Scalar:
    fillScalar(event);
    break;
  case EventPayloadKind::StepCount:
    event.stepCount = 0;
    break;
  case EventPayloadKind::Data:
    fillData(event);
    break;
  }
  return event;
}

} // namespace cdh::sensors
