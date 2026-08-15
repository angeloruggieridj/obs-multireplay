# OBS MultiReplay

broadcast-style **multicamera instant replay** for OBS Studio — open source, in a native dock.

> 🇮🇹 Progetto di porting 1:1 di broadcast replay su OBS Studio. Stato della parità, funzione per funzione, in [docs/PARITA-REFERENCE.md](docs/PARITA-REFERENCE.md); architettura e convenzioni in [the architecture notes](the architecture notes).

Mark an action while it is happening, put it on air a couple of seconds later,
on any camera, at any speed — every angle on one timeline, on a laptop with an
integrated GPU, **without adding a single encoder** to what OBS is already
doing.

## How it works

Recording is done by [Branch Output](https://github.com/OPENSPHERE-Inc/branch-output)
(GPLv2, OPENSPHERE Inc.): one filter per camera, one Hybrid MP4 file each. This
plugin drives those filters — and then **taps the encoders Branch Output is
already running**, receiving their encoded packets live and keeping a bounded
history of them in RAM.

```
Branch Output filter → its encoder ─┬→ its muxer → file        (untouched)
                                    └→ our obs_output → packet ring (RAM)
                                                          ↓
                        "MultiReplay - Replay A" ← decoder ← ring / recorded files
```

Two consequences, and they are the whole design:

- **0 extra encoders, 0 extra disk writes.** libobs lets a second consumer join
  a running encoder — the same mechanism OBS uses to share one encoder between
  streaming and recording.
- **The timeline is measured, not estimated.** Every encoded packet carries
  `sys_dts_usec`, the same system clock for every encoder, so one marker means
  the same instant on every angle. Measured cross-angle skew **0 ms**, live edge
  **138 ms** behind real time (i7-10610U, Intel UHD/QSV, 1080p30, 2 cameras).

The replay is a real OBS input: put "MultiReplay - Replay A" in a scene and
transitions, the audio mixer and every output come for free.

## Using it

1. Install Branch Output and this plugin, then open **View ▸ Docks ▸
   MultiReplay**.
2. In ⚙ **Settings**: the session folder (an SSD) and which OBS source is which
   camera (up to 8).
3. Add **MultiReplay - Replay A** to the scene you cut to for replays.
4. **● REC** starts every camera together. Mark with `Mark / In / Out` or the
   `−5s / −10s / −20s` presets, tick the angles you want on each event, play
   them back at 5–200% — during the recording or after it.

Everything the panel does is also an OBS hotkey, so a Stream Deck runs the same
code as the buttons.

Before a take starts, a pre-flight check refuses what cannot work (no camera,
an unwritable folder, minutes of disk left, a disk too slow for the bitrate, no
RAM for the replay buffer) and says what is merely degraded. While it runs, a
badge next to REC reports a stalled or dead angle, frame drops or a filling
disk — and reports is all it does: nothing in that path can stop, switch or
restart anything on air.

## Requirements

- OBS Studio **32+**
- [Branch Output](https://github.com/OPENSPHERE-Inc/branch-output) ≥ 1.0.9
- A GPU with a hardware H.264 encoder (integrated is fine), SSD or better
- Windows is the supported platform today; macOS is best-effort, Linux is
  X11/XWayland only

## Building

Built on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
Step by step, all platforms: [BUILDING.md](BUILDING.md).

```sh
cmake --preset windows-x64 && cmake --build --preset windows-x64   # Windows
cmake --preset ubuntu-x86_64 && cmake --build --preset ubuntu-x86_64
cmake --preset macos && cmake --build --preset macos
```

Unit tests need neither OBS nor FFmpeg, so they run anywhere:
`ctest --test-dir build_x64 -C RelWithDebInfo`.

The end-to-end gate builds, installs, drives a real OBS with real sources and
writes a JSON verdict: `pwsh -File scripts/run-selftest.ps1 -Sources "C1,C2"`.
Add `-SoakMinutes 60` for the long-run check.

## Source map

| Path | What it is |
|---|---|
| `src/packet-tap.*` | Joins Branch Output's encoders and receives their packets |
| `src/master-timeline.hpp` | Per-encoder timestamps → one shared clock (`sys_dts_usec`) |
| `src/packet-ring.*` | Bounded live history per camera. Refuses a range it cannot serve; never clamps |
| `src/segment-index.*`, `src/segment-reader.*` | The recorded files, anchored onto that same clock |
| `src/replay-channel.*` | The "Replay A" OBS input: decodes and paces frames (slow motion is just wider spacing) |
| `src/event-store.*` | 20 event lists: in/out, angles, per-angle speed and notes |
| `src/playback-coordinator.*` | Queues events and angles, loop, music, play-to-output |
| `src/health-rules.hpp`, `src/health.*` | Pre-flight and runtime health — findings only, never actions |
| `src/multireplay-dock.*`, `src/qt-display.*` | The Qt dock and its previews |
| `src/selftest.cpp` | The scripted gate |

## License

GPL-2.0-or-later. Recording is powered by
[Branch Output](https://github.com/OPENSPHERE-Inc/branch-output) (GPLv2) by
OPENSPHERE Inc., used as an external plugin dependency.
