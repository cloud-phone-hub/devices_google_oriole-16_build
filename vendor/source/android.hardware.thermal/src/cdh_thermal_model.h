// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#pragma once

#include <array>

namespace cdh::thermal {

enum class Level {
  None = 0,
  Light = 1,
  Moderate = 2,
  Severe = 3,
  Critical = 4,
  Emergency = 5,
  Shutdown = 6,
};

struct ModelConfig {
  float baselineCelsius;
  float maximumCelsius;
  float heatingTimeSeconds;
  float coolingTimeSeconds;
  std::array<float, 6> hotThresholds;
};

struct Sample {
  float celsius;
  Level level;
};

class Model {
public:
  explicit Model(ModelConfig config);

  Sample Current() const;
  Sample Update(float load, float elapsedSeconds);

private:
  Level LevelFor(float celsius) const;

  ModelConfig config_;
  Sample sample_;
};

} // namespace cdh::thermal
