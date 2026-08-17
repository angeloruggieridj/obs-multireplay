<div align="center">

# OBS MultiReplay

**Multicamera instant replay for OBS Studio, in a native dock.**
**Instant replay multicamera per OBS Studio, in una dock nativa.**

[![Build](https://github.com/angeloruggieridj/obs-multireplay/actions/workflows/push.yaml/badge.svg)](https://github.com/angeloruggieridj/obs-multireplay/actions/workflows/push.yaml)
[![Latest release](https://img.shields.io/github/v/release/angeloruggieridj/obs-multireplay?include_prereleases&sort=semver)](https://github.com/angeloruggieridj/obs-multireplay/releases)
[![License: GPL v2](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)
[![OBS Studio](https://img.shields.io/badge/OBS%20Studio-32%2B-302E31?logo=obsstudio&logoColor=white)](https://obsproject.com/)
[![Platforms](https://img.shields.io/badge/platforms-Windows%20%C2%B7%20macOS%20%C2%B7%20Linux-lightgrey)](#installation)
[![Downloads](https://img.shields.io/github/downloads/angeloruggieridj/obs-multireplay/total)](https://github.com/angeloruggieridj/obs-multireplay/releases)

[English](#english) · [Italiano](#italiano)

</div>

---

## English

OBS MultiReplay turns OBS Studio into a replay desk. Every camera is recorded at
the same time on one shared timeline, so a single mark is the *same instant* on
every angle — press a key during the action, then play it back from any lens, at
any speed, forwards or backwards, while the match is still being recorded.

The whole thing lives in a native Qt dock inside OBS. There is no browser, no
web server and no second application to keep on screen.

### Why it is fast

The replay does **not** read the files being written. The live edge comes from
the encoded packets of the encoders OBS is *already* running for the recording,
kept in a bounded ring in RAM.

* **No extra encoders and no extra disk writes.** Nothing is encoded twice.
* **~140 ms from the action to the replay** on an Intel iGPU at 1080p30,
  measured, where reading a growing file costs about a second of fragment flush.
* **Zero skew between angles.** Every packet carries the same system clock, so
  the alignment between cameras is read, not estimated.
* **A range is served exactly or refused.** Nothing is ever clamped to
  "near enough", which is how a plausible-looking wrong replay reaches air.

### Features

**Recording**

* Up to **8 camera angles**, recorded simultaneously, one file series each.
* One **master timeline**: a mark means the same frame on every camera.
* **Pre-flight before REC** — free space expressed in *minutes of footage*,
  measured disk bandwidth against the requested bitrate, RAM for the ring,
  cameras actually present. It refuses what cannot work and says why.
* **Runtime health** while recording: per-angle packet flow, dropped frames,
  ring occupancy, disk. It reports; it never takes anything off air by itself.

**Marking**

* **In / Out**, and the `-5s` `-10s` `-20s` keys for the action that just
  happened.
* **Pre-roll and post-roll** applied at marking time, so the In you read in the
  list is the In that will play — and it stays editable.
* **Frame-accurate trim** of an event already marked, from buttons or hotkeys.
* **20 event lists**, renameable, with search.
* Per-angle **enable, speed and tag** in a single cell — the unit on screen is
  the unit in your head. Tags are your own vocabulary, importable and
  exportable as plain text.
* **Manual running order**, saved with the project: a highlights reel does not
  go out in the order the marks were taken.

**Playback**

* **Two replay bays, A and B** (B optional). Each is an ordinary OBS input you
  place in your own scenes, so transitions, the audio mixer and every output
  come for free. `A|B / A / B` says where the transport goes; the swap key
  exchanges what the two are playing.
* **Speed 5-200%**, slider and presets, applied *while the thumb moves* — slow
  motion is only wider spacing between frames, so nothing is re-encoded and the
  clip never restarts. The **audio is time-stretched and keeps its pitch**.
* **Pause holds the frame**, and playing again carries on from there.
* **Reverse playback** and **single-frame step in both directions**.
* **Position bar** over the whole recorded timeline — graduated, zoomable by
  time span (last minute, five, ten…), with draggable event markers. Stops
  between takes are joins, not gaps: every position on the bar is footage.
* **Multiview** of every configured camera, with preview/program tally.
* **Play to Program** with your own in and out transitions (stingers included),
  and a **cut or a dip between events**.
* **Music bed** under the replay, from a file or from one of your OBS sources.

**Export**

* **Frame-accurate MP4 per event**, by stream copy: exact on the In, no
  re-encode, seconds to write. Per-angle speeds are honoured.
* **Highlights reel in a single file** — your events, your order, your angles,
  with your music over them.
* **YouTube chapters** to the clipboard and to a file.

**Control surface**

* A **hotkey for everything**, visible in OBS's own Hotkeys settings and
  therefore mappable from a Stream Deck through OBS's native integration.
* A **keyboard layer** on the dock: arrows step a frame (a second with Shift),
  up/down walk the events, `+`/`-` change speed, Enter plays. Typing always
  wins — the search box and the tag cells are two pixels from these commands.

### Requirements

| | |
|---|---|
| OBS Studio | **32 or newer** |
| Branch Output | [OPENSPHERE-Inc/branch-output](https://github.com/OPENSPHERE-Inc/branch-output) — the recording layer |
| Platforms | Windows (primary) · macOS · Linux (X11/XWayland) |

### Installation

1. Install **Branch Output** first, and set its `Interlock` to `Always ON`.
2. Download the installer for your platform from the
   [releases page](https://github.com/angeloruggieridj/obs-multireplay/releases).
3. Restart OBS. The dock appears under **Docks ▸ MultiReplay**.
4. Open **⚙ ▸ Settings ▸ Cameras**, point each slot at one of your sources, and
   add **MultiReplay - Replay A** to a scene of yours.

On Windows, plugins are only scanned in `%ProgramData%\obs-studio\plugins` —
the installer puts it there.

### Building from source

```sh
git clone https://github.com/angeloruggieridj/obs-multireplay
cd obs-multireplay
cmake --preset windows-x64          # or macos / ubuntu-x86_64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

Dependencies are pinned in `buildspec.json` and **must match the OBS version**
you build against, or the module fails to load with a bare "module not found".

### Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).

The recording layer is provided by the Branch Output plugin (GPL-2.0). This
project clones the layout, arrangement and terminology of established broadcast
replay controllers; it contains none of their code, assets or branding.

---

## Italiano

OBS MultiReplay trasforma OBS Studio in una regia di replay. Tutte le camere
vengono registrate insieme su un'unica timeline, quindi una sola marcatura è lo
*stesso istante* su ogni angolo: premi un tasto durante l'azione e poi la rivedi
da qualsiasi obiettivo, a qualsiasi velocità, avanti o indietro, mentre la
partita è ancora in registrazione.

Tutto vive in una dock Qt nativa dentro OBS. Niente browser, niente server web,
nessuna seconda applicazione da tenere sullo schermo.

### Perché è veloce

Il replay **non** legge i file in scrittura. Il fronte live arriva dai pacchetti
già codificati dagli encoder che OBS sta **già** facendo girare per la
registrazione, tenuti in un ring limitato in RAM.

* **Zero encoder aggiuntivi e zero scritture su disco** nostre. Niente viene
  codificato due volte.
* **~140 ms dall'azione al replay** su iGPU Intel a 1080p30, misurati, contro
  circa un secondo di flush del frammento leggendo un file in crescita.
* **Zero scarto fra gli angoli.** Ogni pacchetto porta lo stesso orologio di
  sistema: l'allineamento fra camere si legge, non si stima.
* **Un intervallo si serve esatto o si rifiuta.** Non viene mai "avvicinato":
  è così che un replay sbagliato dall'aria plausibile finisce in onda.

### Funzionalità

**Registrazione**

* Fino a **8 angoli camera**, registrati insieme, una serie di file ciascuno.
* Una **master timeline**: una marcatura è lo stesso fotogramma su ogni camera.
* **Pre-flight prima del REC** — spazio libero espresso in *minuti di girato*,
  banda del disco **misurata** contro il bitrate richiesto, RAM per il ring,
  sorgenti camera effettivamente presenti. Rifiuta ciò che non può funzionare e
  dice perché.
* **Monitoraggio durante la take**: flusso pacchetti per angolo, frame persi,
  occupazione del ring, disco. Segnala; non toglie mai nulla dall'aria da solo.

**Marcatura**

* **In / Out** e i tasti `-5s` `-10s` `-20s` per l'azione appena successa.
* **Pre-roll e post-roll** applicati al momento della marcatura, così l'In che
  leggi in lista è quello che parte — e resta modificabile.
* **Trim al fotogramma** di un evento già marcato, da bottone o da hotkey.
* **20 liste eventi**, rinominabili, con ricerca.
* **Spunta, velocità e tag per singolo angolo in una sola cella**: l'unità sullo
  schermo è l'unità nella tua testa. I tag sono il tuo vocabolario, importabile
  ed esportabile in testo semplice.
* **Ordinamento manuale**, salvato col progetto: una reel di highlights non esce
  nell'ordine in cui hai preso le marcature.

**Riproduzione**

* **Due baie di replay, A e B** (la B è opzionale). Ognuna è un normale input
  OBS che metti nelle tue scene: transizioni, mixer audio e output vengono
  gratis. `A|B / A / B` dice dove va il trasporto; il tasto di scambio inverte
  ciò che le due stanno suonando.
* **Velocità 5-200%**, slider e preset, applicata **mentre muovi il pollice**:
  lo slow motion è solo una spaziatura più larga fra i fotogrammi, quindi non si
  ri-codifica niente e la clip non riparte mai da capo. **L'audio viene stirato
  e mantiene la tonalità.**
* **La pausa tiene il fotogramma**, e il play riprende da lì.
* **Riproduzione all'indietro** e **step di un fotogramma nelle due direzioni**.
* **Barra di posizione** su tutta la timeline registrata: graduata, con zoom per
  intervallo di tempo (ultimo minuto, cinque, dieci…) e marcatori evento
  trascinabili. Gli stop fra due take sono giunzioni, non buchi: ogni posizione
  sulla barra è girato vero.
* **Multiview** di tutte le camere configurate, con tally preview/program.
* **Messa in onda** con le tue transizioni di andata e ritorno (stinger
  compresi) e **stacco o dissolvenza tra un evento e l'altro**.
* **Musica** sotto il replay, da file o da una tua sorgente OBS.

**Esportazione**

* **MP4 per evento, al fotogramma**, in stream copy: esatto sull'In, nessuna
  ri-codifica, secondi per scriverlo. Le velocità per angolo vengono rispettate.
* **Reel di highlights in un unico file** — i tuoi eventi, il tuo ordine, i tuoi
  angoli, con la tua musica sopra.
* **Capitoli YouTube** negli appunti e su file.

**Superficie di controllo**

* Una **hotkey per ogni comando**, visibile nelle Scorciatoie di OBS e quindi
  mappabile da Stream Deck tramite l'integrazione nativa di OBS.
* Uno **strato tastiera** sulla dock: le frecce spostano di un fotogramma (di un
  secondo con Shift), su/giù scorrono gli eventi, `+`/`-` cambiano velocità,
  Invio riproduce. Scrivere vince sempre: la ricerca e le celle dei tag stanno a
  due pixel da questi comandi.

### Requisiti

| | |
|---|---|
| OBS Studio | **32 o successivo** |
| Branch Output | [OPENSPHERE-Inc/branch-output](https://github.com/OPENSPHERE-Inc/branch-output) — è lo strato di registrazione |
| Piattaforme | Windows (principale) · macOS · Linux (X11/XWayland) |

### Installazione

1. Installa prima **Branch Output** e metti il suo `Interlock` su `Always ON`.
2. Scarica l'installer per la tua piattaforma dalla
   [pagina delle release](https://github.com/angeloruggieridj/obs-multireplay/releases).
3. Riavvia OBS. La dock compare sotto **Dock ▸ MultiReplay**.
4. Apri **⚙ ▸ Impostazioni ▸ Telecamere**, punta ogni slot su una tua sorgente e
   aggiungi **MultiReplay - Replay A** a una tua scena.

Su Windows OBS scansiona i plugin solo in `%ProgramData%\obs-studio\plugins`:
l'installer li mette lì.

### Compilare dai sorgenti

```sh
git clone https://github.com/angeloruggieridj/obs-multireplay
cd obs-multireplay
cmake --preset windows-x64          # oppure macos / ubuntu-x86_64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

Le dipendenze sono pinnate in `buildspec.json` e **devono corrispondere alla
versione di OBS** con cui compili, altrimenti il modulo non si carica e dice
solo "module not found".

### Licenza

GPL-2.0-or-later. Vedi [LICENSE](LICENSE).

Lo strato di registrazione è fornito dal plugin Branch Output (GPL-2.0). Questo
progetto clona layout, disposizione e terminologia dei controller di replay
professionali affermati; non contiene nulla del loro codice, dei loro asset o
del loro marchio.
