// SPDX-License-Identifier: MIT
#pragma once
#include <utility>
#include <algorithm>

namespace gm::ui {

// Returns the normalised value clamped to [0, 1].
// For a 270-degree knob the value proportion IS the clamp — 0 maps to start
// of sweep, 1 maps to end of sweep.
inline float knobSweepProportion(float v01) {
    return std::clamp(v01, 0.0f, 1.0f);
}

// Returns {arcStart, arcEnd} for a bipolar fill drawn from the 12-o'clock
// centre (0.5) toward the knob value.
//   v01 >= 0.5 : fill from 0.5 to v01  -> {0.5, v01}
//   v01 <  0.5 : fill from v01 to 0.5  -> {v01, 0.5}
inline std::pair<float, float> bipolarFill(float v01) {
    v01 = std::clamp(v01, 0.0f, 1.0f);
    return v01 >= 0.5f
        ? std::pair<float, float>{0.5f, v01}
        : std::pair<float, float>{v01, 0.5f};
}

} // namespace gm::ui
