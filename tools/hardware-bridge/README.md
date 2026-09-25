# Hardware controller bridges

[English](#english) · [Italiano](#italiano)

## English

Small Node.js programs that drive obs-multireplay from a USB replay
controller, through obs-websocket. The plugin itself contains no USB code:
each bridge reads its device and sends OBS hotkeys and `multireplay` vendor
requests.

```
controller --USB--> bridge (Node.js) --obs-websocket--> OBS + obs-multireplay
```

Contributed by Thomas Probst.

### Supported controllers

| Script | Controller | Status |
|---|---|---|
| `jlcooper-slomo-mini.js` | JLCooper SloMo Mini (raw mode, FTDI) | Full key map, jog, T-bar, display |
| `shuttlepro-v2.js` | Contour ShuttlePRO v2 (HID) | Jog and shuttle; keys 14/15 mapped, the rest are yours |

### Setup

1. In OBS: **Tools ▸ WebSocket Server Settings**, enable the server and note
   the port and password.
2. Quit the controller's own software: it claims the device exclusively.
3. In this folder:

```bash
npm install
OBS_WS_PASSWORD=yourpassword npm run slomo-mini   # or: npm run shuttlepro
```

On Windows PowerShell: `$env:OBS_WS_PASSWORD='yourpassword'; npm run slomo-mini`.
`OBS_WS_URL` (default `ws://127.0.0.1:4455`) points the bridge at another machine.

Jog ticks are turned into seconds at 25 fps: set `FPS` at the top of the
script to your project's rate.

### JLCooper SloMo Mini key map

| Key | Action |
|---|---|
| `MARK IN` / `MARK OUT` | Mark IN / OUT |
| `W1` / `W2` | Mark the last 5 s / 10 s |
| `PLAY` | Play the last event to output |
| `STOP` | Stop |
| `REVERSE PLAY` | Play backwards |
| `F-FWD` | Back to the live edge |
| `FRAME -1` / `FRAME +1` | One frame back / forward |
| `CAM A`…`CAM D` | Camera 1–4 (with `SHIFT`: 5–8) |
| `CYCLE PLAYER` | Channel A ↔ B (needs the second bay switched on) |
| `REPLAY` | REC on/off |
| `LAST CUE` / `NEXT CUE` | Previous / next event |
| `STORE CUE` | Next list (with `SHIFT`: previous); stops at the first and last shown list |
| digits + `ENTER` | Jump to the event with that id; `CLR` clears the digits |
| T-bar | Replay speed, 5 % … 100 % |
| Jog wheel | Scrub along the recorded footage |

The display shows the position on the panel's seek bar on line 1 and
`event · list · speed` on line 2, refreshed every 200 ms.

### Vendor requests

Vendor name `multireplay`, sent with obs-websocket's `CallVendorRequest`.
Every answer carries `success`, and `error` when it is `false`.

| Request | Data | Does |
|---|---|---|
| `step_frames` | `{"delta": int}` | Frame steps, positive forward (max 10 per request) |
| `set_speed` | `{"percent": int}` | Replay speed, 5–200 |
| `scrub_seconds` | `{"seconds": number}` | One jump along the footage |
| `step_event_selection` | `{"delta": int}` | Previous / next event (selecting cues it) |
| `select_event_by_id` | `{"id": int}` | Select and cue that event |
| `toggle_active_channel` | `{}` | Channel A ↔ B |
| `step_list_selection` | `{"delta": int}` | Previous / next list; answers `activeList` |
| `get_playback_status` | `{}` | `cursorMs`, `speedPercent`, `eventId`, `activeList`, `recording`, `channel` |

`cursorMs` is the position on the seek bar in milliseconds of recorded
footage, or `-1` when there is no timeline yet.

Everything else is an OBS hotkey (`TriggerHotkeyByName`); the names are in
OBS under **Settings ▸ Hotkeys**.

## Italiano

Piccoli programmi Node.js che comandano obs-multireplay da un controller di
replay USB, attraverso obs-websocket. Il plugin non contiene codice USB: ogni
bridge legge il suo dispositivo e invia hotkey OBS e richieste vendor
`multireplay`.

Contributo di Thomas Probst.

### Controller supportati

| Script | Controller | Stato |
|---|---|---|
| `jlcooper-slomo-mini.js` | JLCooper SloMo Mini (raw mode, FTDI) | Tasti, jog, T-bar e display |
| `shuttlepro-v2.js` | Contour ShuttlePRO v2 (HID) | Jog e shuttle; tasti 14/15 assegnati, il resto a scelta |

### Installazione

1. In OBS: **Strumenti ▸ Impostazioni server WebSocket**, attiva il server e
   annota porta e password.
2. Chiudi il software del controller: tiene il dispositivo in esclusiva.
3. In questa cartella:

```powershell
npm install
$env:OBS_WS_PASSWORD='latuapassword'; npm run slomo-mini   # oppure: npm run shuttlepro
```

`OBS_WS_URL` (predefinito `ws://127.0.0.1:4455`) punta il bridge a un'altra
macchina. Il jog converte i passi in secondi a 25 fps: imposta `FPS` in cima
allo script sul frame rate del progetto.

La mappa dei tasti del SloMo Mini e l'elenco delle richieste vendor sono nella
sezione inglese qui sopra: i nomi dei tasti e delle richieste sono gli stessi.
