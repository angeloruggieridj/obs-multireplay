/*
obs-multireplay — checking for, fetching and staging plugin updates
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

WHAT THIS DOES, AND THE ONE THING IT DELIBERATELY DOES NOT.

It asks GitHub which releases exist, decides whether any of them is newer than
the running build (version-compare.hpp, which is pure and unit tested), shows
the operator what changed, and downloads the archive. Then it STAGES it and
asks.

It does not swap the plugin under a running OBS, and that is not a limitation to
be lifted later. On Windows the DLL is loaded and locked; more to the point this
is a tool people record matches with, and an updater that replaces the recording
path while the recording path is in use has picked the wrong thing to be clever
about. The install runs after OBS has exited, from a helper that waits for it.

THREADS. Every network call runs on a thread of its own and nothing on the UI
thread ever waits for one. The panel reads status() — a snapshot behind a mutex
that is never held across a socket — so a check against a server that has gone
quiet costs a spinner, not a frozen OBS. Same rule as the packet tap and the
playback coordinator, for the same reason.

HTTPS is libcurl, which obs-deps ships and OBS itself already loads. NOT Qt
Network: this OBS build logs "No functional TLS backend was found" eleven times
at startup, so a Qt HTTPS request would fail on exactly the machine we have.
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace multireplay {

// Which releases the operator is willing to be offered.
enum class UpdateChannel {
	Stable, // final releases only
	Beta,   // pre-releases too
};

const char *updateChannelToString(UpdateChannel c);
UpdateChannel updateChannelFromString(const std::string &s);

// One release, as GitHub describes it.
struct ReleaseInfo {
	std::string version;   // the tag, e.g. "1.0.0-beta2"
	std::string name;      // the release title
	std::string notes;     // the body: what changed
	std::string assetUrl;  // where the archive is
	std::string assetName; // what it is called
	int64_t assetBytes = 0;
	bool prerelease = false;
	std::string publishedAt;

	bool valid() const { return !version.empty() && !assetUrl.empty(); }
};

class Updater {
public:
	static Updater &instance();

	enum class Phase {
		Idle,        // nothing asked for yet
		Checking,    // talking to GitHub
		UpToDate,    // asked, and this build is the newest
		Available,   // asked, and there is a newer one
		Downloading, // fetching the archive
		Staged,      // downloaded and verified; waiting to be installed
		Failed,      // and `message` says why
	};

	struct Status {
		Phase phase = Phase::Idle;
		ReleaseInfo release;    // meaningful from Available onwards
		int percent = 0;        // Downloading only
		std::string message;    // the reason, when there is one
		std::string stagedPath; // the archive on disk, when Staged
	};

	// A snapshot. Safe from any thread, and the mutex behind it is never held
	// across anything that can block.
	Status status() const;

	// The version this build IS. Comes from the same place the module reports
	// to OBS, so the panel and the log can never disagree.
	static std::string currentVersion();

	// Can this build INSTALL what it downloads, or only hand the file over?
	// Windows has a helper; the other two do not, and pretending otherwise is
	// how an operator ended up with a Windows zip downloaded on a Mac and an
	// "Install" button that quietly returned false. The panel asks first.
	static bool canInstallHere();

	// Ask GitHub. Returns immediately; watch status(). A second call while one
	// is in flight is ignored rather than queued.
	void checkAsync(UpdateChannel channel);

	// Fetch the archive of the release status() is offering. Same contract.
	void downloadAsync();

	// Hand the staged archive to a helper that waits for OBS to exit, unpacks
	// it over the installed plugin and starts OBS again. False (and a reason)
	// when there is nothing staged or the helper cannot be written.
	//
	// It does NOT close OBS. Asking an operator's recording software to quit
	// is his decision; the helper waits for him to make it.
	bool installStaged(std::string &errorOut);

	// Stop waiting on whatever is in flight. Used at unload: a thread still
	// joinable when this object dies is std::terminate.
	void shutdown();

private:
	Updater() = default;
	~Updater();
	Updater(const Updater &) = delete;
	Updater &operator=(const Updater &) = delete;

	void setStatus(const Status &s);
	void joinWorker();

	mutable std::mutex mutex_;
	Status status_;
	std::thread worker_;
	std::atomic<bool> busy_{false};
	std::atomic<bool> abort_{false};
};

} // namespace multireplay
