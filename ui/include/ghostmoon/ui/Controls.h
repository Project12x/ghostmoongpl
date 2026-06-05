#pragma once
// Controls.h — header-only JUCE 8 render primitives for the Breadbin neon synthwave skin.
//
// Design reference: breadbin1/project/parts.jsx + synthwave.css (read 2026-06-05)
// JUCE API verified against: breadbin/build/_deps/juce-src (JUCE 8)
//
// This file requires the consumer's build to have JUCE on the include path.
// It is NOT compiled by ghostmoongpl_ui_tests (that target is JUCE-free).

#include <juce_gui_basics/juce_gui_basics.h>
#include <ghostmoon/ui/Theme.h>
#include <ghostmoon/ui/Geometry.h>

namespace gm::ui {

// ---------------------------------------------------------------------------
// Internal helpers (not part of the public API)
// ---------------------------------------------------------------------------
namespace detail {

// parts.jsx uses degrees measured clockwise from 3 o'clock (SVG convention).
// JUCE addCentredArc uses radians measured clockwise from 3 o'clock.
// The knob track in parts.jsx: arcPath(cx,cy,r, 135, 405)
//   135 deg CW from east = 7 o'clock  (start of sweep)
//   405 deg CW from east = 135+270    (end of sweep)
// In JUCE radians (same convention):
static constexpr float kKnobStartRad = 135.0f * juce::MathConstants<float>::pi / 180.0f;
static constexpr float kKnobEndRad   = 405.0f * juce::MathConstants<float>::pi / 180.0f;
static constexpr float kKnobSweep    = 270.0f * juce::MathConstants<float>::pi / 180.0f;

// Knob track colour from parts.jsx: stroke="#26262f"
inline juce::Colour knobTrackColour()  { return juce::Colour(0xFF26262F); }
// Cap highlight colour from parts.jsx: box-shadow inset rgba(255,255,255,.08)
inline juce::Colour capHighlight()     { return juce::Colour(0x14FFFFFF); }
// Cap shadow colour from parts.jsx: box-shadow 0 2px 5px rgba(0,0,0,.6)
inline juce::Colour capShadow()        { return juce::Colour(0x99000000); }

// Draw a centred arc using JUCE Path + Graphics::strokePath.
// angleFrom/angleTo in radians, JUCE CW-from-east convention.
inline void strokeArc(juce::Graphics& g, float cx, float cy, float r,
                      float angleFrom, float angleTo,
                      float strokeWidth,
                      juce::PathStrokeType::EndCapStyle caps = juce::PathStrokeType::rounded)
{
    juce::Path p;
    p.addCentredArc(cx, cy, r, r, 0.0f, angleFrom, angleTo, true);
    g.strokePath(p, juce::PathStrokeType(strokeWidth,
                                         juce::PathStrokeType::curved, caps));
}

// Draw an arc with a glow pass underneath (thick low-alpha) then a sharp pass on top.
// Matches the CSS filter:drop-shadow pattern used in parts.jsx for accent arcs.
inline void strokeArcGlow(juce::Graphics& g, float cx, float cy, float r,
                           float angleFrom, float angleTo,
                           float strokeWidth, juce::Colour accent)
{
    // Glow pass: thick, half-alpha
    g.setColour(theme::glow(accent, 0.5f));
    strokeArc(g, cx, cy, r, angleFrom, angleTo, strokeWidth * 3.0f,
              juce::PathStrokeType::rounded);
    // Sharp pass on top
    g.setColour(accent);
    strokeArc(g, cx, cy, r, angleFrom, angleTo, strokeWidth,
              juce::PathStrokeType::rounded);
}

} // namespace detail

// ---------------------------------------------------------------------------
// drawKnob
// ---------------------------------------------------------------------------
// Renders a 270° sweep rotary knob as described in parts.jsx (Knob component).
//
// Layout (from parts.jsx):
//   r      = size * 0.43  (arc radius)
//   capR   = size * 0.30  (cap radius)
//   track  : 135°→405°, stroke "#26262f", width 3, round caps
//   value arc: accent, width 3, glow(accent, 0.5) underneath
//   bipolar: fill from 270° (12 o'clock) toward value angle
//   cap    : radial gradient #3A3A46 → #15151C (highlight at ~38%,30%)
//            1px #000 border, inner top highlight rgba(255,255,255,.08)
//   pointer: short bar in accent rotated to (135 + v01*270)°, with glow
//            length ≈ capR*0.92, starts at capR*0.18 from centre
//
inline void drawKnob(juce::Graphics& g,
                     juce::Rectangle<float> bounds,
                     float v01,
                     juce::Colour accent,
                     bool bipolar = false)
{
    v01 = knobSweepProportion(v01);

    const float size  = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const float cx    = bounds.getCentreX();
    const float cy    = bounds.getCentreY();
    const float r     = size * 0.43f;
    const float capR  = size * 0.30f;

    using namespace detail;

    // --- Track arc (background) ---
    g.setColour(knobTrackColour());
    strokeArc(g, cx, cy, r, kKnobStartRad, kKnobEndRad, 3.0f,
              juce::PathStrokeType::rounded);

    // --- Value arc ---
    // parts.jsx: valFrom = bipolar ? min(270, ang) : 135
    //            valTo   = bipolar ? max(270, ang) : ang
    // where ang = 135 + value*270  (degrees)
    // Translate to radians (same formula, just in rad):
    const float angRad = kKnobStartRad + v01 * kKnobSweep;

    if (bipolar) {
        // 12-o'clock = 270° from east = 270*pi/180 rad
        static constexpr float k12 = 270.0f * juce::MathConstants<float>::pi / 180.0f;
        const float arcFrom = juce::jmin(k12, angRad);
        const float arcTo   = juce::jmax(k12, angRad);
        if ((arcTo - arcFrom) > 0.01f)
            strokeArcGlow(g, cx, cy, r, arcFrom, arcTo, 3.0f, accent);
    } else {
        if (v01 > 0.001f)
            strokeArcGlow(g, cx, cy, r, kKnobStartRad, angRad, 3.0f, accent);
    }

    // --- Cap (circular face) ---
    // Radial gradient: #3A3A46 → #15151C, highlight at ~38%,30%
    // JUCE ColourGradient(colour1, x1, y1, colour2, x2, y2, isRadial):
    //   centre point = highlight position (38%,30% of diameter from top-left of cap)
    const float capLeft = cx - capR;
    const float capTop  = cy - capR;
    {
        juce::ColourGradient capGrad(
            juce::Colour(0xFF3A3A46),
            capLeft + capR * 2.0f * 0.38f,   // highlight X
            capTop  + capR * 2.0f * 0.30f,   // highlight Y
            juce::Colour(0xFF15151C),
            cx, cy + capR,                    // edge point
            true  // isRadial
        );
        g.setGradientFill(capGrad);
        g.fillEllipse(capLeft, capTop, capR * 2.0f, capR * 2.0f);
    }

    // 1px black border
    g.setColour(juce::Colours::black);
    g.drawEllipse(capLeft, capTop, capR * 2.0f, capR * 2.0f, 1.0f);

    // Subtle inner top highlight: rgba(255,255,255,.08)
    {
        // Draw a thin crescent highlight near the top of the cap
        juce::ColourGradient innerHL(
            capHighlight(),
            cx, capTop,
            juce::Colours::transparentBlack,
            cx, capTop + capR * 0.7f,
            false
        );
        g.setGradientFill(innerHL);
        g.fillEllipse(capLeft + 1.0f, capTop + 1.0f,
                      capR * 2.0f - 2.0f, capR * 0.8f);
    }

    // --- Pointer bar ---
    // parts.jsx: width=capR*0.92, height=2, transformOrigin='0 50%',
    //            transform=`rotate(${ang}deg) translateX(${capR*0.18}px)`
    // The pointer rotates around (cx, cy), bar starts at capR*0.18 from centre.
    const float pointerLen    = capR * 0.92f;
    const float pointerOffset = capR * 0.18f;
    const float angDeg        = 135.0f + v01 * 270.0f;
    const float angDegRad     = angDeg * juce::MathConstants<float>::pi / 180.0f;

    // Glow pass
    {
        juce::Path pointer;
        pointer.startNewSubPath(cx + pointerOffset * std::cos(angDegRad),
                                cy + pointerOffset * std::sin(angDegRad));
        pointer.lineTo(cx + (pointerOffset + pointerLen) * std::cos(angDegRad),
                       cy + (pointerOffset + pointerLen) * std::sin(angDegRad));
        g.setColour(theme::glow(accent, 0.5f));
        g.strokePath(pointer, juce::PathStrokeType(4.0f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        g.setColour(accent);
        g.strokePath(pointer, juce::PathStrokeType(2.0f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }
}

// ---------------------------------------------------------------------------
// drawHSlider
// ---------------------------------------------------------------------------
// Renders an HSlider as described in parts.jsx (HSlider / bb-h component)
// and synthwave.css (.bb-h rules).
//
// Layout (from synthwave.css + parts.jsx):
//   Container height: 18px
//   Track: full width, height 6, radius 3, fill theme::inset,
//          inset shadow (inset 0 1px 3px #000), 1px #000 border
//   Fill:  height 6, radius 3, accent colour + glow(accent,.5)
//   Bipolar: fill from left:50% toward value; 1px centre tick #4A4A5A
//   Thumb: 13×18, radius 3, vertical gradient #D2D2DC→#5A5A66,
//          1px #000 border, drop shadow (0 2px 4px rgba(0,0,0,.6))
//
inline void drawHSlider(juce::Graphics& g,
                        juce::Rectangle<float> bounds,
                        float v01,
                        juce::Colour accent,
                        bool bipolar = false)
{
    v01 = knobSweepProportion(v01);

    const float bx = bounds.getX();
    const float by = bounds.getY();
    const float bw = bounds.getWidth();
    const float bh = bounds.getHeight();

    // Track dimensions
    const float trackH  = 6.0f;
    const float trackY  = by + (bh - trackH) * 0.5f;
    const float trackR  = 3.0f;

    // --- Track ---
    g.setColour(theme::inset);
    g.fillRoundedRectangle(bx, trackY, bw, trackH, trackR);
    // 1px black border
    g.setColour(juce::Colours::black);
    g.drawRoundedRectangle(bx, trackY, bw, trackH, trackR, 1.0f);

    // --- Fill ---
    {
        float fillX, fillW;
        if (bipolar) {
            // parts.jsx: left=min(0.5,value)*100%, width=abs(value-0.5)*100%
            fillX = bx + juce::jmin(0.5f, v01) * bw;
            fillW = std::fabs(v01 - 0.5f) * bw;
        } else {
            fillX = bx;
            fillW = v01 * bw;
        }
        if (fillW > 0.5f) {
            // Glow layer
            g.setColour(theme::glow(accent, 0.5f));
            g.fillRoundedRectangle(fillX, trackY, fillW, trackH, trackR);
            // Sharp fill on top
            g.setColour(accent);
            g.fillRoundedRectangle(fillX, trackY, fillW, trackH, trackR);
        }
    }

    // Bipolar centre tick: 1px, full track height, colour #4A4A5A
    if (bipolar) {
        g.setColour(juce::Colour(0xFF4A4A5A));
        g.fillRect(bx + bw * 0.5f - 0.5f, trackY, 1.0f, trackH);
    }

    // --- Thumb: 13×18, vertical gradient #D2D2DC→#5A5A66 ---
    const float thumbW = 13.0f;
    const float thumbH = bh;  // full slot height = 18px
    const float thumbX = bx + v01 * bw - thumbW * 0.5f;
    const float thumbY = by;
    const float thumbR = 3.0f;

    {
        juce::ColourGradient thumbGrad = juce::ColourGradient::vertical(
            juce::Colour(0xFFD2D2DC),
            juce::Colour(0xFF5A5A66),
            juce::Rectangle<float>(thumbX, thumbY, thumbW, thumbH)
        );
        g.setGradientFill(thumbGrad);
        g.fillRoundedRectangle(thumbX, thumbY, thumbW, thumbH, thumbR);
    }
    g.setColour(juce::Colours::black);
    g.drawRoundedRectangle(thumbX, thumbY, thumbW, thumbH, thumbR, 1.0f);
    // Drop shadow hint: draw a dark strip near the bottom of the thumb
    g.setColour(juce::Colour(0x99000000));
    g.fillRoundedRectangle(thumbX + 1.0f, thumbY + thumbH - 3.0f,
                            thumbW - 2.0f, 2.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// drawVSlider
// ---------------------------------------------------------------------------
// Renders a VSlider as described in parts.jsx (VSlider / bb-v component)
// and synthwave.css (.bb-v rules).
//
// Layout (from synthwave.css + parts.jsx):
//   Container width: 18px
//   Track: centred horizontally, full height, width 6, radius 3,
//          fill theme::inset, inset shadow + 1px #000 border
//   Fill:  from bottom, width 6, accent colour + glow(accent,.5)
//   Thumb: 18×11, radius 3, vertical gradient #D2D2DC→#5A5A66,
//          1px #000 border, drop shadow — positioned at bottom of fill
//          (transform: translateY(50%) in CSS = centred on fill top edge)
//
inline void drawVSlider(juce::Graphics& g,
                        juce::Rectangle<float> bounds,
                        float v01,
                        juce::Colour accent,
                        bool /*bipolar*/ = false)
{
    // VSlider has no bipolar mode in parts.jsx
    v01 = knobSweepProportion(v01);

    const float bx = bounds.getX();
    const float by = bounds.getY();
    const float bw = bounds.getWidth();
    const float bh = bounds.getHeight();

    // Track dimensions
    const float trackW  = 6.0f;
    const float trackX  = bx + (bw - trackW) * 0.5f;
    const float trackR  = 3.0f;

    // --- Track ---
    g.setColour(theme::inset);
    g.fillRoundedRectangle(trackX, by, trackW, bh, trackR);
    g.setColour(juce::Colours::black);
    g.drawRoundedRectangle(trackX, by, trackW, bh, trackR, 1.0f);

    // --- Fill (from bottom): height = v01 * bh ---
    const float fillH = v01 * bh;
    const float fillY = by + bh - fillH;
    if (fillH > 0.5f) {
        // Glow layer
        g.setColour(theme::glow(accent, 0.5f));
        g.fillRoundedRectangle(trackX, fillY, trackW, fillH, trackR);
        // Sharp fill on top
        g.setColour(accent);
        g.fillRoundedRectangle(trackX, fillY, trackW, fillH, trackR);
    }

    // --- Thumb: 18×11, centred on fill-top edge (translateY(50%) in CSS) ---
    const float thumbW = bw;   // full slot width = 18px
    const float thumbH = 11.0f;
    const float thumbX = bx;
    const float thumbY = fillY - thumbH * 0.5f;  // centred on fill top
    const float thumbR = 3.0f;

    {
        juce::ColourGradient thumbGrad = juce::ColourGradient::vertical(
            juce::Colour(0xFFD2D2DC),
            juce::Colour(0xFF5A5A66),
            juce::Rectangle<float>(thumbX, thumbY, thumbW, thumbH)
        );
        g.setGradientFill(thumbGrad);
        g.fillRoundedRectangle(thumbX, thumbY, thumbW, thumbH, thumbR);
    }
    g.setColour(juce::Colours::black);
    g.drawRoundedRectangle(thumbX, thumbY, thumbW, thumbH, thumbR, 1.0f);
    // Drop shadow hint
    g.setColour(juce::Colour(0x99000000));
    g.fillRoundedRectangle(thumbX + 1.0f, thumbY + thumbH - 3.0f,
                            thumbW - 2.0f, 2.0f, 1.0f);
}

} // namespace gm::ui
