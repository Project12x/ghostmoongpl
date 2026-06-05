// SPDX-License-Identifier: MIT
// CpuMeter.h — Atomic CPU load measurement for processBlock
// Extracted from Breadbin pattern.
//
// Header-only, zero external dependencies.

#pragma once

#include <atomic>
#include <chrono>

namespace gm {

/// Measures DSP CPU load as a percentage of the audio buffer time budget.
/// Usage:
///   gm::CpuMeter meter;
///   // In processBlock:
///   meter.startBlock();
///   // ... DSP processing ...
///   meter.endBlock(sampleRate, numSamples);
///   // From UI thread:
///   float load = meter.getLoadPercent();
class CpuMeter {
public:
  CpuMeter() = default;

  /// Call at the start of processBlock
  void startBlock() {
    blockStart_ = Clock::now();
  }

  /// Call at the end of processBlock
  void endBlock(double sampleRate, int numSamples) {
    auto elapsed = Clock::now() - blockStart_;
    double elapsedSec =
        std::chrono::duration<double>(elapsed).count();
    double budgetSec = static_cast<double>(numSamples) / sampleRate;

    if (budgetSec > 0.0) {
      loadPercent_.store(
          static_cast<float>(elapsedSec / budgetSec * 100.0),
          std::memory_order_relaxed);
    }
  }

  /// Get current CPU load percentage (0-100+). Thread-safe.
  float getLoadPercent() const {
    return loadPercent_.load(std::memory_order_relaxed);
  }

private:
  using Clock = std::chrono::high_resolution_clock;
  Clock::time_point blockStart_{};
  std::atomic<float> loadPercent_{0.0f};
};

} // namespace gm
