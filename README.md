# OBS MultiReplay

broadcast-style **multicamera instant replay** for OBS Studio — open source, cross-platform, browser-controlled.

> 🇮🇹 Progetto di porting 1:1 di broadcast replay su OBS Studio. Documento di progetto completo in [docs/DOCUMENTO-DI-PROGETTO.md](docs/DOCUMENTO-DI-PROGETTO.md).

## Status: Milestone 1 (PoC recorder) — work in progress

| Milestone | Scope | Status |
|---|---|---|
| **M1 — Recorder** | Continuous 4-camera recording via [Branch Output](https://github.com/OPENSPHERE-Inc/branch-output), 20-min splits, session folder, web UI with recording control | ✅ implemented (untested in real OBS) |
| **M2 — Playback** | "Replay A"/"Replay B" OBS sources, synced master timeline across angles, speed 0–100%, reverse, frame-step, position bar, A/B linking | ✅ implemented (untested in real OBS) |
| M3 — Events + full UI | 20 event lists, mark in/out, −5/−10/−20, Live/Recorded modes, multiview | ⏳ |
| M4 — Full parity | Highlight reels, music, MP4 export, WebHID/MIDI controllers, 8 cameras | ⏳ |

## How it works (M1)

1. Install [Branch Output](https://obsproject.com/forum/resources/branch-output-streaming-recording-filter-for-source-scene.1987/) (the recording engine) and this plugin.
2. Open `http://<obs-pc-ip>:8456` from any browser on your LAN (or the same PC).
3. Pick up to 4 OBS sources as cameras, set the session folder (SSD), hit **●**.
4. The plugin creates and drives one Branch Output filter per camera: Hybrid MP4,
   automatic 20-minute splits, hardware encoder auto-detection (NVENC → QSV → AMF →
   VAAPI → VideoToolbox → x264), per-camera start timestamps saved to `session.json`
   for the synced timeline of M2.

Hotkeys `ReplayStartRecording` / `ReplayStopRecording` are available in OBS Settings → Hotkeys.

## Requirements

- OBS Studio ≥ 31 at build time; **OBS 32+ recommended/target** (Hybrid MP4 default, Plugin Manager)
- [Branch Output](https://github.com/OPENSPHERE-Inc/branch-output) ≥ 1.0.9
- Any GPU with a hardware H.264 encoder (integrated GPUs fine), SATA SSD or better

## Building

Built on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
**Step-by-step guide for all platforms (incl. install paths): [BUILDING.md](BUILDING.md).** Quick start:

```sh
# Linux
cmake --preset ubuntu-x86_64 && cmake --build --preset ubuntu-x86_64
# Windows
cmake --preset windows-x64 && cmake --build --preset windows-x64
# macOS
cmake --preset macos && cmake --build --preset macos
```

## Architecture

```
Browser (LAN) ⇄ embedded HTTP server (cpp-httplib) ⇄ ReplayCore ⇄ Branch Output filters ⇄ disk
```

- `src/replay-core.*` — session, config, camera/recording state
- `src/branch-output-control.*` — programmatic driver for `osi_branch_output` filters
- `src/session-index.*` — master timeline: maps a single time cursor onto every camera's segment files (offsets from `session.json`, durations probed via libavformat)
- `src/decoder.*` — FFmpeg segment decoder (seek to 1s-GOP keyframe → decode to exact frame → I420)
- `src/replay-player.*` — playback engine per channel: play/pause, speed 0–100%, reverse (GOP cache), frame-step, angle switching, A|B linking
- `src/replay-source.cpp` — the "MultiReplay — Replay A/B" sources to add to your replay scene
- `src/web-server.*` — REST API + static UI (`data/ui/`)
- `docs/` — full project document (Italian)

### M2 usage

1. Record something (M1), then click **⟳ Load session** in the web UI
   (or `POST /api/session/load`).
2. Add the sources **MultiReplay — Replay A/B** to an OBS scene.
3. Use the position bar to scrub all angles in sync; switch angles with the
   A/B `1 2 3 4` buttons; slow motion with the presets/slider; `◀` toggles
   reverse; `‹ ›` step a single frame.

M2 plays back **completed** segments (after a stop or a 20-min split). Audio
playback and event-based playback arrive with M3.

## License

GPL-2.0-or-later. Recording is powered by [Branch Output](https://github.com/OPENSPHERE-Inc/branch-output)
(GPLv2) by OPENSPHERE Inc., used as an external plugin dependency in M1.
Web server: [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT, vendored).
