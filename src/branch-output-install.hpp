/*
obs-multireplay — fetching and running the Branch Output installer
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

WHY THIS EXISTS AT ALL.

This plugin cannot record a single frame without Branch Output: every camera's
recording is a Branch Output filter, and the packet tap attaches to encoders
Branch Output is already running. On a machine where it is missing, MultiReplay
is a panel that looks complete and refuses at REC — and until now the only thing
that said so was a line in a log file nobody opens before a match.

So the panel asks, on every launch, until it is there. And because "go and find
the right file for your platform" is the step where an operator gives up, it
offers to do it.

WHAT IT DELIBERATELY DOES NOT DO: place a single byte itself. Branch Output
publishes, for each platform, exactly the file that platform knows how to
install — a SIGNED installer on Windows, a .pkg on macOS, a .deb on Linux — and
handing that to the system beats anything we could do on all three counts that
matter. It asks for elevation the way the OS wants to be asked (the OBS plugin
folder is not writable by a normal user on a machine where OBS was installed by
an administrator, which is every fresh machine), it knows where that folder is,
and it is signed by the people who built it. Unpacking an archive into place
ourselves would fake all three, badly.

WHAT IT VERIFIES. GitHub's release API reports a `digest` for every asset. The
download is hashed and compared against it before anything is handed over, with
the same rule the updater already follows: an asset that publishes no digest we
can read is REFUSED rather than run. This is somebody else's installer being
started on the operator's machine, and "probably fine" is not a policy.

THREADS. One worker, owned, joined in shutdown() — which obs_module_unload
calls, in the declared order, for the reason written in replay-channel.cpp.
Nothing on the UI thread ever waits for the network.
*/

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace multireplay {

// Where Branch Output lives. And https, because this decides what executable
// the operator ends up opening.
inline constexpr const char *kBranchOutputReleasesPage =
	"https://github.com/OPENSPHERE-Inc/branch-output/releases/latest";

class BranchOutputInstall {
public:
	static BranchOutputInstall &instance();

	enum class Phase {
		Idle,        // nothing asked for yet
		Asking,      // talking to GitHub about the latest release
		Downloading, // fetching the installer for THIS platform
		Verifying,   // hashing it against the digest GitHub published
		Ready,       // verified and on disk; `filePath` is what to open
		Failed,      // and `message` says why
	};

	struct Status {
		Phase phase = Phase::Idle;
		std::string version;   // the tag, once known
		std::string assetName; // what is being fetched
		std::string filePath;  // UTF-8; meaningful from Ready onwards
		int percent = 0;       // Downloading only
		std::string message;   // the reason, when there is one
	};

	// WHO OPENS THE FILE, AND WHY IT IS NOT THIS CLASS. Handing a downloaded
	// installer to the desktop is one call — QDesktopServices::openUrl —
	// which does the right, different thing on each of the three platforms
	// (ShellExecute on Windows, so the signed installer's own manifest asks
	// for elevation; Installer.app for a .pkg; xdg-open for a .deb) and must
	// run on the UI thread. Keeping it in the panel leaves this file free of
	// Qt and free of a single #ifdef, and leaves the panel able to say what
	// happened when the desktop has nothing that opens a .deb.

	// A snapshot. Safe from any thread; the mutex behind it is never held
	// across a socket.
	Status status() const;

	// Go. Returns immediately; watch status(). A second call while one is in
	// flight is ignored rather than queued.
	void startAsync();

	// Stop waiting on whatever is in flight. A thread still joinable when
	// this object dies is std::terminate.
	void shutdown();

private:
	BranchOutputInstall() = default;
	~BranchOutputInstall();
	BranchOutputInstall(const BranchOutputInstall &) = delete;
	BranchOutputInstall &operator=(const BranchOutputInstall &) = delete;

	void setStatus(const Status &s);
	void setFailed(const std::string &why);
	void joinWorker();
	void run();

	mutable std::mutex mutex_;
	Status status_;
	std::thread worker_;
	std::atomic<bool> busy_{false};
	std::atomic<bool> abort_{false};
};

} // namespace multireplay
