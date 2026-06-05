#pragma once
#include <juce_graphics/juce_graphics.h>
namespace gm::ui::theme {
  // structure
  inline const juce::Colour bg0{0xFF0A0A0E}, bg1{0xFF13131A}, panel{0xFF16161D},
    panel2{0xFF1C1C25}, panel3{0xFF22222D}, line{0xFF2C2C39}, line2{0xFF3A3A4A},
    inset{0xFF0E0E13};
  // text
  inline const juce::Colour txt{0xFFE7E7F0}, txt2{0xFFA6A6B8}, txt3{0xFF6F6F82};
  // neon accents
  inline const juce::Colour cyan{0xFF33EDED}, cyanD{0xFF1AA6A6}, orange{0xFFFFAE3B},
    orangeD{0xFFC97F1E}, grn{0xFFB6F23C}, grnD{0xFF7FAE23}, mag{0xFFFF3DF0},
    purple{0xFF9A6BFF}, gold{0xFFFFCB45}, red{0xFFFF5468}, lime{0xFF5DFF7A}, yellow{0xFFFFE14D};
  // C64 VIC-II (popups, later phase)
  inline const juce::Colour cblue{0xFF8B80E8}, cgrn{0xFF9AD284}, cyel{0xFFD6DD7E},
    cred{0xFFD08A72}, cpur{0xFFB98AE0}, beige{0xFFD8C79F};
  inline juce::Colour glow(juce::Colour c, float a) { return c.withAlpha(a); }
}
