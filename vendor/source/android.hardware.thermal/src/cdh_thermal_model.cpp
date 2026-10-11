// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#include "cdh_thermal_model.h"

#include <algorithm>
#include <cmath>

namespace cdh::thermal {

Model::Model(ModelConfig config)
    : config_(config), sample_{config.baselineCelsius, Level::None} {
  sample_.level = LevelFor(sample_.celsius);
}

Sample Model::Current() const { return sample_; }

Sample Model::Update(float load, float elapsedSeconds) {
  if (!std::isfinite(load)) {
    load = 0.0f;
  }
  load = std::clamp(load, 0.0f, 1.0f);
  if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0f) {
    return sample_;
  }
  elapsedSeconds = std::min(elapsedSeconds, 300.0f);
  const float target =
      config_.baselineCelsius +
      (config_.maximumCelsius - config_.baselineCelsius) * load;
  const float timeConstant = target >= sample_.celsius
                                 ? config_.heatingTimeSeconds
                                 : config_.coolingTimeSeconds;
  if (!std::isfinite(timeConstant) || timeConstant <= 0.0f) {
    return sample_;
  }
  const float response = 1.0f - std::exp(-elapsedSeconds / timeConstant);
  sample_.celsius += (target - sample_.celsius) * response;
  sample_.celsius = std::clamp(sample_.celsius, config_.baselineCelsius,
                               config_.maximumCelsius);
  sample_.level = LevelFor(sample_.celsius);
  return sample_;
}

Level Model::LevelFor(float celsius) const {
  for (int index = static_cast<int>(config_.hotThresholds.size()) - 1;
       index >= 0; --index) {
    if (celsius >= config_.hotThresholds[static_cast<size_t>(index)]) {
      return static_cast<Level>(index + 1);
    }
  }
  return Level::None;
}

} // namespace cdh::thermal
