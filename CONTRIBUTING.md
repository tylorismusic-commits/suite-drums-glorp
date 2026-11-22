# Contributing to Suite Drums Glorp

Thanks for helping improve Suite Drums Glorp. This fork focuses on the drum
instrument: kit loading, kit view, mixer/stems, channel FX, e-drum input,
bleed matrix and the `.glorpkit` editor. Practice, recording, streaming and
standalone-host features are intentionally out of scope.

## Development setup

```bash
git clone https://github.com/tylorismusic-commits/suite-drums-glorp.git
cd suite-drums-glorp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DGLORP_BUILD_VST3=ON -DGLORP_BUILD_LV2=ON \
  -DGLORP_BUILD_KIT_EDITOR=ON -DGLORP_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

JUCE 8.0.6 is fetched automatically when `libs/JUCE` is absent.

## Project layout

| Path | Purpose |
| :--- | :--- |
| `src/core/` | Drum kit parsing, sampler, GlorpKit format, MIDI map, bleed matrix, settings |
| `src/plugin/` | JUCE processor/editor wrapper, parameter manager, audio engine |
| `src/ui/` | Suite theme, kit view, mixer, FX, knobs, scope, e-drum wizard |
| `src/editor_app/` | Suite Kit Studio standalone `.glorpkit` editor |
| `test/` | Headless core tests, UI snapshot tests and demo kit |
| `cmake/` | Install rules |
| `tools/` | Packaging and local install scripts |

## Pull requests

- Keep changes focused and consistent with the existing C++20/JUCE style.
- Run `ctest --test-dir build --output-on-failure` before opening a PR.
- If you change the UI, regenerate the snapshots with `test_editor` and update
  `docs/`.
- Do not add practice, recording, setlist, backing-track, metronome, streaming
  or standalone-host features to this fork.
- New bundled assets must have a clear license compatible with AGPL-3.0
  distribution; add attribution to `NOTICE.md`.

## Reporting bugs

Open an issue at
https://github.com/tylorismusic-commits/suite-drums-glorp/issues with:

- Your distribution, DAW/host and plugin format (VST3/LV2).
- Steps to reproduce and the kit used.
- The diagnostic report from `SETTINGS > Open Debug Log`.

For security issues, see `SECURITY.md`.
