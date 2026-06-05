#pragma once
// ghostmoon DSP: ReverbSCAdapter
// Convenience wrapper around gm::ReverbSC with a single decay parameter
// that maps to both feedback and LP frequency.
// Origin: Poompatoom ReverbScAdapter (Wombletook)
//
// Decay 0-1 maps to:
//   feedback: 0.4 - 0.97 (musical sweet spot)
//   lpFreq:   2kHz - 14kHz (low decay = dark, high = bright)
//
// SPDX-License-Identifier: LGPL-2.1-or-later
//   Original wrapper (c) Wombletook, but it composes the LGPL-licensed
//   gm::ReverbSC and is distributed as part of GhostmoonGPL under
//   LGPL-2.1-or-later. See ../../../LICENSE.
//
// Zero JUCE dependency. Header-only.

#include "ReverbSC.h"

namespace gm {

class ReverbSCAdapter {
public:
    ReverbSCAdapter() = default;

    void prepare(float sampleRate) { rev_.prepare(sampleRate); }

    /// Single-knob decay control (0-1).
    /// Maps to feedback (0.4-0.97) and LP filter (2kHz-14kHz).
    void setDecay(float d) {
        decay_ = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
        rev_.setFeedback(0.4f + decay_ * 0.57f);
        rev_.setLPFreq(2000.0f + decay_ * 12000.0f);
    }

    float getDecay() const { return decay_; }

    /// Wet/dry mix 0..1
    void setMix(float m) { rev_.setMix(m); }

    void processSample(float inL, float inR, float& outL, float& outR) {
        rev_.processSample(inL, inR, outL, outR);
    }

    void reset() { rev_.reset(); }

private:
    ReverbSC rev_;
    float decay_ = 0.3f;
};

} // namespace gm
