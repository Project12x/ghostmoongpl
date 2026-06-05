// SPDX-License-Identifier: MIT
#pragma once
// ScaledEditor — DPI-aware plugin editor base
// Adapted from the Joychord scale pattern.
// Ported from bb::ScaledEditor (D:\Code\breadbin\src\ScaledEditor.h) into gm::ui.
//
// Usage:
//   class MyEditor : public gm::ui::ScaledEditor {
//   public:
//       MyEditor(MyProcessor& p)
//           : ScaledEditor(p, 1000, 800) {
//           // add children ...
//           setScale(1.0f);  // trigger initial layout
//       }
//       void resizedContent() override { /* layout using getLocalBounds() */ }
//   };
//
// setScale() applies AffineTransform::scale(s), which causes the host/
// standalone window to resize to baseW*s x baseH*s physical pixels.
// The component's logical bounds stay at baseW x baseH, so resizedContent()
// and getLocalBounds() always return the base resolution.

#include <juce_audio_processors/juce_audio_processors.h>

namespace gm::ui {

class ScaledEditor : public juce::AudioProcessorEditor {
public:
  ScaledEditor(juce::AudioProcessor &processor, int baseWidth, int baseHeight)
      : juce::AudioProcessorEditor(processor),
        baseWidth_(baseWidth), baseHeight_(baseHeight) {}

  void setScale(float s) {
    currentScale_ = s;
    setSize(baseWidth_, baseHeight_);
    setTransform(juce::AffineTransform::scale(s));
  }

  float getCurrentScale() const { return currentScale_; }
  int getBaseWidth() const { return baseWidth_; }
  int getBaseHeight() const { return baseHeight_; }

  virtual void resizedContent() {}

  void resized() override { resizedContent(); }

private:
  int baseWidth_, baseHeight_;
  float currentScale_ = 1.0f;
};

} // namespace gm::ui
