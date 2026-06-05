// SPDX-License-Identifier: MIT
#pragma once
// Scope.h — header-only JUCE 8 CRT phosphor scope render primitives for the Breadbin skin.
//
// Design reference: breadbin1/project/synthwave.css .bb-scope/.scope-fx + parts.jsx
//                   FilterGraph / LFOGraph components (read 2026-06-05)
// JUCE API verified against: breadbin/build/_deps/juce-src (JUCE 8)
//
// This file requires the consumer's build to have JUCE on the include path.

#include <juce_gui_basics/juce_gui_basics.h>
#include <ghostmoon/ui/synthwave/Theme.h>

namespace gm::ui {

// ---------------------------------------------------------------------------
// drawScopeBackground
// ---------------------------------------------------------------------------
// Renders the CRT phosphor scope background.
//
// From synthwave.css .bb-scope:
//   background: radial-gradient(125% 150% at 50% 22%, #0A1417, #05080A 86%)
//   border: 1px solid #000
//   border-radius: 5px
//   box-shadow: inset 0 2px 7px rgba(0,0,0,.72), inset 0 0 0 1px rgba(255,255,255,.02)
//
// JUCE ColourGradient with isRadial=true places centre at (x1,y1) and maps
// the given colour at that point, with the second colour at (x2,y2).
// We approximate the radial as: centre ~(50%, 22%), spreading to the corners.
//
inline void drawScopeBackground(juce::Graphics& g,
                                juce::Rectangle<float> bounds)
{
    const float radius = 5.0f;
    const float cx = bounds.getCentreX();
    const float cy = bounds.getY() + bounds.getHeight() * 0.22f;  // 22% from top
    // Edge point: lower-right corner (approximates radial reaching 86% at edge)
    const float ex = bounds.getRight();
    const float ey = bounds.getBottom();

    // Radial gradient: #0A1417 (centre) -> #05080A (outer)
    {
        juce::ColourGradient scopeGrad(
            juce::Colour(0xFF0A1417),  // centre colour
            cx, cy,
            juce::Colour(0xFF05080A),  // outer colour
            ex, ey,
            true  // isRadial
        );
        g.setGradientFill(scopeGrad);
        g.fillRoundedRectangle(bounds, radius);
    }

    // 1px black border
    g.setColour(juce::Colours::black);
    g.drawRoundedRectangle(bounds, radius, 1.0f);

    // Inset shadow: dark at top — 0 2px 7px rgba(0,0,0,.72)
    {
        juce::ColourGradient insetShadow = juce::ColourGradient::vertical(
            juce::Colour(0xB8000000),   // rgba(0,0,0,.72) at top
            juce::Colour(0x00000000),   // transparent at bottom of shadow zone
            juce::Rectangle<float>(bounds.getX(), bounds.getY(),
                                   bounds.getWidth(), juce::jmin(14.0f, bounds.getHeight()))
        );
        g.setGradientFill(insetShadow);
        g.fillRoundedRectangle(bounds, radius);
    }

    // Subtle inset highlight: 0 0 0 1px rgba(255,255,255,.02)
    g.setColour(juce::Colour(0x05FFFFFF));  // rgba(255,255,255,.02)
    g.drawRoundedRectangle(bounds.reduced(0.5f), radius - 0.5f, 1.0f);
}

// ---------------------------------------------------------------------------
// makeScanlineOverlay
// ---------------------------------------------------------------------------
// Creates a scanline overlay image for the CRT scope effect.
//
// From synthwave.css .bb-scope .scope-fx:
//   background: repeating-linear-gradient(
//     0deg,
//     rgba(0,0,0,0) 0 1.4px,
//     rgba(0,0,0, calc(.34 * var(--scope, 1))) 2.3px
//   )
//
// Pattern: transparent gap 0..1.4px, then dark line at 2.3px.
// Period = 2.3px. Line alpha = 0.34 * scopeFactor.
//
// The returned image is intended to be cached by the caller and drawn via
// Graphics::drawImageAt / drawImage with reduced opacity or directly.
//
inline juce::Image makeScanlineOverlay(int w, int h, float scopeFactor = 1.48f)
{
    juce::Image img(juce::Image::ARGB, w, h, true);  // true = clear to transparent
    juce::Graphics gImg(img);

    const float lineAlpha = juce::jlimit(0.0f, 1.0f, 0.34f * scopeFactor);
    const juce::Colour lineColour = juce::Colours::black.withAlpha(lineAlpha);

    // Pattern: period ~2.3px, line at the 2.3px mark (1px wide), gap of 1.4px before it.
    // We draw a 1px dark line every ~2.3px (at y = 1 (0-indexed), 3, 5, ... rounding to period).
    const float period = 2.3f;
    gImg.setColour(lineColour);
    float y = period - 1.0f;  // first line position (gap is 1.4px, line at 2.3px)
    while (y < static_cast<float>(h)) {
        gImg.drawHorizontalLine(static_cast<int>(y), 0.0f, static_cast<float>(w));
        y += period;
    }

    return img;
}

// ---------------------------------------------------------------------------
// drawScopeTrace
// ---------------------------------------------------------------------------
// Renders a waveform/filter-response trace with a CRT phosphor glow effect.
//
// From parts.jsx FilterGraph / LFOGraph:
//   Bloom pass: stroke width 3.5, opacity 0.55 via feGaussianBlur (approximated
//               as a thick glow(accent, .5) stroke)
//   Sharp pass: stroke width 1.3, accent colour at full opacity
//
inline void drawScopeTrace(juce::Graphics& g,
                            const juce::Path& path,
                            juce::Colour accent)
{
    // Bloom pass: wide stroke in glow colour (approximates feGaussianBlur drop-shadow)
    g.setColour(theme::glow(accent, 0.5f));
    g.strokePath(path, juce::PathStrokeType(3.5f,
                                             juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    // Sharp pass: thin crisp line on top
    g.setColour(accent);
    g.strokePath(path, juce::PathStrokeType(1.3f,
                                             juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
}

} // namespace gm::ui
