// SPDX-License-Identifier: MIT
#pragma once
// Chrome.h — header-only JUCE 8 chrome render primitives for the Breadbin neon synthwave skin.
//
// Design reference: breadbin1/project/synthwave.css + parts.jsx (read 2026-06-05)
// JUCE API verified against: breadbin/build/_deps/juce-src (JUCE 8)
//
// This file requires the consumer's build to have JUCE on the include path.

#include <juce_gui_basics/juce_gui_basics.h>
#include <ghostmoon/ui/synthwave/Theme.h>

namespace gm::ui {

// ---------------------------------------------------------------------------
// drawPanel
// ---------------------------------------------------------------------------
// Renders a panel chrome background.
//
// From synthwave.css .bb-panel:
//   background: linear-gradient(180deg, #1C1C25 top -> #16161D bottom)
//   border: 1px solid #2C2C39
//   border-radius: 8px
//   box-shadow: 0 1px 0 rgba(255,255,255,.04) inset (top sheen),
//               0 8px 24px rgba(0,0,0,.45) (drop shadow)
//
// If glowBorder: .bb-glow-border rule:
//   box-shadow: 0 0 0 1px accent, 0 0 22px -4px accent (glow ring)
//
inline void drawPanel(juce::Graphics& g,
                      juce::Rectangle<float> bounds,
                      juce::Colour accent,
                      bool glowBorder = false)
{
    const float radius = 8.0f;

    // Drop shadow (dark, below) — approximate with a dark slightly-offset fill
    g.setColour(juce::Colour(0x73000000));  // rgba(0,0,0,.45)
    g.fillRoundedRectangle(bounds.translated(0.0f, 2.0f).expanded(0.0f, 2.0f), radius + 1.0f);

    // Main panel gradient: panel2 (#1C1C25, top) -> panel (#16161D, bottom)
    {
        juce::ColourGradient panelGrad = juce::ColourGradient::vertical(
            theme::panel2,
            theme::panel,
            bounds
        );
        g.setGradientFill(panelGrad);
        g.fillRoundedRectangle(bounds, radius);
    }

    // Glow ring (if enabled): expanding low-alpha passes to simulate box-shadow: 0 0 22px -4px accent
    if (glowBorder) {
        // Several expanding passes with low alpha, mimicking CSS box-shadow spread
        const float glowAlphas[] = { 0.55f, 0.30f, 0.15f, 0.07f };
        const float glowExpands[] = { 0.0f, 2.5f, 6.0f, 11.0f };
        for (int i = 0; i < 4; ++i) {
            g.setColour(theme::glow(accent, glowAlphas[i]));
            g.drawRoundedRectangle(bounds.expanded(glowExpands[i]), radius + glowExpands[i], 1.0f);
        }
    }

    // 1px border: #2C2C39
    g.setColour(theme::line);
    g.drawRoundedRectangle(bounds, radius, 1.0f);

    // Inset top sheen: rgba(255,255,255,.04) — 1px line at the top inner edge
    g.setColour(juce::Colour(0x0AFFFFFF));  // rgba(255,255,255,.04)
    g.drawLine(bounds.getX() + radius * 0.5f,
               bounds.getY() + 1.0f,
               bounds.getRight() - radius * 0.5f,
               bounds.getY() + 1.0f,
               1.0f);
}

// ---------------------------------------------------------------------------
// drawGlowText
// ---------------------------------------------------------------------------
// Renders text with a phosphor glow halo.
//
// From synthwave.css .glow-cy:
//   color: accent
//   text-shadow: 0 0 8px rgba(accent,.55), 0 0 2px rgba(accent,.9)
//
// Implemented as: 2-3 layered draws of the text at glow alpha, then sharp pass on top.
//
inline void drawGlowText(juce::Graphics& g,
                         const juce::String& text,
                         const juce::Font& font,
                         juce::Rectangle<float> bounds,
                         juce::Colour accent,
                         juce::Justification justification)
{
    g.setFont(font);

    // Bloom passes (no offset — the halo comes from layering)
    // Match .glow-cy: wide spread at .55 alpha, tight spread at .5 alpha
    g.setColour(theme::glow(accent, 0.35f));
    g.drawText(text, bounds, justification, false);
    g.setColour(theme::glow(accent, 0.50f));
    g.drawText(text, bounds, justification, false);

    // Sharp pass on top (full accent colour)
    g.setColour(accent);
    g.drawText(text, bounds, justification, false);
}

// ---------------------------------------------------------------------------
// drawComboBackground
// ---------------------------------------------------------------------------
// Renders the background of a combo box widget (caller draws value text).
//
// From synthwave.css .bb-combo:
//   background: linear-gradient(180deg, #202028, #17171E)
//   border: 1px solid #3A3A4A
//   border-radius: 5px
//   box-shadow: inset 0 1px 0 rgba(255,255,255,.04)
//
inline void drawComboBackground(juce::Graphics& g,
                                juce::Rectangle<float> bounds)
{
    const float radius = 5.0f;

    // Vertical gradient #202028 -> #17171E
    {
        juce::ColourGradient comboGrad = juce::ColourGradient::vertical(
            juce::Colour(0xFF202028),
            juce::Colour(0xFF17171E),
            bounds
        );
        g.setGradientFill(comboGrad);
        g.fillRoundedRectangle(bounds, radius);
    }

    // 1px border: #3A3A4A (line2)
    g.setColour(theme::line2);
    g.drawRoundedRectangle(bounds, radius, 1.0f);

    // Inset top sheen: rgba(255,255,255,.04)
    g.setColour(juce::Colour(0x0AFFFFFF));
    g.drawLine(bounds.getX() + radius * 0.4f,
               bounds.getY() + 1.0f,
               bounds.getRight() - radius * 0.4f,
               bounds.getY() + 1.0f,
               1.0f);
}

// ---------------------------------------------------------------------------
// drawToggleDot
// ---------------------------------------------------------------------------
// Renders a 10x10-ish rounded-square toggle indicator dot.
//
// From synthwave.css .bb-tgl .dot (off state):
//   width: 10px; height: 10px; border-radius: 3px
//   background: #0C0C10; border: 1px solid #3A3A4A
//   box-shadow: inset 0 1px 2px #000
//
// .bb-tgl.on.cy .dot (on state, accent = cyan):
//   background: accent; border-color: accent
//   box-shadow: 0 0 8px rgba(accent,.8)
//
inline void drawToggleDot(juce::Graphics& g,
                          juce::Rectangle<float> bounds,
                          juce::Colour accent,
                          bool on)
{
    const float dotSize = 10.0f;
    const float radius  = 3.0f;

    // Centre the 10x10 dot within bounds
    const float dotX = bounds.getCentreX() - dotSize * 0.5f;
    const float dotY = bounds.getCentreY() - dotSize * 0.5f;
    const juce::Rectangle<float> dotRect(dotX, dotY, dotSize, dotSize);

    if (on) {
        // Glow passes: box-shadow: 0 0 8px rgba(accent,.8)
        // A couple expanding low-alpha strokes outside the dot
        const float glowAlphas[] = { 0.40f, 0.20f };
        const float glowExpands[] = { 2.0f, 4.5f };
        for (int i = 0; i < 2; ++i) {
            g.setColour(theme::glow(accent, glowAlphas[i]));
            g.fillRoundedRectangle(dotRect.expanded(glowExpands[i]), radius + glowExpands[i]);
        }
        // Fill: accent
        g.setColour(accent);
        g.fillRoundedRectangle(dotRect, radius);
        // Border: accent
        g.setColour(accent);
        g.drawRoundedRectangle(dotRect, radius, 1.0f);
    } else {
        // Fill: #0C0C10
        g.setColour(juce::Colour(0xFF0C0C10));
        g.fillRoundedRectangle(dotRect, radius);
        // Inset shadow hint: dark strip at bottom of dot
        g.setColour(juce::Colour(0xFF000000));
        g.fillRoundedRectangle(dotRect.withTrimmedTop(dotSize - 3.0f), radius * 0.5f);
        // Border: #3A3A4A (line2)
        g.setColour(theme::line2);
        g.drawRoundedRectangle(dotRect, radius, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// drawButtonBackground
// ---------------------------------------------------------------------------
// Renders a button chrome background.
//
// From synthwave.css .bb-btn:
//   background: linear-gradient(180deg, #2A2A34, #1D1D25)
//   border: 1px solid #3A3A4A
//   border-radius: 5px
//   box-shadow: 0 1px 0 rgba(255,255,255,.05) inset, 0 2px 4px rgba(0,0,0,.4)
//
// When down: darken slightly (swap or darken gradient), invert sheen to bottom.
//
inline void drawButtonBackground(juce::Graphics& g,
                                 juce::Rectangle<float> bounds,
                                 bool down)
{
    const float radius = 5.0f;

    // Drop shadow (behind button)
    g.setColour(juce::Colour(0x66000000));  // rgba(0,0,0,.4)
    g.fillRoundedRectangle(bounds.translated(0.0f, 1.5f).expanded(0.0f, 0.5f), radius + 0.5f);

    if (down) {
        // Pressed: slightly darker gradient
        juce::ColourGradient btnGrad = juce::ColourGradient::vertical(
            juce::Colour(0xFF1A1A20),  // darker than normal top
            juce::Colour(0xFF252530),  // slightly brighter at bottom (inverted)
            bounds
        );
        g.setGradientFill(btnGrad);
    } else {
        // Normal: #2A2A34 -> #1D1D25
        juce::ColourGradient btnGrad = juce::ColourGradient::vertical(
            juce::Colour(0xFF2A2A34),
            juce::Colour(0xFF1D1D25),
            bounds
        );
        g.setGradientFill(btnGrad);
    }
    g.fillRoundedRectangle(bounds, radius);

    // 1px border: #3A3A4A (line2)
    g.setColour(theme::line2);
    g.drawRoundedRectangle(bounds, radius, 1.0f);

    // Inset sheen
    if (down) {
        // Pressed: sheen at bottom instead of top
        g.setColour(juce::Colour(0x0DFFFFFF));  // rgba(255,255,255,.05) approx
        g.drawLine(bounds.getX() + radius * 0.4f,
                   bounds.getBottom() - 1.5f,
                   bounds.getRight() - radius * 0.4f,
                   bounds.getBottom() - 1.5f,
                   1.0f);
    } else {
        // Normal: sheen at top
        g.setColour(juce::Colour(0x0DFFFFFF));  // rgba(255,255,255,.05)
        g.drawLine(bounds.getX() + radius * 0.4f,
                   bounds.getY() + 1.0f,
                   bounds.getRight() - radius * 0.4f,
                   bounds.getY() + 1.0f,
                   1.0f);
    }
}

} // namespace gm::ui
