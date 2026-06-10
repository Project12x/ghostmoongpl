// SPDX-License-Identifier: MIT
#pragma once
// ghostmoon DSP: SilenceGate
//
// First-party ghostmoon-oss export for block-level activity gating. Reuse mode:
// clean direct implementation from the Moonglow optimization plan
// docs/superpowers/plans/2026-06-10-moonglow-cpu-optimization-sequence.md
// at Moonglow commit c31a2c38ea56d0f72a1d8bb79243d62c0ea41569.
//
// The gate closes only after both the input/source side and the measured output
// tail are silent for the configured hold period. This keeps resonant or
// self-ringing stages open until their output energy has actually decayed.

#include <algorithm>
#include <cmath>

namespace gm {

class SilenceGate {
public:
  void prepare(double, int) noexcept { reset(); }

  void reset() noexcept {
    silentBlocks_ = 0;
    skipping_ = false;
  }

  void setThreshold(float threshold) noexcept {
    if (std::isfinite(threshold))
      threshold_ = std::max(0.0f, threshold);
  }

  void setHoldBlocks(int blocks) noexcept {
    holdBlocks_ = std::max(1, blocks);
  }

  bool update(float inputPeak, float tailPeak) noexcept {
    const bool inputSilent =
        std::isfinite(inputPeak) && std::abs(inputPeak) <= threshold_;
    const bool tailSilent =
        std::isfinite(tailPeak) && std::abs(tailPeak) <= threshold_;

    if (!inputSilent || !tailSilent) {
      silentBlocks_ = 0;
      skipping_ = false;
      return false;
    }

    ++silentBlocks_;
    skipping_ = silentBlocks_ >= holdBlocks_;
    return skipping_;
  }

  bool shouldSkip() const noexcept { return skipping_; }

private:
  float threshold_ = 1.0e-5f;
  int holdBlocks_ = 8;
  int silentBlocks_ = 0;
  bool skipping_ = false;
};

} // namespace gm
