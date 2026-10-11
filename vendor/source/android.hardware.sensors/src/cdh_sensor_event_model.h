// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cdh::sensors {

enum class EventPayloadKind {
  Vec3,
  Vec4,
  Uncal,
  Scalar,
  StepCount,
  Data,
};

struct SensorEventSpec {
  int32_t handle;
  int32_t type;
  int32_t reportingMode;
  int32_t minDelayUs;
};

struct VirtualSensorEvent {
  int64_t timestampNs = 0;
  int32_t sensorHandle = 0;
  int32_t sensorType = 0;
  EventPayloadKind payloadKind = EventPayloadKind::Scalar;
  std::array<float, 16> values{};
  int64_t stepCount = 0;
};

class SensorEventModel {
public:
  explicit SensorEventModel(std::vector<SensorEventSpec> specs);

  bool setActive(int32_t sensorHandle, bool enabled, int64_t nowNs);
  bool setSamplingPeriod(int32_t sensorHandle, int64_t samplingPeriodNs);
  int64_t samplingPeriodNs(int32_t sensorHandle) const;
  void disableAll();
  std::vector<VirtualSensorEvent> collectDue(int64_t nowNs);

private:
  struct State {
    SensorEventSpec spec;
    bool active = false;
    bool initialSamplePublished = false;
    int64_t samplingPeriodNs = 0;
    int64_t nextSampleNs = 0;
  };

  static int64_t defaultSamplingPeriodNs(const SensorEventSpec &spec);
  static VirtualSensorEvent buildEvent(const SensorEventSpec &spec,
                                       int64_t nowNs);

  std::vector<int32_t> handles_;
  std::unordered_map<int32_t, State> states_;
};

} // namespace cdh::sensors
