// SPDX-License-Identifier: MIT
#pragma once
// Oscilloscope — skin-neutral real-time waveform capture.
//
// Ring-buffer + zero-crossing (rising-edge) trigger, double-buffered for
// lock-free UI reads. Exposes the captured frame as raw points and as a
// juce::Path; the consumer strokes it in whatever skin it likes. This class
// has no colours, no juce::Component, and no timer — it is pure scope data.
//
// Ported from the Ghostmoon catalog Oscilloscope (data path only; origin
// Pedalboard3 OscilloscopeControl/Processor) into gm::ui.
//
// Usage:
//   gm::ui::Oscilloscope scope;
//   // audio thread:  scope.process(buffer.getReadPointer(0), numSamples);
//   // UI thread:      auto p = scope.makePath(bounds); g.strokePath(p, ...);

#include <juce_graphics/juce_graphics.h>
#include <array>
#include <atomic>

namespace gm::ui {

class Oscilloscope {
public:
  static constexpr int kNumPoints = 512;

  Oscilloscope() {
    displayBuffers_[0].fill(0.0f);
    displayBuffers_[1].fill(0.0f);
  }

  // ----- Audio thread -----
  // Feed samples; captures a zero-cross-triggered snapshot into the back
  // buffer and atomically publishes it when a full frame is collected.
  void process(const float* samples, int numSamples) {
    const float trigger = triggerLevel_.load(std::memory_order_relaxed);
    for (int i = 0; i < numSamples; ++i) {
      const float s = samples[i];

      // Zero-crossing trigger (rising edge), re-armed once a frame completes.
      const bool isNeg = s < trigger;
      if (lastNeg_ && !isNeg && samplesSinceTrigger_ >= kNumPoints)
        samplesSinceTrigger_ = 0;
      lastNeg_ = isNeg;

      if (samplesSinceTrigger_ < kNumPoints) {
        displayBuffers_[backBuffer_][samplesSinceTrigger_] = s;
        ++samplesSinceTrigger_;
        if (samplesSinceTrigger_ >= kNumPoints) {
          frontBuffer_.store(backBuffer_, std::memory_order_release);
          backBuffer_ = 1 - backBuffer_;
        }
      }
    }
  }

  // ----- Configuration -----
  void  setTriggerLevel(float level) { triggerLevel_.store(level, std::memory_order_relaxed); }
  float getTriggerLevel() const      { return triggerLevel_.load(std::memory_order_relaxed); }

  // ----- UI thread: read the latest triggered frame -----
  // Pointer to the current front display buffer (kNumPoints samples, ~[-1,1]).
  const float* displayData() const {
    return displayBuffers_[frontBuffer_.load(std::memory_order_acquire)].data();
  }

  // Copy the front frame into dest[kNumPoints] (JUCE-free convenience).
  void copyDisplay(float* dest) const {
    const float* src = displayData();
    for (int i = 0; i < kNumPoints; ++i) dest[i] = src[i];
  }

  // Build a vertically-centred waveform path across `bounds`. `verticalScale`
  // is the fraction of the bounds height used for full-scale (+/-1.0). No
  // colour or stroke is applied — the caller strokes it in its own skin.
  juce::Path makePath(juce::Rectangle<float> bounds, float verticalScale = 0.45f) const {
    juce::Path path;
    const float* buf    = displayData();
    const float  xScale = bounds.getWidth() / static_cast<float>(kNumPoints);
    const float  yScale = bounds.getHeight() * verticalScale;
    const float  cy     = bounds.getCentreY();
    for (int i = 0; i < kNumPoints; ++i) {
      const float x = bounds.getX() + static_cast<float>(i) * xScale;
      const float y = juce::jlimit(bounds.getY(), bounds.getBottom(), cy - buf[i] * yScale);
      if (i == 0) path.startNewSubPath(x, y);
      else        path.lineTo(x, y);
    }
    return path;
  }

private:
  std::array<float, kNumPoints> displayBuffers_[2];
  std::atomic<int>   frontBuffer_{0};
  int                backBuffer_ = 1;
  std::atomic<float> triggerLevel_{0.0f};
  bool               lastNeg_ = true;
  int                samplesSinceTrigger_ = kNumPoints;
};

} // namespace gm::ui
