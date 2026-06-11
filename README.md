# OBS MultiReplay

broadcast-style **multicamera instant replay** for OBS Studio — open source, cross-platform, browser-controlled.

> 🇮🇹 Progetto di porting 1:1 di broadcast replay su OBS Studio. Documento di progetto completo in [docs/DOCUMENTO-DI-PROGETTO.md](docs/DOCUMENTO-DI-PROGETTO.md).

## Status: Milestone 1 (PoC recorder) — work in progress

| Milestone | Scope | Status |
|---|---|---|
| **M1 — Recorder** | Continuous 4-camera recording via [Branch Output](https://github.com/OPENSPHERE-Inc/branch-output), 20-min splits, session folder, web UI with recording control | ✅ implemented (untested in real OBS) |
| **M2 — Playback** | "Replay A"/"Replay B" OBS sources, synced master timeline across angles, speed 0–100%, reverse, frame-step, position bar, A/B linking | ✅ implemented (untested in real OBS) |
| **M3 — Events + multiview** | 20 event lists, mark in/out + −5/−10/−20 presets, Live/Recorded modes, per-event angles/notes/speed, play last/selected **to output** (auto scene switch & return), MJPEG multiview (4 cams + A/B) in the browser | ✅ implemented (untested in real OBS) |
| **M4 — Export, loop, music, shortcuts** | MP4 clip export (stream-copy, instant, works while recording), Loop, music source unmute during playback, Delete All, keyboard shortcuts | ✅ implemented (untested in real OBS) |
| **M5 — Streaming previews, audio, 8 cams, controllers** | Continuous push video previews (per-tile MJPEG streams on a dedicated port, automatic polling fallback), audio in replay playback (forward @100%), 8 cameras end-to-end, Web MIDI + WebHID ShuttlePro (experimental) | ✅ implemented (untested in real OBS) |
| M6 — Polish | WebRTC/WHIP previews (H.264, requires libdatachannel), overlap transitions between events (dual-decoder compositing) | ⏳ |

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

M2 plays back **completed** segments (after a stop or a 20-min split).

### M3 usage

- The **multiview** at the bottom shows live MJPEG previews of the 4 cameras
  plus the Replay A/B channels; click a camera tile to assign that angle to
  the controlled channel.
- **LIVE** (red) marks events "as they happen" while recording; switch it off
  for Recorded mode, where marks use the position-bar time.
- **Mark In/Out**, **−5/−10/−20** create events in the selected list (tabs
  1–20); checkboxes pick the angles, double-click a note to edit it, click the
  speed cell for a per-event speed ("--" inherits, the reference controller semantics).
- **↺** plays the last event straight to program: set *Output scene* in the
  settings (e.g. a scene containing Replay A), OBS switches to it and back
  automatically when the event ends. **Play Events** does the same for the
  selected events back-to-back.

### M4 usage

- **Export Clips** exports the selected events (or the last event) as MP4 into
  `<session>/export/` — stream-copy, no re-encode, works while recording. The
  clip starts at the keyframe at/before the In point (≤1s pre-roll, 1s GOP).
  Limitation: an event must not span a 20-minute file split.
- **Loop** repeats the playing selection; **🎵** unmutes the *Music source*
  configured in settings during event playback and mutes it after.
- **Delete All** (settings) wipes recordings + events, keeps configuration —
  the reference controller behaviour for reusing the same session per game.
- Keyboard shortcuts: `Space` play/pause · `←/→` frame step (`Shift` = ±10) ·
  `I/O` mark in/out · `5/6/7` = −5/−10/−20 · `1–4` angle · `P` play last ·
  `D` direction · `N` jump to now · `L` Live/Recorded.

### M5 usage

- **Previews are now continuous video**: each tile is a persistent MJPEG
  stream served on **port 8457** (REST port + 1) at 12 fps. Allow that port
  in the firewall for LAN clients; if unreachable, tiles fall back to
  snapshot polling automatically.
- **Audio plays in replays** at 100% forward speed (slow motion / reverse
  are mute, like a stadium replay).
- **8 cameras**: all selectors, tiles, angle buttons and event columns go
  up to 8. Keyboard: digits `1-8` = angle; `Z/X/C` = −5/−10/−20 presets.
- **🎛 Shuttle** (settings) connects a Contour ShuttlePro v2/Xpress via
  WebHID (Chrome/Edge): jog = frame step, shuttle ring = speed+direction,
  buttons 1-8 = angles, 9/10 = mark in/out, 11 = play last, 12 = play/pause,
  13 = jump to now. Experimental — untested on real hardware.

Deferred to M6 (heavy infrastructure, documented rationale): WebRTC/WHIP
previews (needs libdatachannel + DTLS vendoring), overlap transitions
between events (needs dual-decoder compositing in the replay source).

## License

GPL-2.0-or-later. Recording is powered by [Branch Output](https://github.com/OPENSPHERE-Inc/branch-output)
(GPLv2) by OPENSPHERE Inc., used as an external plugin dependency in M1.
Web server: [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT, vendored).
