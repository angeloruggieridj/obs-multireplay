---
name: fix-obs-deps-version
description: Fix "errore 126 module not found" al LoadLibrary del plugin dopo un bump della versione major di OBS (es. FFmpeg 7→8) — riallinea le dipendenze pinnate in buildspec.json e ricostruisce.
---

Le dipendenze pinnate in `buildspec.json` (obs-deps + Qt6) DEVONO seguire la
versione di OBS. OBS bumpa FFmpeg a major nuovi (es. OBS 32.2 →
`avcodec-62`/`avformat-62`/`avutil-60`/`swscale-9`/`swresample-6`, FFmpeg 8).
Se il plugin è linkato contro un obs-deps più vecchio (FFmpeg 7 = `avcodec-61`)
il `LoadLibrary` fallisce con **errore 126 "module not found"** (la dipendenza
FFmpeg manca, non la DLL plugin).

Fix: allinea `prebuilt`+`qt6` in `buildspec.json` alla versione obs-deps usata
da quella release OBS (presa da `CMakePresets.json` → vendor
`obsproject.com/obs-studio` → `dependencies` nel tag OBS; OBS non ha più
`buildspec.json` in root da 32.2), poi **wipe `build_x64`** (il CMakeCache
pinna i `*_DIR` Qt e i path FFmpeg vecchi: un reconfigure non li sovrascrive)
+ `cmake --preset windows-local` per ri-scaricare le deps +
`build-and-install.bat`.

Diagnosi rapida import DLL:
`[regex]::Matches([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll)),'av\w+-\d+\.dll')`.
