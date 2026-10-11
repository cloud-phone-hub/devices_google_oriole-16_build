// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#include "../src/cdh_thermal_model.h"

#include <cmath>

int main() {
  const cdh::thermal::ModelConfig config{
      37.0f, 50.0f, 20.0f, 45.0f, {39.0f, 43.0f, 45.0f, 46.5f, 52.0f, 55.0f}};
  cdh::thermal::Model model(config);
  const auto initial = model.Current();
  if (std::fabs(initial.celsius - 37.0f) > 0.001f ||
      initial.level != cdh::thermal::Level::None) {
    return 1;
  }

  auto hot = initial;
  for (int second = 0; second < 80; ++second) {
    const auto previous = hot.celsius;
    hot = model.Update(1.0f, 1.0f);
    if (hot.celsius <= previous) {
      return 2;
    }
  }
  if (hot.celsius < 46.5f || hot.level < cdh::thermal::Level::Critical) {
    return 3;
  }

  auto cool = hot;
  for (int second = 0; second < 240; ++second) {
    cool = model.Update(0.0f, 1.0f);
  }
  if (cool.celsius >= hot.celsius || cool.celsius > 39.0f ||
      cool.level != cdh::thermal::Level::None) {
    return 4;
  }

  const auto clamped = model.Update(2.0f, 1.0f);
  if (!std::isfinite(clamped.celsius) || clamped.celsius <= cool.celsius ||
      clamped.celsius > config.maximumCelsius) {
    return 5;
  }
  return 0;
}
