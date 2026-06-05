# ghostmoon-oss

ghostmoon-oss is the distributable open-source slice of the Ghostmoon audio
library, intended for GPL plugins (Nessy, Breadbin, Pedalboard3) that need
shared UI scaffolding and DSP without violating copyleft requirements.
Licensing is per-file via SPDX headers: first-party UI and utility code is MIT;
the ReverbSC DSP family (Csound-derived) is LGPL-2.1-or-later. Both licenses
are compatible with GPLv3 consumers.

## Targets

| CMake target | Include path | Notes |
|---|---|---|
| `ghostmoon_oss::dsp` | `#include <ghostmoon/...>` | DSP — ReverbSC family. Mixed MIT (future utils) + LGPL-2.1-or-later. |
| `ghostmoon_oss::core` | `#include <ghostmoon/ui/Geometry.h>` / `<ghostmoon/ui/ScaledEditor.h>` | Skin-neutral UI scaffolding. MIT. `Geometry.h` is JUCE-free; `ScaledEditor.h` needs JUCE on the consumer's include path. |
| `ghostmoon_oss::ui_synthwave` | `#include <ghostmoon/ui/synthwave/...>` | Breadbin's neon synthwave skin (controls, chrome, scope). MIT. Depends on `ghostmoon_oss::core`. Requires JUCE. |

### Include layout

```
dsp/include/ghostmoon/
    ReverbSC.h          LGPL-2.1-or-later
    ReverbSCAdapter.h   LGPL-2.1-or-later
    ReverbSCX.h         LGPL-2.1-or-later

core/include/ghostmoon/ui/
    Geometry.h          MIT  (JUCE-free)
    ScaledEditor.h      MIT  (needs JUCE)
    Version.h           MIT

ui/include/ghostmoon/ui/synthwave/
    Theme.h             MIT
    Controls.h          MIT
    Chrome.h            MIT
    Scope.h             MIT
```

## Licensing

Every source file carries an SPDX-License-Identifier comment on its first line.
First-party UI and scaffolding code is MIT (`LICENSES/MIT.txt`).
The ReverbSC DSP family is LGPL-2.1-or-later (`LICENSES/LGPL-2.1.txt`),
derived from the Csound `reverbsc` opcode via Soundpipe and Poompatoom.
Both licenses are compatible with GPLv3 consumers — LGPL is explicitly
permitted to link into GPL works.

## Usage

```cmake
add_subdirectory(ghostmoon-oss)

# Link only the targets you need:
target_link_libraries(MyPlugin PRIVATE ghostmoon_oss::dsp)
target_link_libraries(MyPlugin PRIVATE ghostmoon_oss::core)
target_link_libraries(MyPlugin PRIVATE ghostmoon_oss::ui_synthwave)
```

JUCE must be on the consumer's include path when linking `ghostmoon_oss::core`
(for `ScaledEditor.h`) or `ghostmoon_oss::ui_synthwave`. `Geometry.h` has no
JUCE dependency and compiles without it.
