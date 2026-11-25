# Roadmap

## Hotswap — separate plugin

Hot-swappable pieces are **not** planned for Suite Drums Glorp. That work will
live in a separate plugin:

- Forked from Suite Drums Glorp and refined as its own product.
- Kits built in.
- Preset configurations shipped with the plugin.
- Full mixer implemented.

Suite Drums Glorp stays the lean kit player/editor and will not grow the
hotswap feature.

## Suite Drum Factory — plan

Suite Drum Factory is the separate product. Naked Drums is the fixed base
(cymbals plus base shells); Crocell, DRS, Salamander and Monster are optional
shell modules. Pieces are enabled or disabled from a dropdown opened by a
down-triangle on the kit image, and user one-shots stay available. The MIDI
browser is reused.

### Module loading

- One shell module loaded at a time, filtered to the shell roles it provides.
- Shell-module cymbals are not loaded; Naked supplies all cymbals.
- Piece roles: Kick, Snare, Hihat, Tom 1–5, Ride, Crash 1/2, Splash 1/2,
  China 1/2. A role is owned by at most one active module, so MIDI notes and
  kit-view graphics never collide.

### Mixer — fixed bus set

The mixer is deliberately small: **one kick sum, one snare sum, one room sum
and the mix bus**. There is no arbitrary bus graph, no user-created buses and
no sends to free buses.

```
kick pieces   -> Kick sum  --\
snare pieces  -> Snare sum ----> Mix bus -> output
room/OH mics  -> Room sum  --/
toms, cymbals, hats, one-shots -----------/
```

- **Kick sum**: close mics of the active kick piece.
- **Snare sum**: close mics of the active snare piece.
- **Room sum**: room, overhead and ambient mics from the base and shell
  modules; usually carries the bus reverb.
- **Mix bus**: toms, cymbals, hi-hat and user one-shots go here directly,
  together with the three sums. Master inserts and metering live here.
- Every sum and channel has a serial insert chain: graphical EQ (Common Sense
  EQ), compressor, drive, transient shaper, limiter, clipper. Slots are
  bypassable and reorderable.
- Solo/mute, gain, pan, meters and insert state are saved per preset.

### Presets

A versioned `.factorypreset` stores module selection, enabled pieces, user
one-shots, MIDI map, Kick/Snare/Room/Mix insert chains, and channel
gain/pan/mute/solo. Module IDs stay stable across versions.

### Licensing

Naked, Crocell and DRS are CC BY 4.0; Salamander and Monster are CC BY-SA 3.0.
Samples may ship with attribution, but the BY-SA portions require the sample
derivatives to remain BY-SA. Code can stay separately licensed; the installer
and About must include the full notices. Optional shell packs with their own
attribution are the simpler commercial route.

### Milestones

1. Prototype: Naked base plus one shell module, piece dropdown and toggles;
   measure CPU and memory.
2. Implement the Kick/Snare/Room/Mix graph and serial insert slots.
3. Build the preset format and preset UI.
4. Fork only after the prototype and mixer design are accepted.
