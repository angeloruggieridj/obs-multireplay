# Verifying a MultiReplay download

> The releases are not code-signed. This page explains what that means on each
> platform, and the three independent ways to check a download for yourself.
> Back to the [README](../README.md).

MultiReplay is a free, one-person project, and code-signing certificates are
neither free nor issued to projects: an Apple Developer membership is a yearly
fee, and a Windows OV/EV certificate costs more. So the released packages carry
**no publisher signature on any platform**. That is why your system treats them
as untrusted, and what you have to do about it:

| Platform | What you'll see | What to do |
|---|---|---|
| **macOS** | Gatekeeper quarantines anything downloaded from a browser. Double-clicking the `.pkg` gives *"cannot be opened because it is from an unidentified developer"*, and if OBS ends up loading a quarantined plugin bundle it usually fails silently — the dock never appears under *Docks*. | Right-click the `.pkg` → **Open** → **Open** again in the dialog; or clear the quarantine first with `xattr -dr com.apple.quarantine obs-multireplay-1.0.0-macos-universal.pkg`, then run it. If the plugin still will not load, clear it on the installed bundle too: `xattr -dr com.apple.quarantine "/Library/Application Support/obs-studio/plugins/obs-multireplay.plugin"`. |
| **Windows** | The downloaded `.zip` is marked as coming from the internet; SmartScreen may warn, and some antivirus products flag unsigned DLLs on sight. | Right-click the zip → **Properties** → tick **Unblock**, then extract into `%ProgramData%\obs-studio\plugins`. If your antivirus quarantines the DLL, verify it first (below) and add an exclusion. |
| **Linux** | Nothing. There is no signature check to fail. | `sudo apt install ./obs-multireplay-1.0.0-x86_64-linux-gnu.deb`. |

"Unsigned" means nobody paid to vouch for the file — it does not mean the file
is unverified. Every release is built in public by
[GitHub Actions](../.github/workflows/push.yaml) from the tagged source, and
each one ships evidence you can check yourself:

**1. Integrity.** Every release publishes a SHA-256 for each asset, in the
`### Checksums` block at the bottom of the release notes (GitHub also exposes a
per-asset digest through its API). Hash your download and compare:

```bash
# macOS / Linux
shasum -a 256 obs-multireplay-1.0.0-windows-x64.zip
```
```powershell
# Windows
(Get-FileHash obs-multireplay-1.0.0-windows-x64.zip -Algorithm SHA256).Hash
```
```bash
# what GitHub says it should be
gh release view 1.0.0-beta11 --repo angeloruggieridj/obs-multireplay --json assets \
  --jq '.assets[] | .name + "  " + .digest'
```

The **in-app updater already does this check automatically** before it installs
anything — it parses the same `### Checksums` block and refuses a download whose
hash does not match. The commands above are for a package you downloaded by hand
from the releases page.

This catches a truncated or corrupted download. It is not evidence of who built
the file — the digest and the file come from the same place — which is what the
next step is for.

**2. Build provenance.** Each package is published with a signed
[GitHub attestation](https://docs.github.com/actions/security-guides/using-artifact-attestations)
binding it to the exact workflow run, commit and runner that produced it — proof
it came out of this repository's CI and not off someone's laptop. This is the
guarantee that actually replaces a publisher signature. Verify it with the
[GitHub CLI](https://cli.github.com/):

```bash
gh attestation verify obs-multireplay-1.0.0-windows-x64.zip --repo angeloruggieridj/obs-multireplay
```

**3. Malware scan.** When a VirusTotal API key is configured for the repository,
each release is scanned across ~70 antivirus engines and the report links are
added to the release page under **Verification**. You can also upload any file to
[virustotal.com](https://www.virustotal.com/gui/home/upload) yourself — a handful
of engines flagging an unsigned DLL is a common false positive, which is exactly
why the provenance attestation matters more than a scan.

> There is no VirusTotal badge anywhere in this project: VirusTotal has no live
> badge endpoint, and a static one would only say the packages **are** scanned,
> not that any particular scan came back clean. The per-file reports linked from
> each release are the actual result, and they are what you should read.

> Provenance attestations are produced from **`1.0.0-beta12` onwards** — the
> first release built after the workflow gained the attestation step. Earlier
> releases have GitHub's asset digests but no attestation.

Finally, nothing here is a black box: the plugin is GPL-2.0-or-later, the full
source is in this repository, and you can always
[build it yourself](../README.md#building-from-source).
