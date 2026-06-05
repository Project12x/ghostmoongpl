# GhostmoonGPL

The **copyleft** companion to Ghostmoon. It houses DSP that **cannot** be
MIT-licensed — primarily code derived from LGPL/GPL sources — so the main
[Ghostmoon](../ghostmoon) library stays cleanly permissive (MIT) for proprietary
plugins, while GPL/LGPL projects pull their copyleft components from here.

## License

**LGPL-2.1-or-later** (`SPDX-License-Identifier: LGPL-2.1-or-later`). Full text in
[LICENSE](LICENSE).

A GPL/LGPL project can use these components freely. A **proprietary / closed-source**
plugin may only use them under the LGPL's terms (dynamic linking, or static linking
with relinkable object files). If you need a clean reverb for a proprietary build,
use `gm::CustomFDNReverb` or `gm::PlateReverb` from the MIT Ghostmoon instead.

## Components

| Class | Header | Origin / license note |
|-------|--------|-----------------------|
| `gm::ReverbSC` | `ReverbSC.h` | Sean Costello 8-delay-line stereo FDN — "extracted from" the Csound `reverbsc` opcode (Costello & Varga, 1999/2005). **LGPL-2.1-or-later.** |
| `gm::ReverbSCAdapter` | `ReverbSCAdapter.h` | Single-knob decay wrapper around `gm::ReverbSC`. |
| `gm::ReverbSCX` | `ReverbSCX.h` | Extended ReverbSC: Costello core (LGPL) + original safety/character extensions. Combined work, LGPL-2.1-or-later. |

> **Why this repo exists:** the `ReverbSC` family was long carried as "MIT (via
> Soundpipe)" across projects. It is in fact **LGPL-2.1** — the Csound `reverbsc`
> opcode it derives from is LGPL, and Soundpipe's top-level MIT relicensing does
> not validly cover Csound-derived modules (a dispute documented on the Csound
> forum and around AudioKit's use of Soundpipe). Rather than delete the code, it
> lives here under its correct license for GPL/LGPL projects.

## Usage

Header-only, C++20, zero runtime dependencies.

```cmake
add_subdirectory(ghostmoongpl)
target_link_libraries(your_target PRIVATE ghostmoongpl)
```

```cpp
#include <ghostmoon/ReverbSCX.h>
gm::ReverbSCX rev;
rev.prepare(44100.0f);
```

The include path stays `ghostmoon/` and the C++ namespace stays `gm::`, so moving a
reverb from Ghostmoon to GhostmoonGPL changes only which library a project links —
not its `#include`s or type names.

## Tests

```
cmake -B build -DGHOSTMOONGPL_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

`tests/test_reverbscx.cpp` (GoogleTest) covers `ReverbSCX` plus baseline `ReverbSC`
drop-in-equivalence checks.
