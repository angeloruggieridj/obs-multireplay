/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

See selftest.hpp. This is the scripted form of the M0 gate.
*/

#include <obs-module.h> // MUST precede plugin-support.h (MSVC C2375)
#include <obs-frontend-api.h>

#include "selftest.hpp"

#include "branch-output-control.hpp"
#include "event-store.hpp"
#include "export.hpp"

// The gate READS BACK the reel it exported: "a file appeared" is not the claim
// worth checking (see the reel check), so it demuxes the result.
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
#include "health.hpp"
#include "multireplay-dock.hpp"
#include "dock-icons.hpp"
#include "packet-tap.hpp"
// pathToUtf8: a path handed to FFmpeg is UTF-8, never path::string() (which is
// the ANSI code page on MSVC). See the rule in CLAUDE.md.
#include "path-utf8.hpp"
#include "playback-coordinator.hpp"
#include "plugin-support.h"
#include "replay-channel.hpp"
#include "replay-core.hpp"
#include "qt-display.hpp"
#include "replay-decoder.hpp"
#include "segment-index.hpp"

#include <util/base.h>
#include <util/platform.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QMouseEvent>
#include <QRect>
#include <QScreen>
#include <QObject>
#include <QItemSelectionModel>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QFontMetrics>
#include <QTabBar>
#include <QTableWidget>
#include <QCoreApplication>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace multireplay {

namespace {

std::atomic<bool> g_started{false};

// The project the gate works in. A fixed name on purpose: the reopen pass has
// to find the same one in the next OBS process, and it is deleted at the end of
// that pass so the whole gate can simply be run again.
constexpr const char *kSelfTestProject = "MRSelfTest";

std::string envStr(const char *key, const std::string &def = {})
{
	const char *v = getenv(key);
	return (v && *v) ? std::string(v) : def;
}

bool envOn(const char *key)
{
	const char *v = getenv(key);
	return v && *v && strcmp(v, "0") != 0;
}

int envInt(const char *key, int def)
{
	const std::string s = envStr(key);
	if (s.empty())
		return def;
	try {
		return std::stoi(s);
	} catch (...) {
		return def;
	}
}

// Split "A,B,C" into names, trimming surrounding blanks.
std::vector<std::string> splitCsv(const std::string &s)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= s.size() && !s.empty()) {
		size_t comma = s.find(',', start);
		if (comma == std::string::npos)
			comma = s.size();
		std::string item = s.substr(start, comma - start);
		size_t b = item.find_first_not_of(" \t");
		size_t e = item.find_last_not_of(" \t");
		if (b != std::string::npos)
			out.push_back(item.substr(b, e - b + 1));
		if (comma == s.size())
			break;
		start = comma + 1;
	}
	return out;
}

// Run `fn` on the OBS UI thread and wait for it.
//
// This matters more than it looks: Branch Output evaluates its start conditions
// from a QTimer owned by the filter. A QTimer created on a plain std::thread
// has no event loop, so it never fires and the filter is created but never
// starts recording. In the normal product flow the filters are created from the
// dock (already the UI thread); the self-test has to ask for it explicitly.
void runOnUi(const std::function<void()> &fn)
{
	if (obs_in_task_thread(OBS_TASK_UI)) {
		fn();
		return;
	}
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) { (*static_cast<const std::function<void()> *>(p))(); },
		const_cast<std::function<void()> *>(&fn), true);
}

// A synthetic camera. OBS renamed the colour source a few times; try newest first.
// Put a channel's input into the scene that is meant to put it on air.
//
// A scene NAMED as the output scene but empty switches the Program to black,
// and every check that asks "did the Program change" still passes — a run that
// proves the switch happened and nothing about what went out. The harness
// generates the collection before this plugin has loaded, so the replay inputs
// do not exist yet and cannot be written into that file; they go in here.
//
// Idempotent: adds nothing that is already there. UI thread only (obs_scene_add).
// The items this harness put into scenes, so it can take them out again.
//
// IT HAS TO TAKE THEM OUT, and that is a crash paid for: adding the replay
// input to the collection's output scenes and leaving it there made OBS die on
// the way out, every run, in
//
//   obs.dll!obs_source_release          (on a NULL source)
//   obs.dll!obs_sceneitem_release
//   obs.dll!scene_destroy
//   obs.dll!obs_source_destroy_defer    (libobs' deferred destroy thread)
//
// The first one is timestamped 14:04:53, which is the run that validated the
// commit adding ensureChannelInScene. The plugin releases its channel sources
// on SCENE_COLLECTION_CLEANUP and EXIT — deliberately, so OBS does not report
// us as a plugin that failed to let go — and a scene the harness had seeded was
// then torn down around an item whose source had already gone. Whatever the
// exact ordering inside libobs, the harness leaving its own furniture in the
// operator's scenes and hoping teardown copes is the part that is ours to fix.
std::vector<obs_sceneitem_t *> g_seededItems;

// Put a channel's input into the scene that is meant to put it on air.
//
// A scene NAMED as the output scene but empty switches the Program to black,
// and every check that asks whether the Program changed still passes — a run
// that proves the switch happened and nothing about what went out. The harness
// generates the collection before this plugin has loaded, so the replay inputs
// do not exist yet and cannot be written into that file; they go in here, and
// come out in removeSeededSceneItems().
//
// Idempotent: adds nothing that is already there. UI thread only.
void ensureChannelInScene(const char *sceneName, Which which)
{
	if (!sceneName || !*sceneName)
		return;
	obs_source_t *sceneSrc = obs_get_source_by_name(sceneName);
	if (!sceneSrc)
		return;
	obs_scene_t *scene = obs_scene_from_source(sceneSrc);
	obs_source_t *chan = ReplayChannel::instance(which).acquireSource();
	if (scene && chan) {
		const char *n = obs_source_get_name(chan);
		if (n && !obs_scene_find_source(scene, n)) {
			if (obs_sceneitem_t *item = obs_scene_add(scene, chan)) {
				// A reference of our own on the ITEM, so it is
				// still ours to remove later however the scene
				// is handled in between.
				obs_sceneitem_addref(item);
				g_seededItems.push_back(item);
				obs_log(LOG_INFO,
					"[selftest] added '%s' to output scene "
					"'%s'",
					n, sceneName);
			}
		}
	}
	if (chan)
		obs_source_release(chan);
	obs_source_release(sceneSrc);
}

// Undo the above. Called at the end of a pass, on the UI thread, BEFORE the
// operator's project is restored and long before OBS clears scene data.
void removeSeededSceneItems()
{
	for (obs_sceneitem_t *item : g_seededItems) {
		if (!item)
			continue;
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}
	if (!g_seededItems.empty())
		obs_log(LOG_INFO,
			"[selftest] removed %d seeded scene item(s)",
			(int)g_seededItems.size());
	g_seededItems.clear();
}

obs_source_t *createSyntheticCamera(int idx, uint32_t cx, uint32_t cy)
{
	static const char *kColorIds[] = {"color_source_v3", "color_source_v2",
					  "color_source"};
	// Distinct colours so the recordings are visually distinguishable.
	static const uint32_t kColors[] = {0xFF2266DD, 0xFF22AA55, 0xFFDD8822,
					   0xFFCC3355, 0xFF8844CC, 0xFF11AAAA,
					   0xFFBBBB22, 0xFF777777};

	const std::string name = "MRSelfTest Cam" + std::to_string(idx + 1);

	for (const char *id : kColorIds) {
		if (!obs_source_get_display_name(id))
			continue;
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "color",
				 kColors[idx % (int)(sizeof(kColors) /
						     sizeof(kColors[0]))]);
		obs_data_set_int(s, "width", cx);
		obs_data_set_int(s, "height", cy);
		obs_source_t *src = obs_source_create(id, name.c_str(), s, nullptr);
		obs_data_release(s);
		if (src) {
			obs_log(LOG_INFO, "[selftest] created synthetic camera '%s' (%s)",
				name.c_str(), id);
			return src;
		}
	}
	obs_log(LOG_ERROR, "[selftest] no colour source type available");
	return nullptr;
}

// ---------------------------------------------------------------------------
// The dock, driven through its own widgets.
//
// Everything above this point proves the engine. None of it touches the only
// thing the operator ever uses, and the rewrite left the dock re-wired but
// never once run: a dock that throws on construction, freezes the GUI in its
// timer, marks at an instant no footage covers, or fails to play what it just
// marked would sail through the whole M0 gate. These checks close that hole by
// finding the REAL dock OBS registered and clicking its REAL buttons.
// ---------------------------------------------------------------------------

struct DockChecks {
	bool found = false;
	bool pollRuns = false;
	bool pollQuiet = false;
	bool pollResponsive = false;
	// How hard the panel is leaning on Qt. The two bars used to ask for a
	// full-width repaint on EVERY tick of the 30 Hz poll, whether or not
	// anything had moved, and every one of those makes Qt re-compose and
	// flush the backing store of the OBS main window — the same top-level
	// that carries our native flip-model swap-chain children. On a live rig
	// that combination took every Qt-painted pixel in the OBS window black
	// while the previews kept drawing, and the operator got it back only by
	// clicking until a repaint landed.
	//
	// So this is the check that stands between that bug and its return, and
	// it is MEASURED rather than argued from the source: a guard that stops
	// being true is invisible in a reading of the code.
	bool repaintRateSane = false;
	double seekRepaintsPerSec = 0.0;
	double clipRepaintsPerSec = 0.0;
	double seekSuppressedPerSec = 0.0;
	bool markOnTimeline = false;
	bool playsMark = false;
	bool playheadInsideClip = false;
	bool multiAngleQueue = false;
	// Any combination of enabled angles, and the combination changing between
	// two plays of the SAME event. These are separate claims: the first is
	// about how a queue is built, the second about the previous queue being
	// gone before the new one is built.
	bool angleCombinations = false;
	bool angleChoiceRepeatable = false;
	bool singleNonFirstAnglePlays = false;
	// The queue does not just hold two clips, it walks them: clip 1 ends and
	// clip 2 goes on air by itself. That hop crosses two threads (the playback
	// worker's finish callback, re-posted onto the OBS UI queue) and is the
	// half of "play both angles" no shape check can see.
	bool queueAdvancesToSecond = false;
	// The preview belongs to the SEQUENCE, not to the clip: between two angles
	// of the same event the engine is briefly idle, and the dock used to put the
	// live camera on screen for that moment - on air too, with "to output".
	bool previewHoldsSequence = false;
	// ...and when the sequence ends, the transport goes back to the live edge by
	// itself, instead of leaving the operator to press NOW.
	bool followsLiveAfterSequence = false;
	// A scrub shows the FOOTAGE at that instant and keeps showing it after the
	// review clip has run out - the live camera belongs to follow-live only.
	bool scrubShowsFootage = false;
	// The angle button wins: Single mode plays the camera it was asked for, and
	// refuses out loud when that camera has nothing, instead of quietly playing
	// a different one.
	bool singleHonoursRequestedAngle = false;
	bool singleReportsUnplayableAngle = false;
	// A mark flags the angle the operator is WATCHING. Tested on an angle that
	// is not the first one, because "always flag camera 1" passes any check made
	// on camera 1 - and that was the real bug on the hotkey path.
	bool markInheritsAngle = false;
	// reference parity, M3. The id is drawn zero-padded to the configured width (so
	// it can be called out loud), double-clicking a row puts that event on air,
	// and the frame step really moves the picture on.
	bool idPadded = false;
	bool doubleClickPlays = false;
	bool frameStepAdvances = false;
	// v1.3: the two backwards keys, on the real widgets. A step back is the
	// one transport gesture that can look like it worked while doing nothing —
	// played forwards from one frame back it would come to rest on the frame
	// already on screen — so what is checked is that the picture the engine
	// pushed is EARLIER than the one before the press, not merely that
	// something played.
	bool stepBackMovesPlayhead = false;
	bool reverseButtonPlaysBackwards = false;
	// ONE layer of input: a real key event into the dock has to do what the key
	// of the same name does. Both ways this breaks are invisible to a direct call
	// of the handler — the table eating ↑/↓ and Enter for its own navigation, and
	// a focused button in a QButtonGroup taking the arrows for focus travel.
	bool keyboardLayerWorks = false;
	// Config.continuePastOutMs: the LAST clip of a queue runs past the event's
	// OUT. Read off queuedWallNs, which is what the green band counts down.
	bool continuePastOutExtends = false;
	int64_t continueExtraMs = 0;
	// The transport fixes. All four used to be answered by "stop and play it
	// again from the in-point", which is not what any of them mean — so each
	// check is about WHERE the playhead is afterwards, not about whether
	// something is playing.
	bool pauseHoldsAndResumes = false;
	bool speedChangeKeepsPosition = false;
	bool selectionCuesEvent = false;
	bool angleKeysFollowCameras = false;
	bool clipBarSpansSequence = false;
	int visibleAngleKeys = 0;
	// The second bay is optional, and OFF is the default: with it off the B box
	// and the A|B/A/B selector are absent, with it on they are there.
	bool channelBIsOptional = false;
	// Nothing still referenced when OBS clears scene data: a held reference there
	// becomes a dialog telling the operator a plugin leaked.
	bool releasesSourcesOnCleanup = false;
	// M5: the preview is no longer one picture. There is a tile per camera
	// plus one for the replay, and each is an obs_display of its own — so the
	// two things that can go wrong are "the tiles were never built" and "a
	// tile is on screen with no display behind it" (a black rectangle, which
	// looks exactly like a camera with no signal).
	bool multiviewBuilt = false;
	bool multiviewDisplaysLive = false;
	// ...and the hole that check had: `withDisplay == visible` is TRUE when
	// nothing is visible, so a dock that never made it on screen — and every
	// run's first instants — passed it without a single display existing.
	// Now the panel has to have pictures on screen, each with a display, and
	// none of them may have been waiting for one longer than a start-up
	// explains (PreviewStats::starved).
	bool previewsNeverStarved = false;
	int previewTilesStarved = 0;
	int64_t worstDisplayWaitMs = 0;
	int displaysForced = 0;
	// M5: the per-angle triplet lives IN the table — one column pair per
	// configured camera ([enable + speed], [comment]) and NO per-event speed
	// column. Both halves are checked on real cells: the geometry (which is
	// what proves the event-speed column is gone) and an edit typed into the
	// speed cell, which must reach the store as that ANGLE's speed.
	bool angleColumnsInTable = false;
	bool tableEditsAngleSpeed = false;
	// A search hides rows and clearing it brings them back. What comes back
	// has to be what the STORE holds — reported from a real panel as the
	// per-angle speed and comment reverting to their placeholders once a
	// search was cleared, with the values still correct in events.json. The
	// store is the truth here; the cell is the thing that can lie.
	bool searchRestoresCellValues = false;
	std::string searchCellNote;   // what the cell showed afterwards
	std::string searchStoredNote; // what the store held all along
	int searchCellPct = 0;
	int searchStoredPct = 0;
	int eventTableColumns = 0;
	// M5: the green band is the state of the clip ON AIR (id, angle, time
	// left, speed) with its fill as the progress through that clip — and the
	// >> key beside it takes the next item of the queue. Both are checked on
	// the real widgets: a status bar nobody updates looks exactly like a
	// status bar that has nothing to say.
	bool clipBarReportsOnAir = false;
	bool skipAdvancesQueue = false;
	// v1.4: FOOTAGE NOBODY MARKED.
	//
	// ▶ on a stretch no event covers has to play THAT stretch, and it has to
	// do it off air — the second half is the whole safety of the first, and
	// it is invisible to any check that only asks whether something played.
	// So the Program scene is read before and after, and a review that took
	// it fails here.
	bool freeReviewPlaysUnmarked = false;
	// ...and Play events is what puts it up. The claim is not "something
	// played": it is that the queue starts at the instant the operator armed
	// and carries NO event id. An event replay would satisfy a weaker check
	// exactly, and that IS the bug — the key ignoring the free review and
	// replaying the selected row instead.
	bool freeReviewIsSecondPlayFunction = false;
	// A free run ends on Stop, so Stop has to be a key. It used to be two
	// clicks deep in a menu, which was survivable only while every replay
	// ended by itself at its OUT.
	bool stopKeyEndsPlayback = false;
	// v1.4: THE ANGLE BOXES FOLLOW THE REVIEW.
	//
	// A tile still mirroring the live camera during a replay looks perfectly
	// fine on its own — it is a moving picture of the right camera. It is
	// wrong only next to the picture on air, which no widget check compares
	// it with. So this asks the dock what the boxes are for right now, and
	// how many feeds have really pushed a frame: a feed that never decoded
	// anything and a camera with no signal are the same black rectangle.
	bool multiviewFollowsReview = false;
	int multiviewFeeds = 0;
	int multiviewFeedsWithPicture = 0;
	// ...and Live is what brings them back — including letting the feeds GO.
	// Eight decoders kept alive to hold a still nobody is looking at is the
	// one way this feature could cost something while it is not in use.
	bool multiviewReturnsToLive = false;
	// ...and a cue does not blank them on the way. The feeds stay alive,
	// decoding and correct through the whole thing — it is only what is
	// PUBLISHED for the graphics thread that used to go away, for a quarter
	// of a second, on every cue. Invisible to every other check here.
	// MEASUREMENTS, NOT CHECKS, and the difference was established by trying
	// to make them checks and failing.
	//
	// `multiviewCueGapMs` is how far apart the bay and the angle boxes get
	// their picture on a cue; `multiviewCueDarkMs` is how long a tile's source
	// is withdrawn while it happens. Both are the symptom reported from a real
	// panel, and neither can be ASSERTED from here:
	//
	//   - the gap only exists when the clip comes from a FILE (an open, a
	//     seek and a demux, ~100 ms per angle). Everything the gate can cue
	//     inside a 25 s take is in the ring, where a fetch is a memcpy — so
	//     the gap is 0 ms whether the fetches are issued in parallel or one
	//     behind the other. Measured both ways, with the fix and with it
	//     deliberately disabled: 10 ms and 0 ms.
	//   - the blank is intermittent by construction. It needs the 4 Hz
	//     refresh to land inside the few tens of milliseconds between play()
	//     and the first frame, so one cue catches it perhaps a third of the
	//     time. An assertion on it would be a check that fails at random,
	//     which is worse than no check.
	//
	// So they are reported as numbers a regression moves, and the claim they
	// would have made is not made. A green check that cannot fail is
	// furniture, and furniture is what nobody looks at on the day it should
	// have gone red.
	int multiviewCueDarkMs = -1;
	int multiviewCueGapMs = -1;
	// HOW LONG A CUE TAKES TO SHOW, best and worst over several cold cues.
	// The complaint after the symmetry was fixed was that the wait itself
	// comes and goes — quick on one row, a noticeable pause on the next — so
	// the SPREAD is the number, not an average. An average of four cues where
	// three are instant and one is a third of a second reads as "fine".
	int cueBayMinMs = -1;
	int cueBayMaxMs = -1;
	int cueTileMinMs = -1;
	int cueTileMaxMs = -1;
	// M5: the running order is the operator's. The ▲/▼ keys must really move
	// the ROW (not just a field nobody draws), and reordering by hand must
	// turn the chronological auto-sort off — with both on, the row snaps back
	// and nothing says why.
	bool manualReorderMovesRow = false;
	bool manualReorderDisablesAutoSort = false;
	// M5: a renamed list must be READABLE. The tab bar keeps every tab at its
	// natural width and scrolls; if a tab is ever laid out narrower than it
	// asked for, Qt draws the name cut off ("PAR…") and the name is worth
	// nothing. And the number of tabs follows the configured list count.
	bool listTabsFitTheirNames = false;
	bool listTabCountFollowsConfig = false;
	int visibleListTabs = 0;
	// M6: the ORDER of the panel's zones, and the position bar being a scale.
	// Both are things every other check in this file was blind to: the search
	// row sat above the pictures for a whole milestone with every widget
	// check passing (they were all present, just in the wrong place), and
	// "there is a SeekBar" stayed true while the operator was telling us he
	// could not see one. Read off real geometry through MultiReplayDock::
	// layoutProbe().
	bool layoutOrderTopToBottom = false;
	bool seekbarGraduated = false;
	int seekGraduations = 0;
	int seekHeight = 0;
	// ...and the failure this whole widget family is famous for: a display
	// left presenting into a native window Qt has destroyed. It used to be
	// visible only by reading the OBS log by eye, which is the check that
	// stops being done as soon as there are ten of them.
	bool displaysNeverStranded = false;
	int previewTiles = 0;
	int previewTilesVisible = 0;
	int previewTilesWithDisplay = 0;
	int displaysCreated = 0;
	int displaysStranded = 0;
	// A NUMBER IN THE REPORT, NOT A CHECK — on purpose. Nobody knows yet what
	// the right value is: it counts displays whose top-level was rebuilt under
	// them (qt-display.cpp, the ancestor check), which is a hole this file
	// could not see at all until now. Asserting on it before knowing what a
	// healthy run produces would be inventing the answer.
	int displaysReparented = 0;
	int previewLiveSamples = 0; // frames of the sequence spent on the live camera
	int queuedClips = 0;
	int ticks = 0;
	int64_t worstGapMs = 0;
	int logErrors = 0;
	int64_t markInNs = 0;
	int64_t markOutNs = 0;
	int playedFrames = 0;
	// Channel B, the swap, the trim keys and the zoom — the four things
	// that shipped without a check and therefore without a claim.
	int channelBFrames = 0;
	// FIRST RUN. The harness configures a rig, so the guided setup must NOT
	// consider itself needed — the way this goes wrong is a wizard in the face
	// of an operator whose panel has been set up for months.
	bool setupNotNeededWhenConfigured = false;
	bool channelBIndependent = false;
	bool swapMovesClip = false;
	bool trimMovedIn = false;
	bool seekbarZooms = false;

	bool dragMovesMarker = false;
	bool secondsHotkeyMovesPoint = false;
	int64_t trimDeltaMs = 0;
	double zoomReached = 1.0;
};

// --- M4: hardening ----------------------------------------------------------
// Four claims, all of them about a rig that is having a bad day:
//   1. a take that cannot work is refused at REC, by the engine and not by the
//      dock, so a hotkey is refused exactly like the button;
//   2. the real rig clears pre-flight, so the refusal above is not a rig that
//      simply refuses everything;
//   3. the monitor really samples a running take;
//   4. an angle that DIES is reported, by name, while the take keeps running
//      and the Program is not touched — which is the whole M4 rule.
struct HealthChecks {
	bool ran = false;
	bool recRefusesImpossibleTake = false;
	bool preflightClearsTheRig = false;
	bool monitorSamplesTheTake = false;
	bool deadAngleReported = false;
	bool deadAngleIsNotFatal = false;
	bool findingsClearedAtStop = false;
	// B has an output scene of its own (the bug where play on B put A on air).
	bool bTakesItsOwnScene = false;
	int samples = 0;
	int64_t detectMs = -1;
	std::string refusal;     // what REC said when it refused
	std::string deadFinding; // the id that named the killed camera
	std::string sceneBefore;
	std::string sceneAfter;
};

// Counts OUR error lines while the dock's timer is being watched. Only ours:
// the claim is "the dock's poll is quiet", not "nothing in OBS ever complains".
std::atomic<int> g_ourLogErrors{0};
std::atomic<bool> g_countLogErrors{false};
log_handler_t g_prevLogHandler = nullptr;
void *g_prevLogParam = nullptr;

void countingLogHandler(int level, const char *msg, va_list args, void *)
{
	if (g_countLogErrors.load() && level <= LOG_ERROR && msg &&
	    strstr(msg, PLUGIN_NAME) != nullptr)
		g_ourLogErrors++;
	// Chain, so the run still produces its normal OBS log.
	if (g_prevLogHandler)
		g_prevLogHandler(level, msg, args, g_prevLogParam);
}

// A short take of its own, driven through ReplayCore::startRecording — the
// product path, the one a hotkey uses — and then deliberately broken.
//
// It runs after the measurement take has been torn down, so the two never share
// an encoder, and it puts the operator's configuration back before it returns.
// The camera names it installs are logged first: if OBS were killed in the
// middle of this, that log line is how they are recovered.
HealthChecks runHealthChecks(const std::vector<obs_source_t *> &cams, int camCount,
			     const std::array<bool, kMaxTapChannels> &want)
{
	HealthChecks c;
	auto &core = ReplayCore::instance();
	auto &monitor = HealthMonitor::instance();

	int first = -1, victim = -1;
	for (int i = 0; i < camCount; i++) {
		if (!want[i] || !cams[i])
			continue;
		if (first < 0)
			first = i;
		else if (victim < 0)
			victim = i;
	}
	if (first < 0) {
		obs_log(LOG_ERROR, "[selftest] no armed camera — health checks skipped");
		return c;
	}
	c.ran = true;

	const Config original = core.getConfig();
	Config testCfg = original;
	for (auto &cam : testCfg.cameras)
		cam.sourceName.clear();
	std::string installed;
	for (int i = 0; i < camCount && i < kMaxCameras; i++) {
		if (!want[i] || !cams[i])
			continue;
		const char *n = obs_source_get_name(cams[i]);
		testCfg.cameras[i].sourceName = n ? n : "";
		installed += (installed.empty() ? "" : ", ") +
			     std::to_string(i + 1) + "=" + testCfg.cameras[i].sourceName;
	}
	std::string originalNames;
	for (int i = 0; i < kMaxCameras; i++)
		if (!original.cameras[i].sourceName.empty())
			originalNames += (originalNames.empty() ? "" : ", ") +
					 std::to_string(i + 1) + "=" +
					 original.cameras[i].sourceName;
	obs_log(LOG_INFO,
		"[selftest] health pass: camera config [%s] -> [%s] (restored at the "
		"end of this pass)",
		originalNames.c_str(), installed.c_str());

	// --- 1. REC refuses a take that cannot work ---------------------------
	// Not "the dock refuses": the refusal has to come out of the engine, or a
	// take started from a hotkey or a Stream Deck would arm filters over a
	// configuration pre-flight has already rejected.
	{
		Config broken = testCfg;
		for (auto &cam : broken.cameras)
			cam.sourceName.clear();
		bool started = true;
		std::string err;
		runOnUi([&]() {
			core.setConfig(broken);
			started = core.startRecording(err);
		});
		c.refusal = err;
		c.recRefusesImpossibleTake = !started && !err.empty() &&
					     !core.isRecording();
		if (started)
			runOnUi([&]() { core.stopRecording(); });
		obs_log(c.recRefusesImpossibleTake ? LOG_INFO : LOG_ERROR,
			"[selftest] REC on a rig with no camera: %s (\"%s\")",
			started ? "STARTED — pre-flight did not refuse" : "refused",
			err.c_str());
	}

	runOnUi([&]() { core.setConfig(testCfg); });

	// --- 2. ...and it clears the rig that does work -----------------------
	{
		const auto pf = monitor.preflight(testCfg, 90);
		c.preflightClearsTheRig = pf.ok();
		for (const auto &f : pf.findings)
			obs_log(LOG_INFO, "[selftest] preflight %s: %s %s",
				health::levelName(f.level), f.id.c_str(),
				f.detail.c_str());
		obs_log(c.preflightClearsTheRig ? LOG_INFO : LOG_ERROR,
			"[selftest] pre-flight on the real rig: %s (ring %d s)",
			c.preflightClearsTheRig ? "clear" : "REFUSED",
			pf.ringSeconds);
	}

	// --- 3. the monitor watches a running take ----------------------------
	const uint64_t samplesBefore = monitor.samples();
	bool started = false;
	std::string err;
	runOnUi([&]() { started = core.startRecording(err); });
	if (!started) {
		obs_log(LOG_ERROR, "[selftest] health pass could not record: %s",
			err.c_str());
		runOnUi([&]() { core.setConfig(original); });
		return c;
	}
	// Wait for FRAMES, not for flags. "Attached" is not "capturing": the tap
	// can land on an encoder Branch Output is about to replace, and an angle
	// that reports attached while receiving nothing would make the check
	// below meaningless — it would kill a camera and then measure a survivor
	// that was never alive either.
	bool bothFlowing = false;
	for (int i = 0; i < 60 && !bothFlowing; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		bothFlowing = core.branchOutputRecording() &&
			      PacketTap::instance().newestNs(first) > 0 &&
			      (victim < 0 ||
			       PacketTap::instance().newestNs(victim) > 0);
	}
	obs_log(bothFlowing ? LOG_INFO : LOG_ERROR,
		"[selftest] health take: every armed angle producing packets: %s",
		bothFlowing ? "yes" : "NO");
	// Let the monitor see a few seconds of a healthy take before anything is
	// broken on purpose.
	std::this_thread::sleep_for(std::chrono::seconds(6));
	c.samples = (int)(monitor.samples() - samplesBefore);
	// A healthy take is a SILENT take: the samples happened and found nothing
	// worth blocking on. (A warning is allowed: the gate's own machine may
	// legitimately be short of disk or RAM, and that is not a regression.)
	c.monitorSamplesTheTake = bothFlowing && c.samples >= 4 &&
				  monitor.worst() < health::Level::Blocker;
	obs_log(c.monitorSamplesTheTake ? LOG_INFO : LOG_ERROR,
		"[selftest] health monitor took %d samples during the take, worst = %s",
		c.samples, health::levelName(monitor.worst()));

	// --- 4. kill an angle -------------------------------------------------
	// The one thing hardening is for: a camera stops mid-match. It must be
	// reported by name, and NOTHING else may happen — the take keeps running,
	// the other angle keeps recording, the Program is not touched.
	if (victim >= 0) {
		runOnUi([&]() {
			if (obs_source_t *sc = obs_frontend_get_current_scene()) {
				const char *n = obs_source_get_name(sc);
				c.sceneBefore = n ? n : "";
				obs_source_release(sc);
			}
		});
		const int64_t survivorBefore = PacketTap::instance().newestNs(first);

		runOnUi([&]() {
			const std::string fname =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(victim + 1);
			if (obs_source_t *f = obs_source_get_filter_by_name(
				    cams[victim], fname.c_str())) {
				branch_output::setEnabled(f, false);
				obs_source_release(f);
			}
		});
		obs_log(LOG_INFO, "[selftest] killed CAM%d mid-take on purpose",
			victim + 1);

		const std::string wanted = "CAM" + std::to_string(victim + 1);
		const int64_t t0 = (int64_t)os_gettime_ns();
		for (int i = 0; i < 44 && !c.deadAngleReported; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			for (const auto &f : monitor.findings()) {
				if (f.detail.rfind(wanted, 0) != 0)
					continue;
				if (f.id == "angle_dead" || f.id == "angle_stalled" ||
				    f.id == "angle_no_packets" ||
				    f.id == "angle_not_tapped") {
					c.deadAngleReported = true;
					c.deadFinding = f.id + " (" + f.detail + ")";
					c.detectMs =
						((int64_t)os_gettime_ns() - t0) /
						1'000'000LL;
					break;
				}
			}
		}
		runOnUi([&]() {
			if (obs_source_t *sc = obs_frontend_get_current_scene()) {
				const char *n = obs_source_get_name(sc);
				c.sceneAfter = n ? n : "";
				obs_source_release(sc);
			}
		});
		const int64_t survivorAfter = PacketTap::instance().newestNs(first);
		// "Not fatal" is three separate facts, and all three have to hold:
		// the take is still on, the surviving angle is still capturing, and
		// nothing switched what is on air.
		c.deadAngleIsNotFatal = core.isRecording() &&
					survivorAfter > survivorBefore &&
					c.sceneAfter == c.sceneBefore;
		obs_log(c.deadAngleReported ? LOG_INFO : LOG_ERROR,
			"[selftest] dead angle reported after %lld ms as %s; take still "
			"running=%s, survivor advanced=%s, program scene '%s' -> '%s'",
			(long long)c.detectMs,
			c.deadFinding.empty() ? "(nothing)" : c.deadFinding.c_str(),
			core.isRecording() ? "yes" : "NO",
			survivorAfter > survivorBefore ? "yes" : "NO",
			c.sceneBefore.c_str(), c.sceneAfter.c_str());
	}

	runOnUi([&]() { core.stopRecording(); });
	c.findingsClearedAtStop = monitor.findings().empty();

	// --- B goes to ITS OWN scene ------------------------------------------
	// Reported from the panel: with B selected and "to output" on, program
	// showed A. Both channels switched to the one configured scene — the one
	// holding A's input — so B's clip played into an input nobody was
	// looking at.
	//
	// Checked HERE, with nothing recording, and not among the dock checks:
	// it needs setConfig, setConfig re-applies the Branch Output filter
	// settings, and that makes Branch Output rebuild its encoders out from
	// under the tap (the architecture notes's oldest trap). Run mid-take it cost the two
	// health checks their packets — the gate caught its own test.
	{
		auto &store = EventStore::instance();
		const int64_t newest = PacketTap::instance().newestNs(first);
		int evId = 0;
		if (newest > 0) {
			evId = store.markIn(newest - 3'000'000'000LL, first);
			store.markOut(newest - 1'000'000'000LL);
			store.setAngle(evId, first + 1, true);
		}
		obs_source_t *sceneA = nullptr;
		obs_source_t *sceneB = nullptr;
		std::string beforeScene;
		Config cfgOut = core.getConfig();
		runOnUi([&]() {
			if (obs_source_t *cur = obs_frontend_get_current_scene()) {
				const char *n = obs_source_get_name(cur);
				beforeScene = n ? n : "";
				obs_source_release(cur);
			}
			obs_scene_t *sa = obs_scene_create("MRSelfTest Out A");
			obs_scene_t *sb = obs_scene_create("MRSelfTest Out B");
			sceneA = sa ? obs_scene_get_source(sa) : nullptr;
			sceneB = sb ? obs_scene_get_source(sb) : nullptr;
			cfgOut.outputSceneName = "MRSelfTest Out A";
			cfgOut.outputSceneNameB = "MRSelfTest Out B";
			core.setConfig(cfgOut);
			// ...with the replay in them. An output scene that does not
			// contain the channel puts BLACK on the Program, and the
			// check that only asks "did the Program change" passes.
			ensureChannelInScene("MRSelfTest Out A", Which::A);
			ensureChannelInScene("MRSelfTest Out B", Which::B);
		});
		std::string oerr;
		std::string programAfterB;
		if (evId > 0)
			runOnUi([&]() {
				PlaybackCoordinator::instance(Which::B).playEvents(
					{evId}, first, /*toOutput*/ true, oerr,
					PlaybackCoordinator::AngleMode::Single);
			});
		for (int i = 0; i < 20 && programAfterB != "MRSelfTest Out B"; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			runOnUi([&]() {
				if (obs_source_t *cur =
					    obs_frontend_get_current_scene()) {
					const char *n = obs_source_get_name(cur);
					programAfterB = n ? n : "";
					obs_source_release(cur);
				}
			});
		}
		c.bTakesItsOwnScene = programAfterB == "MRSelfTest Out B";
		obs_log(c.bTakesItsOwnScene ? LOG_INFO : LOG_ERROR,
			"[selftest] B to output put '%s' on program (wanted "
			"'MRSelfTest Out B')%s%s",
			programAfterB.c_str(), oerr.empty() ? "" : " — ",
			oerr.c_str());

		PlaybackCoordinator::instance(Which::B).stopEvents();
		if (evId > 0)
			store.remove(evId);
		runOnUi([&]() {
			if (!beforeScene.empty()) {
				if (obs_source_t *s = obs_get_source_by_name(
					    beforeScene.c_str())) {
					obs_frontend_set_current_scene(s);
					obs_source_release(s);
				}
			}
			if (sceneA)
				obs_source_remove(sceneA);
			if (sceneB)
				obs_source_remove(sceneB);
		});
	}

	runOnUi([&]() { core.setConfig(original); });
	obs_log(LOG_INFO, "[selftest] health pass done, camera config restored");
	return c;
}

DockChecks runDockChecks(int firstCam, int secondCam,
			 const std::string &tempFolder, double canvasFps)
{
	DockChecks c;

	MultiReplayDock *dock = nullptr;
	QTimer *pollTimer = nullptr;
	QPushButton *markBtn = nullptr;     // the "-5s" preset
	QPushButton *playBtn = nullptr;     // "play selected"
	QPushButton *stepBtn = nullptr;     // "one frame forward"
	QPushButton *stepBackBtn = nullptr; // "one frame back"
	QPushButton *revBtn = nullptr;      // "play backwards"

	runOnUi([&]() {
		auto *main = static_cast<QMainWindow *>(
			obs_frontend_get_main_window());
		if (!main)
			return;
		// The registered dock, not a fresh one: this also proves
		// obs_frontend_add_dock_by_id actually took it.
		dock = main->findChild<MultiReplayDock *>();
		if (!dock)
			return;
		for (QTimer *t : dock->findChildren<QTimer *>()) {
			if (t->isActive() && t->interval() > 0 &&
			    t->interval() <= 100) {
				pollTimer = t;
				break;
			}
		}
		// ── FOUND BY IDENTITY, NOT BY LABEL ──────────────────────
		//
		// These used to be matched on the literal text the dock built
		// them with — "-5s", "⏭", "■", ">>" — and that made every one of
		// them a tripwire under the WRONG thing. A label is a translation
		// and a glyph is a drawing; neither is what the key IS. The panel
		// redesign that replaced the glyphs with drawn marks would have
		// broken twelve checks at once while every key still worked
		// perfectly, which is a gate reporting on itself.
		//
		// Every command key now carries a stable id in a dynamic property
		// (see dock-icons.hpp). It survives a locale, a font and a
		// redesign, and there is exactly one key per id — which is the
		// constraint that used to be written as "exactly one button may
		// carry this glyph" and is now enforced by the panel rather than
		// hoped for.
		for (QPushButton *b : dock->findChildren<QPushButton *>()) {
			const QString id = b->property(kKeyProperty).toString();
			if (id == QLatin1String("mark5"))
				markBtn = b;
			else if (id == QLatin1String("playEvents"))
				playBtn = b;
			else if (id == QLatin1String("stepFwd"))
				stepBtn = b;
			else if (id == QLatin1String("stepBack"))
				stepBackBtn = b;
			else if (id == QLatin1String("playReverse"))
				revBtn = b;
		}
	});

	c.found = dock && pollTimer && markBtn && playBtn && stepBtn &&
		  stepBackBtn && revBtn && revBtn->isEnabled();
	if (!c.found) {
		obs_log(LOG_ERROR,
			"[selftest] dock not usable (dock=%p timer=%p mark=%p "
			"play=%p step=%p stepBack=%p reverse=%p%s)",
			(void *)dock, (void *)pollTimer, (void *)markBtn,
			(void *)playBtn, (void *)stepBtn, (void *)stepBackBtn,
			(void *)revBtn,
			(revBtn && !revBtn->isEnabled()) ? " DISABLED" : "");
		return c;
	}

	// --- Is the timer running, and does it hand the GUI thread back? -----
	// The gap between consecutive ticks is measured AFTER the dock's own
	// slot has run (our connection is made later, so it fires later), which
	// makes the worst gap the honest cost of a poll: a poll that blocks —
	// on a network stat(), a lock, a modal — shows up here and nowhere else.
	std::atomic<int> ticks{0};
	std::atomic<int64_t> worstGapNs{0};
	int64_t lastTickNs = 0; // UI thread only
	QMetaObject::Connection conn;

	g_ourLogErrors.store(0);
	base_get_log_handler(&g_prevLogHandler, &g_prevLogParam);
	base_set_log_handler(countingLogHandler, nullptr);
	g_countLogErrors.store(true);
	// Census readings taken across the SAME window as the tick count, so the
	// repaint rate below is per second of the window actually measured and not
	// of an idealised one.
	uint64_t seekReq0 = 0, seekSup0 = 0, clipReq0 = 0;

	// UP TO THREE WINDOWS, stopping at the first good one, and the reason is a
	// false failure this check produced on a run where nothing was wrong.
	//
	// What it is FOR is a poll timer that never started or is wedged behind
	// something blocking. What it cannot tell apart from that, on one sample,
	// is the gate's OWN busiest two seconds: this window used to land right
	// after the reverse check, which decodes a GOP three times over on an iGPU,
	// and it counted 15 ticks where the dock's own accounting (see the [ui]
	// lines in the log) reported 20-21 per second either side of it. A wedged
	// timer stays wedged for all three windows; a busy moment does not survive
	// one. Loosening the threshold instead would have thrown away the one
	// failure the check exists to catch.
	for (int attempt = 1; attempt <= 3; attempt++) {
		ticks.store(0);
		worstGapNs.store(0);
		g_ourLogErrors.store(0);
		seekReq0 = g_seekCensus.requested;
		seekSup0 = g_seekCensus.suppressed;
		clipReq0 = g_clipCensus.requested;
		runOnUi([&]() {
			lastTickNs = (int64_t)os_gettime_ns();
			conn = QObject::connect(pollTimer, &QTimer::timeout, dock,
						[&]() {
							const int64_t now =
								(int64_t)os_gettime_ns();
							const int64_t gap =
								now - lastTickNs;
							lastTickNs = now;
							if (gap > worstGapNs.load())
								worstGapNs.store(gap);
							ticks++;
						});
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		runOnUi([&]() { QObject::disconnect(conn); });

		c.ticks = ticks.load();
		c.worstGapMs = worstGapNs.load() / 1000000;
		if (c.ticks >= 25 && c.worstGapMs <= 250)
			break;
		obs_log(LOG_INFO,
			"[selftest] dock: poll window %d/3 saw %d ticks, worst gap "
			"%lld ms — re-sampling (a wedged timer stays wedged)",
			attempt, c.ticks, (long long)c.worstGapMs);
	}
	g_countLogErrors.store(false);
	base_set_log_handler(g_prevLogHandler, g_prevLogParam);

	c.logErrors = g_ourLogErrors.load();
	// A 33 ms timer would fire ~60 times in 2 s in the arithmetic; it does not,
	// and never did. Measured on this machine across every run of the gate:
	// 42-43 ticks, i.e. a ~46 ms period, because these 2 s are shared with
	// everything else OBS is doing (encoding two angles, the multiview, a
	// playback). That is 21 Hz of seekbar, which is fine, and it is the normal
	// state of this check — so a threshold of 40 sat 7% above the norm and
	// failed the whole gate on ordinary jitter (33 ticks on one run, 40 on
	// another, both with the dock perfectly alive).
	//
	// What this check is FOR is a timer that never started or one wedged
	// behind something blocking, and 25 ticks (~12 Hz) still catches both by a
	// wide margin. The single-stall case is the worst-gap check below, which
	// is the one that should stay tight.
	c.pollRuns = c.ticks >= 25;
	// 250 ms is far above normal jitter and far below anything a human would
	// not notice as a freeze.
	c.pollResponsive = c.pollRuns && c.worstGapMs <= 250;
	c.pollQuiet = c.logErrors == 0;
	obs_log(LOG_INFO,
		"[selftest] dock: %d poll ticks in 2 s, worst gap %lld ms, "
		"%d plugin errors logged",
		c.ticks, (long long)c.worstGapMs, c.logErrors);

	// --- the repaint rate -------------------------------------------------
	// TWO conditions, and the second is the one that actually catches the bug.
	//
	// The ceiling (45/s per bar) only rules out an absurd rate: a repaint per
	// bar per poll tick is ~30/s and legitimate while a clip is running, since
	// the timecode printed on the track changes every tick. Deferred repaints
	// are held for at most one frame, so nothing beyond that is expected.
	//
	// `suppressed > 0` is the real detector. The failure this stands in front
	// of was two setters that repainted UNCONDITIONALLY: called on every tick
	// of a 30 Hz poll with values identical to the tick before, and repainting
	// the full-width bar anyway, forever, including with the panel completely
	// idle. In that code `suppressed` is zero BY CONSTRUCTION — there was no
	// comparison to fail. So a run where nothing is ever suppressed is a run
	// where the guards are gone, whatever the rate happens to look like.
	//
	// It is also why a low rate alone must not pass: a stopped poll produces a
	// low rate too, and a check that goes green when the panel is dead is not
	// a check.
	const double secs = 2.0;
	c.seekRepaintsPerSec = (double)(g_seekCensus.requested - seekReq0) / secs;
	c.seekSuppressedPerSec =
		(double)(g_seekCensus.suppressed - seekSup0) / secs;
	c.clipRepaintsPerSec = (double)(g_clipCensus.requested - clipReq0) / secs;
	c.repaintRateSane = c.seekRepaintsPerSec <= 45.0 &&
			    c.clipRepaintsPerSec <= 45.0 &&
			    c.seekSuppressedPerSec > 0.0;
	obs_log(LOG_INFO,
		"[selftest] dock repaints/s: seek %.1f (suppressed %.1f), clip %.1f "
		"— ceiling 45.0, and suppressed must be > 0",
		c.seekRepaintsPerSec, c.seekSuppressedPerSec,
		c.clipRepaintsPerSec);

	// --- the preview area: one tile per angle, plus the replay ------------
	// Structure first, because it holds whether the dock is on screen or
	// tabbed behind another: nine tiles plus the big preview must EXIST. Then
	// the part that only means something when they are visible — a tile with
	// no obs_display behind it is a black rectangle, indistinguishable from a
	// camera that has lost signal.
	{
		MultiReplayDock::PreviewStats st;
		runOnUi([&]() { st = dock->previewStats(); });
		c.previewTiles = st.tiles;
		c.previewTilesVisible = st.visible;
		c.previewTilesWithDisplay = st.withDisplay;
		c.displaysCreated = OBSQTDisplay::createdCount();
		c.displaysStranded = OBSQTDisplay::strandedCount();
		c.displaysForced = OBSQTDisplay::forcedCount();
		c.displaysReparented = OBSQTDisplay::reparentedCount();
		c.previewTilesStarved = st.starved;
		c.worstDisplayWaitMs = st.worstBlockedMs;
		// 1 big preview per BAY (2) + 8 camera tiles + the replay tile.
		//
		// It was 10, and the missing one was bay B — the same widget the
		// destructor forgot to detach its draw callback from (B4). A census
		// that does not count a display cannot report it starved or
		// stranded either, so B was invisible to three checks at once. It is
		// counted whether or not the second bay is enabled: the widget
		// exists, hidden, and a hidden OBSQTDisplay creates no display at
		// all — which is what the visible/withDisplay pair below measures.
		c.multiviewBuilt = st.tiles == 11;
		// There must BE pictures on screen. Without that clause this is
		// "0 == 0" on a dock nobody ever showed, which is exactly the
		// shape of the failure it is supposed to catch.
		c.multiviewDisplaysLive =
			st.visible > 0 && st.withDisplay == st.visible;
		c.previewsNeverStarved = st.starved == 0;
		c.displaysNeverStranded = c.displaysStranded == 0;
		obs_log((c.multiviewBuilt && c.multiviewDisplaysLive &&
			 c.previewsNeverStarved && c.displaysNeverStranded)
				? LOG_INFO
				: LOG_ERROR,
			"[selftest] dock: %d preview widget(s), %d visible, %d with a "
			"live display, %d starved (worst wait %lld ms); %d display(s) "
			"created, %d forced past exposure, %d stranded, %d left on a "
			"rebuilt top-level",
			st.tiles, st.visible, st.withDisplay, st.starved,
			(long long)st.worstBlockedMs, c.displaysCreated,
			c.displaysForced, c.displaysStranded,
			c.displaysReparented);
	}

	// --- the zones are in the operator's order, and the bar is a scale -----
	// The panel is read top to bottom: pictures, then what picks the event
	// (search + Live, then the list tabs), then the events, then the
	// controls, the green on-air band and the position bar. That order was
	// wrong for a whole milestone — the tabs and the search box were ABOVE
	// the pictures — and not one check noticed, because every widget was
	// present and findable. Geometry is the only thing that can tell.
	//
	// The bar at the end has to be a SCALE. A take has been running for the
	// whole measurement window, so the timeline exists and the graduations
	// must be on it: a bar with none is the coloured rectangle nobody
	// recognised as a scrubber.
	{
		MultiReplayDock::LayoutProbe lp;
		runOnUi([&]() { lp = dock->layoutProbe(); });
		c.layoutOrderTopToBottom =
			lp.previewBottomY > 0 && lp.searchY >= lp.previewBottomY &&
			lp.listTabsY >= lp.searchY && lp.tableY > lp.listTabsY &&
			lp.clipBarY > lp.tableY && lp.seekY > lp.clipBarY;
		c.seekGraduations = lp.seekGraduations;
		c.seekHeight = lp.seekHeight;
		// >= 2 marks: one is an accident of rounding, two is a scale.
		c.seekbarGraduated = lp.seekEnabled && lp.seekGraduations >= 2 &&
				     lp.seekHeight >= 36;
		obs_log((c.layoutOrderTopToBottom && c.seekbarGraduated)
				? LOG_INFO
				: LOG_ERROR,
			"[selftest] dock: zones at y — pictures end %d, search %d, "
			"tabs %d, table %d, on-air band %d, position bar %d "
			"(%d px tall, %d graduations, timeline %s)",
			lp.previewBottomY, lp.searchY, lp.listTabsY, lp.tableY,
			lp.clipBarY, lp.seekY, lp.seekHeight, lp.seekGraduations,
			lp.seekEnabled ? "yes" : "NO");
	}

	// --- Mark, through the button an operator actually hits ---------------
	auto &store = EventStore::instance();
	// Point the store at the throwaway folder first: this must neither read
	// nor overwrite the operator's own events.json.
	store.setSessionFolder(tempFolder);
	store.setLiveMode(true);
	// The dock marks on ITS current angle, which it copies from the core on
	// every tick — so set it there and let one tick carry it across.
	ReplayCore::instance().setCurrentAngle(firstCam);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	runOnUi([&]() { markBtn->click(); });

	ReplayEvent ev;
	const int evId = store.lastEventId();
	const bool haveEvent = evId > 0 && store.get(evId, ev);
	if (haveEvent) {
		c.markInNs = ev.tInNs;
		c.markOutNs = ev.tOutNs;
		const int64_t oldest =
			PacketTap::instance().oldestReplayableNs(firstCam);
		const int64_t newest = PacketTap::instance().newestNs(firstCam);
		// The mark has to fall INSIDE the window the dock is drawing,
		// on the angle that was selected. A mark with no instant at all
		// — what a dead live edge produces — fails here, which is the
		// regression this exists to catch. The ring's 0 really does mean
		// empty (it only ever holds this session's packets); the event's
		// "none" is kNoInstant.
		c.markOnTimeline = ev.tInNs != kNoInstant &&
				   ev.tOutNs != kNoInstant && oldest > 0 &&
				   ev.tInNs >= oldest && ev.tOutNs <= newest &&
				   ev.tOutNs > ev.tInNs &&
				   ev.angles[firstCam].enabled;
		obs_log(LOG_INFO,
			"[selftest] dock: marked event %d in=%lld ms out=%lld ms "
			"(ring holds %lld..%lld ms, angle %d enabled: %s)",
			evId, (long long)(ev.tInNs / 1000000),
			(long long)(ev.tOutNs / 1000000),
			(long long)(oldest / 1000000),
			(long long)(newest / 1000000), firstCam + 1,
			ev.angles[firstCam].enabled ? "yes" : "NO");
	} else {
		obs_log(LOG_ERROR, "[selftest] dock: the mark button created "
				   "no event");
	}

	// --- Play it back, through the play button ----------------------------
	// The play button plays the SELECTED rows, and the table only selects the
	// fresh mark on its next poll tick - so clicking straight after the mark
	// is a race the gate loses roughly one run in three. When it loses,
	// playEvents finds no event, the dock opens a modal QMessageBox on the GUI
	// thread, and every runOnUi() after it (including the teardown) blocks
	// forever: the run produces no report at all rather than a failure.
	// So wait for the selection the dock is supposed to make.
	bool selected = false;
	for (int i = 0; i < 40 && !selected; i++) {
		runOnUi([&]() {
			for (QTableWidget *t :
			     dock->findChildren<QTableWidget *>()) {
				for (const auto &idx : t->selectionModel()
							       ->selectedRows()) {
					QTableWidgetItem *it =
						t->item(idx.row(), 0);
					if (it && it->data(Qt::UserRole).toInt() ==
							  evId)
						selected = true;
				}
			}
		});
		if (!selected)
			std::this_thread::sleep_for(
				std::chrono::milliseconds(50));
	}
	if (haveEvent && !selected)
		obs_log(LOG_ERROR,
			"[selftest] dock: the table never selected the event it "
			"just marked — 'play selected' would have nothing to play");

	// --- the id is drawn the way the reference controller draws it ----------------------------
	// Zero-padded to the configured width, so it keeps the same length for the
	// whole match and can be called out loud ("play 0142"). Read off the real
	// cell: this is a rendering claim, and the store knows nothing about it.
	if (selected) {
		const int digits = std::clamp(
			ReplayCore::instance().getConfig().eventIdDigits, 1, 8);
		QString cell;
		runOnUi([&]() {
			QTableWidget *t = dock->findChild<QTableWidget *>();
			if (!t)
				return;
			for (int r = 0; r < t->rowCount(); r++) {
				QTableWidgetItem *it = t->item(r, 0);
				if (it && it->data(Qt::UserRole).toInt() == evId)
					cell = it->text();
			}
		});
		c.idPadded = cell == QString("%1").arg(evId, digits, 10,
						       QLatin1Char('0'));
		obs_log(c.idPadded ? LOG_INFO : LOG_ERROR,
			"[selftest] dock: event %d drawn as '%s' (%d digits "
			"configured)",
			evId, cell.toUtf8().constData(), digits);
	}

	// --- the per-angle edit is IN the table, and it is per ANGLE ----------
	// Two claims, both made on real cells of the real table:
	//   1. the geometry is "fixed columns + TWO per configured camera". That
	//      is what proves the per-event speed column is gone — a count alone
	//      could be satisfied by any number of stray columns.
	//   2. typing a speed into a camera cell sets THAT ANGLE's speed in the
	//      store. Through itemChanged, exactly as an operator's keystroke.
	if (selected) {
		const Config cfg = ReplayCore::instance().getConfig();
		int configured = 0;
		for (int i = 0; i < kMaxCameras; i++)
			if (!cfg.cameras[i].sourceName.empty())
				configured++;
		const int wantCols = MultiReplayDock::kColFirstCam +
				     configured * MultiReplayDock::kColsPerCam;

		int cols = 0;
		int editedCam0 = -1;
		runOnUi([&]() {
			QTableWidget *t = dock->findChild<QTableWidget *>();
			if (!t)
				return;
			cols = t->columnCount();
			if (configured <= 0)
				return;
			for (int r = 0; r < t->rowCount(); r++) {
				QTableWidgetItem *idIt =
					t->item(r, MultiReplayDock::kColId);
				if (!idIt ||
				    idIt->data(Qt::UserRole).toInt() != evId)
					continue;
				// The first camera's cell now holds ONE widget
				// with the whole angle in it: the check, the
				// speed and the comment. So the speed is picked
				// from its drop-down, which is what an operator
				// does — no item text to type into any more.
				QWidget *cell = t->cellWidget(
					r, MultiReplayDock::kColFirstCam);
				QComboBox *sp =
					cell ? cell->findChild<QComboBox *>(
						       "mrAngleSpeed")
					     : nullptr;
				if (!sp)
					break;
				editedCam0 = firstCam;
				// Which camera the first column stands for is the
				// first configured slot, whatever its number.
				for (int i = 0; i < kMaxCameras; i++) {
					if (cfg.cameras[i].sourceName.empty())
						continue;
					editedCam0 = i;
					break;
				}
				const int idx = sp->findData(50);
				if (idx >= 0)
					sp->setCurrentIndex(idx);
				break;
			}
		});
		c.eventTableColumns = cols;
		c.angleColumnsInTable = configured > 0 && cols == wantCols;

		ReplayEvent spEv;
		double stored = -1.0;
		if (editedCam0 >= 0 && store.get(evId, spEv)) {
			stored = spEv.angles[editedCam0].speed;
			c.tableEditsAngleSpeed = std::abs(stored - 0.5) < 0.001;
			// Put it back: the gate must not leave a 50% override on
			// an event it is about to play at 1x.
			store.setAngleSpeed(evId, editedCam0 + 1, -1.0);
		}
		obs_log((c.angleColumnsInTable && c.tableEditsAngleSpeed)
				? LOG_INFO
				: LOG_ERROR,
			"[selftest] dock: event table has %d column(s) (expected %d "
			"for %d camera(s)); picking 50%% in camera %d's cell stored "
			"%.2f",
			cols, wantCols, configured, editedCam0 + 1, stored);

		// --- a search, then no search --------------------------------
		// Type something that matches nothing, let the panel settle,
		// clear it, and read the cell back. This is the gesture that was
		// reported as losing the per-angle comment and speed: the row
		// comes back and shows its placeholders while events.json still
		// holds the values. Driven through the REAL search box, because
		// the way the rows are rebuilt is exactly what is on trial.
		if (editedCam0 >= 0) {
			const std::string wantNote = "GateTag";
			const double wantSpeed = 0.75;
			store.setAngleNote(evId, editedCam0 + 1, wantNote);
			store.setAngleSpeed(evId, editedCam0 + 1, wantSpeed);
			// Settle windows: refreshEvents runs off the dock's own
			// poll, and a rebuild deferred past an open editor takes
			// one more tick to arrive.
			runOnUi([&]() { dock->setSearchText("zzzz-no-match"); });
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			runOnUi([&]() { dock->setSearchText(""); });
			std::this_thread::sleep_for(std::chrono::milliseconds(400));

			runOnUi([&]() {
				QTableWidget *t =
					dock->findChild<QTableWidget *>();
				if (!t)
					return;
				for (int r = 0; r < t->rowCount(); r++) {
					QTableWidgetItem *idIt = t->item(
						r, MultiReplayDock::kColId);
					if (!idIt ||
					    idIt->data(Qt::UserRole).toInt() !=
						    evId)
						continue;
					QWidget *cell = t->cellWidget(
						r,
						MultiReplayDock::kColFirstCam);
					if (!cell)
						break;
					if (auto *cm = cell->findChild<QComboBox *>(
						    "mrAngleNote"))
						c.searchCellNote =
							cm->currentText()
								.toStdString();
					if (auto *sp = cell->findChild<QComboBox *>(
						    "mrAngleSpeed"))
						c.searchCellPct =
							sp->currentData().toInt();
					break;
				}
			});

			ReplayEvent back;
			if (store.get(evId, back)) {
				c.searchStoredNote =
					back.angles[editedCam0].note;
				c.searchStoredPct =
					back.angles[editedCam0].speed > 0
						? (int)std::lround(
							  back.angles[editedCam0]
								  .speed *
							  100.0)
						: -1;
			}
			c.searchRestoresCellValues =
				c.searchCellNote == c.searchStoredNote &&
				c.searchCellPct == c.searchStoredPct;
			obs_log(c.searchRestoresCellValues ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: after clearing a search the cell "
				"shows note '%s' / %d%%, the store holds '%s' / %d%%",
				c.searchCellNote.c_str(), c.searchCellPct,
				c.searchStoredNote.c_str(), c.searchStoredPct);
			// Put it back the way it was found.
			store.setAngleNote(evId, editedCam0 + 1, "");
			store.setAngleSpeed(evId, editedCam0 + 1, -1.0);
		}

	}

	// Checked also that the range is servable, for the same modal reason.
	bool servable = false;
	if (c.markOnTimeline && selected) {
		std::vector<LivePacket> probe;
		int64_t pIn = 0, pOut = 0;
		servable = PacketTap::instance().resolveRange(
			firstCam, ev.tInNs, ev.tOutNs, probe, pIn, pOut);
	}
	if (servable) {
		auto &chan = ReplayChannel::instance();
		obs_source_t *src = chan.acquireSource();
		if (src) {
			// In no scene during the gate, so OBS would not consume
			// the frames we push (see the M2 pass above).
			obs_source_inc_active(src);
			obs_source_inc_showing(src);
		}

		runOnUi([&]() { playBtn->click(); });

		// 5 s of footage at 1x, generously bounded. WHILE it runs, watch
		// the green band: it has to NAME the clip on air (the padded id)
		// and its fill has to move. A status bar nobody updates looks
		// exactly like a status bar with nothing to say, which is why
		// "the widget exists" would not be worth checking.
		const QString idText =
			QString("%1").arg(evId,
					  std::clamp(ReplayCore::instance()
							     .getConfig()
							     .eventIdDigits,
						     1, 8),
					  10, QLatin1Char('0'));
		bool barNamedClip = false;
		double barProgress = 0.0;
		for (int i = 0; i < 300 && chan.playing(); i++) {
			runOnUi([&]() {
				ClipBar *bar = dock->findChild<ClipBar *>();
				if (!bar)
					return;
				if (bar->onAir() &&
				    bar->overlayText().contains(idText))
					barNamedClip = true;
				barProgress = std::max(barProgress,
						       bar->progress());
			});
			std::this_thread::sleep_for(
				std::chrono::milliseconds(50));
		}
		c.clipBarReportsOnAir = barNamedClip && barProgress > 0.05;
		obs_log(c.clipBarReportsOnAir ? LOG_INFO : LOG_ERROR,
			"[selftest] dock: on-air band named event %s: %s, fill "
			"reached %.0f%%",
			idText.toUtf8().constData(),
			barNamedClip ? "yes" : "NO", barProgress * 100.0);

		const auto st = chan.stats();
		const int64_t pos = chan.positionNs();
		c.playedFrames = (int)st.framesPushed;
		const int expected = (int)(5.0 * (canvasFps > 0 ? canvasFps : 30.0));
		c.playsMark = st.lastRunCompleted &&
			      c.playedFrames >= expected * 9 / 10;
		// The playhead the dock draws (and marks against in review) must
		// end up inside what was PLAYED, never back at 0.
		//
		// "What was played" is not the same as "what was marked any more:
		// with Config.continuePastOutMs set, the last clip of a queue runs
		// on past the OUT on purpose (and stops early where the footage
		// stops — marking live, the live edge is usually a few frames past
		// the OUT, which is why this ran 200 ms over rather than the full
		// 1.5 s). The bound follows the setting, so the check still catches
		// the failure it was written for — a playhead back at zero — without
		// calling a working feature a regression.
		const int64_t pastOutNs =
			(int64_t)ReplayCore::instance().getConfig().continuePastOutMs *
			1000000;
		c.playheadInsideClip =
			pos >= ev.tInNs && pos <= ev.tOutNs + pastOutNs;
		obs_log(LOG_INFO,
			"[selftest] dock: play button pushed %d frames, playhead "
			"at %lld ms (clip %lld..%lld ms, +%lld ms allowed past the "
			"OUT)",
			c.playedFrames, (long long)(pos / 1000000),
			(long long)(ev.tInNs / 1000000),
			(long long)(ev.tOutNs / 1000000),
			(long long)(pastOutNs / 1000000));

		if (src) {
			obs_source_dec_showing(src);
			obs_source_dec_active(src);
			obs_source_release(src);
		}
	} else if (c.markOnTimeline) {
		obs_log(LOG_ERROR, "[selftest] dock: the marked range is not "
				   "servable — not clicking play");
	}

	// --- Any combination of enabled angles, on the same event -------------
	// Enabling C1 and C2 on the same mark is a request to SEE both, one after
	// the other. The queue used to hold a single clip whatever was enabled -
	// every replay in the log read "[1/1]" - and no check outside a live
	// session ever noticed, because nothing looked at the queue.
	//
	// A count is not enough: [1,2] and [2,2] are both "two clips". These
	// assert the exact angle SEQUENCE, for the combinations an operator
	// actually produces - both cameras, one camera that is not the first, and
	// a set with a hole in it (angle 1 and angle 3 on a two-camera rig, where
	// nothing is wired to 3).
	if (!haveEvent) {
		// nothing to queue: the mark check above already failed loudly
	} else if (secondCam < 0) {
		// One camera: nothing about combinations — nor about what happens
		// BETWEEN two clips — can be proven here.
		c.multiAngleQueue = true;
		c.angleCombinations = true;
		c.angleChoiceRepeatable = true;
		c.singleNonFirstAnglePlays = true;
		c.queueAdvancesToSecond = true;
		c.previewHoldsSequence = true;
		c.followsLiveAfterSequence = true;
		c.singleHonoursRequestedAngle = true;
		c.singleReportsUnplayableAngle = true;
		c.markInheritsAngle = true;
		// One camera: there is never a second queue item to skip TO.
		c.skipAdvancesQueue = true;
	} else {
		auto &pc = PlaybackCoordinator::instance();
		const int a1 = firstCam + 1;
		const int a2 = secondCam + 1;
		// A slot nothing is wired to. On the operator's two-camera rig
		// this IS his "angle 3": enabling it must not shift, drop or
		// reorder the angles that can actually play.
		const int hole = kEventAngles;

		// Set the enabled set EXACTLY, queue it, and read back the shape
		// of the queue. Requesting `reqCam` as the current angle at the
		// same time proves AllEnabled ignores it whenever the event says
		// something.
		const auto queueFor = [&](const char *what,
					  const std::vector<int> &enable,
					  int reqCam,
					  const std::vector<int> &expect) {
			for (int a = 1; a <= kEventAngles; a++)
				store.setAngle(evId, a, false);
			for (int a : enable)
				store.setAngle(evId, a, true);
			std::string qerr;
			const bool started =
				pc.playEvents({evId}, reqCam, false, qerr);
			const auto ps = pc.playState();
			const bool ok = started && ps.queuedAngles == expect;
			std::string got, want;
			for (int a : ps.queuedAngles)
				got += std::to_string(a) + " ";
			for (int a : expect)
				want += std::to_string(a) + " ";
			obs_log(ok ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: %s — queued [%s] expected [%s]%s%s",
				what, got.c_str(), want.c_str(),
				started ? "" : " — playEvents refused: ",
				started ? "" : qerr.c_str());
			return ok;
		};

		// Both angles, in angle order, whichever one the operator is on.
		const bool both = queueFor("both angles", {a1, a2}, firstCam,
					   {a1, a2});
		const bool bothFromSecond = queueFor("both angles, on the second",
						     {a1, a2}, secondCam,
						     {a1, a2});
		c.queuedClips = pc.playState().queued;
		// Non-contiguous: the enabled set has a hole in it.
		const bool gapLow = queueFor("angles 1 and a hole", {a1, hole},
					     firstCam, {a1});
		const bool gapHigh = queueFor("angles 2 and a hole", {a2, hole},
					      secondCam, {a2});
		// One angle only, and NOT the first one, requested from the
		// first: the event decides, not the button.
		const bool onlySecond = queueFor("only the second angle", {a2},
						 firstCam, {a2});
		c.angleCombinations = both && bothFromSecond && gapLow &&
				      gapHigh && onlySecond;
		c.multiAngleQueue = both;

		// The single non-first angle must not just queue - it must run.
		auto &chan = ReplayChannel::instance();
		bool ranSecond = false;
		if (onlySecond) {
			std::string qerr;
			if (pc.playEvents({evId}, firstCam, false, qerr)) {
				for (int i = 0; i < 30 &&
						chan.stats().framesPushed == 0;
				     i++)
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				const auto ps = pc.playState();
				ranSecond = ps.angle1 == a2 &&
					    chan.stats().framesPushed > 0;
				obs_log(ranSecond ? LOG_INFO : LOG_ERROR,
					"[selftest] dock: only-angle-%d playback — "
					"on air angle %d, %llu frame(s)",
					a2, ps.angle1,
					(unsigned long long)
						chan.stats().framesPushed);
			}
			pc.stopEvents();
		}
		c.singleNonFirstAnglePlays = ranSecond;

		// The SAME event, played three times with a different choice each
		// time: one angle, then both, then one again. Each play must see
		// the current enabled set and nothing of the previous queue.
		const bool r1 = queueFor("repeat 1/3: only the second", {a2},
					 firstCam, {a2});
		const bool r2 = queueFor("repeat 2/3: both", {a1, a2}, firstCam,
					 {a1, a2});
		const bool r3 = queueFor("repeat 3/3: only the first", {a1},
					 secondCam, {a1});
		c.angleChoiceRepeatable = r1 && r2 && r3;
		pc.stopEvents();

		// --- and it must WALK the queue, not just hold it -------------
		// Clip 1 ends on the playback thread, which posts the advance
		// onto the OBS UI queue, filtered by a generation counter. If any
		// link in that chain drops the callback the operator sees one
		// angle and the sequence stops - silently, because the queue
		// still reads "2 clips". So let it run for real.
		if (both) {
			for (int a = 1; a <= kEventAngles; a++)
				store.setAngle(evId, a, false);
			store.setAngle(evId, a1, true);
			store.setAngle(evId, a2, true);
			std::string qerr;
			if (pc.playEvents({evId}, firstCam, false, qerr)) {
				// Watch the WHOLE sequence, sampling faster than
				// the dock's own 33 ms tick: two 5 s clips plus
				// the hop, capped at 20 s.
				bool reachedSecond = false;
				bool secondRuns = false;
				uint64_t framesAtHop = 0;
				int previewSamples = 0, previewLive = 0;
				for (int i = 0; i < 1000; i++) {
					const auto ps = pc.playState();
					if (!reachedSecond && ps.queuePos == 2 &&
					    ps.angle1 == a2) {
						reachedSecond = true;
						framesAtHop = chan.stats()
								      .framesPushed;
					}
					if (reachedSecond &&
					    chan.stats().framesPushed > 0)
						secondRuns = true;
					// Only once something is really on air: the
					// very first tick is legitimately still on
					// the live camera.
					if (chan.stats().framesPushed > 0 ||
					    reachedSecond) {
						previewSamples++;
						if (!dock->previewShowsReplay())
							previewLive++;
					}
					if (!ps.active)
						break; // sequence finished
					std::this_thread::sleep_for(
						std::chrono::milliseconds(20));
				}
				c.queueAdvancesToSecond = reachedSecond &&
							  secondRuns;
				c.previewLiveSamples = previewLive;
				// Not "mostly": a single frame of the wrong camera
				// between two angles is a flash the operator sees,
				// and puts on air with "to output" on.
				c.previewHoldsSequence =
					previewSamples > 0 && previewLive == 0;
				obs_log(c.queueAdvancesToSecond ? LOG_INFO
								: LOG_ERROR,
					"[selftest] dock: two-angle sequence — "
					"reached clip 2 on angle %d: %s, frames "
					"at the hop %llu, frames after %llu",
					a2, reachedSecond ? "yes" : "NO",
					(unsigned long long)framesAtHop,
					(unsigned long long)
						chan.stats().framesPushed);
				obs_log(c.previewHoldsSequence ? LOG_INFO
							       : LOG_ERROR,
					"[selftest] dock: preview stayed on the "
					"replay for %d/%d samples of the sequence",
					previewSamples - previewLive,
					previewSamples);

				// The queue drained by itself: within a second the
				// dock must have handed the transport back to the
				// live edge (the tap is still attached, so there IS
				// a live edge to go back to).
				for (int i = 0; i < 50; i++) {
					if (ReplayCore::instance().followLive()) {
						c.followsLiveAfterSequence = true;
						break;
					}
					std::this_thread::sleep_for(
						std::chrono::milliseconds(20));
				}
				obs_log(c.followsLiveAfterSequence ? LOG_INFO
								   : LOG_ERROR,
					"[selftest] dock: back to the live edge after "
					"the sequence: %s",
					c.followsLiveAfterSequence ? "yes" : "NO");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] dock: two-angle sequence — "
					"playEvents refused: %s", qerr.c_str());
			}
			pc.stopEvents();
		}

		// --- >> drops the clip and takes the next queue item ----------
		// The operator who has seen enough of an angle should not have to
		// sit through the rest of it. Clicked through the real key, and
		// checked on the queue: the position has to MOVE, onto the second
		// angle, and the sequence has to still be alive afterwards (Stop
		// would also "leave clip 1" — and that is the bug this would be).
		{
			QPushButton *skipBtn = nullptr;
			const QString skip = QStringLiteral("skipNext");
			runOnUi([&]() {
				for (QPushButton *b :
				     dock->findChildren<QPushButton *>())
					if (b->property(kKeyProperty).toString() == skip)
						skipBtn = b;
			});
			for (int a = 1; a <= kEventAngles; a++)
				store.setAngle(evId, a, false);
			store.setAngle(evId, a1, true);
			store.setAngle(evId, a2, true);
			std::string kerr;
			if (skipBtn && pc.playEvents({evId}, firstCam, false, kerr)) {
				// Let clip 1 really get going first, or "it is on
				// clip 2" would be indistinguishable from a queue
				// that had already walked on by itself.
				for (int i = 0; i < 40 &&
						chan.stats().framesPushed == 0;
				     i++)
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				const auto before = pc.playState();
				runOnUi([&]() { skipBtn->click(); });
				// The skip crosses the UI task queue and then has
				// to start a clip, so this waits for the queue to
				// MOVE rather than for it to move quickly — the
				// claim is "a skip is not a stop", not a latency
				// figure. Two seconds was not enough on a loaded
				// machine and failed a queue that did advance; the
				// time it took is logged so "slow" and "stuck"
				// stay distinguishable.
				PlaybackCoordinator::PlayState after;
				int waitedMs = 0;
				for (int i = 0; i < 80; i++) {
					after = pc.playState();
					if (after.active && after.queuePos == 2)
						break;
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
					waitedMs += 50;
				}
				c.skipAdvancesQueue = before.queuePos == 1 &&
						      after.active &&
						      after.queuePos == 2 &&
						      after.angle1 == a2;
				obs_log(c.skipAdvancesQueue ? LOG_INFO : LOG_ERROR,
					"[selftest] dock: >> moved the queue from "
					"%d/%d (angle %d) to %d/%d (angle %d) in "
					"%d ms, still active: %s",
					before.queuePos, before.queued,
					before.angle1, after.queuePos,
					after.queued, after.angle1, waitedMs,
					after.active ? "yes" : "NO");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] dock: no >> key found (%p) or "
					"the two-angle queue refused: %s",
					(void *)skipBtn, kerr.c_str());
			}
			pc.stopEvents();
		}

		// --- pause holds the frame, and play carries on FROM IT -------
		// The pause key used to stop the queue, so the next press re-cued
		// the event and played it again from the IN: the operator paused on
		// the moment he wanted and then lost it. What is checked is the
		// thing that was wrong — where the playhead is when it resumes —
		// not merely that something is playing again.
		{
			std::string perr;
			if (pc.playEvents({evId}, firstCam, false, perr)) {
				for (int i = 0; i < 40 && chan.stats().framesPushed < 3;
				     i++)
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				pc.setPaused(true);
				std::this_thread::sleep_for(
					std::chrono::milliseconds(200));
				const int64_t held = chan.positionNs();
				// Frozen: half a second of wall time must not move
				// the picture on.
				std::this_thread::sleep_for(
					std::chrono::milliseconds(500));
				const int64_t stillHeld = chan.positionNs();
				pc.setPaused(false);
				std::this_thread::sleep_for(
					std::chrono::milliseconds(300));
				const int64_t after = chan.positionNs();
				c.pauseHoldsAndResumes =
					held > 0 && stillHeld == held &&
					after > held &&
					// ...and it did NOT start again from the
					// top, which is the actual bug: a restart
					// would land at or before where the pause
					// was, not after it.
					after - held < 2'000'000'000LL;
				obs_log(c.pauseHoldsAndResumes ? LOG_INFO : LOG_ERROR,
					"[selftest] dock: pause held %lld ms for "
					"500 ms (still %lld ms), resumed to %lld ms",
					(long long)(held / 1000000),
					(long long)(stillHeld / 1000000),
					(long long)(after / 1000000));
			}
			pc.stopEvents();
		}

		// --- a speed change re-spaces the clip, it does not restart it -
		// Same shape of bug and the same kind of check: what matters is
		// that the picture the operator was looking at is still the picture
		// he is looking at, one dial-turn later.
		{
			std::string perr;
			if (pc.playEvents({evId}, firstCam, false, perr)) {
				for (int i = 0; i < 40 && chan.stats().framesPushed < 3;
				     i++)
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				const int64_t before = chan.positionNs();
				const bool took = pc.setLiveSpeedPct(25);
				std::this_thread::sleep_for(
					std::chrono::milliseconds(200));
				const int64_t after = chan.positionNs();
				// Forward from where it was, and slowly: at 25% two
				// tenths of a second is ~50 ms of footage, so a
				// restart (which would go backwards) and a runaway
				// are both excluded.
				c.speedChangeKeepsPosition =
					took && before > 0 && after >= before &&
					after - before < 500'000'000LL;
				obs_log(c.speedChangeKeepsPosition ? LOG_INFO
								   : LOG_ERROR,
					"[selftest] dock: 100%%→25%% mid-clip moved "
					"the playhead %lld ms → %lld ms (accepted: "
					"%s)",
					(long long)(before / 1000000),
					(long long)(after / 1000000),
					took ? "yes" : "NO");
				pc.setLiveSpeedPct(100);
			}
			pc.stopEvents();
		}

		// --- choosing a row LOADS it ----------------------------------
		// Selecting an event used to do nothing until Play was pressed, so
		// the operator picked clips he could not see. Driven through the
		// table's own selection so the check goes through the same signal a
		// mouse does — and asserted on the frame that reached the source,
		// because "a cue was requested" and "a picture arrived" are not the
		// same claim.
		{
			pc.stopEvents();
			chan.stop();
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			ReplayEvent cueEv;
			const bool haveEv = store.get(evId, cueEv);
			int row = -1;
			runOnUi([&]() {
				QTableWidget *t = dock->findChild<QTableWidget *>();
				if (!t)
					return;
				t->clearSelection();
				for (int r = 0; r < t->rowCount(); r++) {
					QTableWidgetItem *it = t->item(r, 0);
					if (it && it->data(Qt::UserRole).toInt() ==
							  evId) {
						row = r;
						t->selectRow(r);
						break;
					}
				}
			});
			int64_t landed = 0;
			for (int i = 0; i < 40; i++) {
				if (chan.stats().framesPushed > 0) {
					landed = chan.stats().firstFrameNs;
					break;
				}
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			// On the event's IN, within a frame: a cue that landed
			// somewhere else is a cue of the wrong moment.
			const int64_t frameNs =
				(int64_t)(1e9 / (canvasFps > 0 ? canvasFps : 30.0));
			c.selectionCuesEvent =
				row >= 0 && haveEv && landed > 0 &&
				std::llabs(landed - cueEv.tInNs) <= frameNs;
			obs_log(c.selectionCuesEvent ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: selecting row %d cued %lld ms "
				"(event IN %lld ms)",
				row, (long long)(landed / 1000000),
				(long long)(haveEv ? cueEv.tInNs / 1000000 : 0));
			chan.stop();

			// --- ...AND THE BOXES ARRIVE WITH THE BAY -------------
			// Reported from a real panel: on a cue the bay and the
			// angle boxes were not symmetric, one arriving noticeably
			// after the other.
			//
			// The cause was serial: a fetch that is not in the ring
			// is an open, a seek and a demux, and play() does it
			// INLINE. Bay then feed then feed is three of them in a
			// row, so somebody is a third of a second late by
			// construction — and which somebody was decided only by
			// call order. The feeds' reads now go out first, in
			// parallel, and run under the bay's own.
			//
			// Timed on a COLD event, because that is the only kind
			// that has the fetch in it: cueing the same row twice
			// hits every cache and would pass with any ordering at
			// all. So this marks one of its own, times the cue, and
			// takes it away again.
			// ...AND SEVERAL TIMES, because the complaint that came
			// back from the panel next was that the wait itself is
			// INTERMITTENT — sometimes instant on both, sometimes a
			// noticeable pause. A single sample cannot say that. Each
			// pass marks its own event at a different instant, so no
			// two share a cache entry, and the SPREAD is what gets
			// reported: a mean says nothing about a panel that is
			// quick four times and slow the fifth.
			int bayMin = INT_MAX, bayMax = -1;
			int tileMin = INT_MAX, tileMax = -1;
			int gapMax = -1;
			for (int pass = 0; pass < 4; pass++) {
				const int64_t coldNewest =
					PacketTap::instance().newestNs(firstCam);
				const int coldId =
					coldNewest > 0
						? store.markInOut(
							  coldNewest -
								  (6 + pass * 2) *
									  1'000'000'000LL,
							  2, firstCam)
						: 0;
				int coldRow = -1;
				if (coldId > 0) {
					// Let the table rebuild carry the new row
					// in before trying to select it.
					std::this_thread::sleep_for(
						std::chrono::milliseconds(300));
					runOnUi([&]() {
						QTableWidget *t =
							dock->findChild<
								QTableWidget *>();
						if (!t)
							return;
						t->clearSelection();
						for (int r = 0; r < t->rowCount();
						     r++) {
							QTableWidgetItem *it =
								t->item(r, 0);
							if (it &&
							    it->data(Qt::UserRole)
									    .toInt() ==
								    coldId) {
								coldRow = r;
								t->selectRow(r);
								break;
							}
						}
					});
				}
				int bayMs = -1, tilesMs = -1, feeds = 0;
				for (int i = 0; i < 400; i++) {
					const auto mv = dock->multiviewState();
					feeds = std::max(feeds, mv.feeds);
					if (bayMs < 0 &&
					    chan.stats().framesPushed > 0)
						bayMs = i * 5;
					if (tilesMs < 0 && mv.feeds > 0 &&
					    mv.feedsWithCurrentClip == mv.feeds)
						tilesMs = i * 5;
					if (bayMs >= 0 && tilesMs >= 0)
						break;
					std::this_thread::sleep_for(
						std::chrono::milliseconds(5));
				}
				const int gap = (bayMs >= 0 && tilesMs >= 0)
							? std::abs(tilesMs - bayMs)
							: -1;
				if (bayMs >= 0) {
					bayMin = std::min(bayMin, bayMs);
					bayMax = std::max(bayMax, bayMs);
				}
				if (tilesMs >= 0) {
					tileMin = std::min(tileMin, tilesMs);
					tileMax = std::max(tileMax, tilesMs);
				}
				gapMax = std::max(gapMax, gap);
				// ...and WHY it took that long. framesPreroll is
				// how many pictures the decoder had to run
				// through to reach the marked one — the distance
				// from the keyframe before it. With keyint_sec=1
				// that is 0..30 frames depending on where the
				// mark happened to fall inside the GOP, and it is
				// the one thing here that varies per cue while
				// everything else is constant. If the wait tracks
				// it, the wait is decode, not I/O.
				const auto cueSt = chan.stats();
				obs_log(LOG_INFO,
					"[selftest] dock: cold cue %d — bay had its "
					"picture at %d ms, %d box(es) at %d ms, "
					"gap %d ms, preroll %llu frame(s)",
					pass + 1, bayMs, feeds, tilesMs, gap,
					(unsigned long long)cueSt.framesPreroll);
				if (coldId > 0)
					store.remove(coldId);
				chan.stop();
				std::this_thread::sleep_for(
					std::chrono::milliseconds(200));
				runOnUi([&]() {
					QTableWidget *t =
						dock->findChild<QTableWidget *>();
					if (t) {
						t->clearSelection();
						if (row >= 0 && row < t->rowCount())
							t->selectRow(row);
					}
				});
			}
			c.multiviewCueGapMs = gapMax;
			c.cueBayMinMs = bayMax >= 0 ? bayMin : -1;
			c.cueBayMaxMs = bayMax;
			c.cueTileMinMs = tileMax >= 0 ? tileMin : -1;
			c.cueTileMaxMs = tileMax;
			obs_log(LOG_INFO,
				"[selftest] dock: cue latency over 4 cold cues — "
				"bay %d..%d ms, boxes %d..%d ms, worst gap %d ms",
				c.cueBayMinMs, bayMax, c.cueTileMinMs, tileMax,
				gapMax);

			// --- ...AND THE BOXES DO NOT GO DARK WHILE IT DOES ----
			// Reported from a real panel: the bay and the angle boxes
			// were not symmetric on a cue, one arriving noticeably
			// after the other. The cause was a per-clip gate on the
			// tile publish — hasPosition() is framesPushed > 0 and
			// play() zeroes the stats on every clip, so each cue
			// WITHDREW every tile's source and the next 4 Hz beat put
			// it back, about a quarter of a second later. The bay
			// never did that: its own gate is about the tap, not the
			// clip.
			//
			// Nothing already in this file could see it. The feeds
			// were alive, decoding and correct the whole time; only
			// what was PUBLISHED for the graphics thread went away,
			// and only for a moment. So this cues again, on a panel
			// whose feeds are already warm, and samples fast enough
			// to catch a gap that short.
			if (row >= 0) {
				MultiReplayDock::MultiviewState warm;
				for (int i = 0; i < 60; i++) {
					warm = dock->multiviewState();
					if (warm.tilesPublished > 0 &&
					    warm.feedsWithPicture == warm.feeds)
						break;
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				}
				// Re-cue the same row: clearing and re-selecting
				// is a selection change, which is the operator's
				// gesture and the one that used to blank them.
				runOnUi([&]() {
					QTableWidget *t =
						dock->findChild<QTableWidget *>();
					if (!t)
						return;
					t->clearSelection();
					t->selectRow(row);
				});
				int darkMs = 0, worstPublished = warm.tilesPublished;
				for (int i = 0; i < 60; i++) {
					const auto mv = dock->multiviewState();
					if (mv.tilesPublished < warm.tilesPublished) {
						darkMs += 10;
						worstPublished = std::min(
							worstPublished,
							mv.tilesPublished);
					}
					std::this_thread::sleep_for(
						std::chrono::milliseconds(10));
				}
				// Zero is what the fix produces — the publish no
				// longer depends on the clip at all. The margin is
				// for the sampler, not for the behaviour: the bug
				// this replaces was ~264 ms, so nothing about it
				// could hide under this.
				const bool keptPicture =
					warm.tilesPublished > 0 && darkMs <= 50;
				obs_log(keptPicture ? LOG_INFO
								   : LOG_ERROR,
					"[selftest] dock: re-cue with %d tile(s) "
					"published — dark for %d ms, worst %d "
					"published",
					warm.tilesPublished, darkMs,
					worstPublished);
				c.multiviewCueDarkMs = darkMs;
				chan.stop();
			}
		}

		// --- one bay or two, and the panel says which ------------------
		// The second bay is OPTIONAL now, and off is the default: on a
		// single-bay rig the B box, the A|B/A/B selector and the swap key
		// are absent rather than greyed out, and every command means A.
		// Both states are driven here — off first (nobody would notice a
		// default that quietly showed two bays), then on, which is what the
		// channel-B checks below need.
		// Read-only ON PURPOSE. Proving it by flipping the setting would mean
		// calling setConfig() in the middle of the take, and setConfig()
		// re-points the segment index — a gate that disturbs the anchoring it
		// is about to check is a gate that fails for its own reasons. The
		// self-test's setup turns B on before recording starts, so what is
		// asserted here is that the FLAG and the WIDGETS agree, in whichever
		// state the run is configured for.
		{
			const bool want =
				ReplayCore::instance().getConfig().enableChannelB;
			bool selectorShown = false;
			runOnUi([&]() {
				for (QPushButton *b :
				     dock->findChildren<QPushButton *>())
					if (b->property(kKeyProperty).toString() ==
						    QStringLiteral("bay2") &&
					    b->isVisibleTo(dock))
						selectorShown = true;
			});
			const bool boxShown = dock->layoutProbe().channelBVisible;
			c.channelBIsOptional =
				boxShown == want && selectorShown == want;
			obs_log(c.channelBIsOptional ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: channel B configured %s — box %s, "
				"A|B selector %s",
				want ? "on" : "off", boxShown ? "shown" : "hidden",
				selectorShown ? "shown" : "hidden");
		}

		// --- nothing is still held when OBS clears scene data ---------
		// OBS clears scene data on the way out of a collection and on the
		// way out of the program, and anything still referenced then is
		// reported to the OPERATOR as a plugin that failed to release its
		// resources — in a dialog, on shutdown. It happened: the dock's
		// destructor released A's preview and the multiview tiles but never
		// B's preview, and the destructor runs long after the clearing
		// anyway, so the cleanup path released nothing at all. OBS named
		// "MultiReplay - Replay B", then "C1" and "C2" (the tiles), which
		// are the operator's own cameras.
		//
		// Driven through the same static the module's frontend handler
		// calls, then counted — a log line saying "released" is not the
		// claim; zero references held is.
		{
			const int before = dock->heldSourceRefs();
			runOnUi([&]() { MultiReplayDock::releasePreviewRefs(); });
			const int after = dock->heldSourceRefs();
			c.releasesSourcesOnCleanup = after == 0;
			obs_log(c.releasesSourcesOnCleanup ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: held %d source ref(s), %d after the "
				"scene-data cleanup hook",
				before, after);
			// Put them back for the checks below. The dock's own timer
			// re-resolves them on its slow beat (4 Hz), so a wait is all
			// this needs — and it keeps the check out of poll(), which is
			// the dock's business and not the gate's.
			std::this_thread::sleep_for(std::chrono::milliseconds(600));
		}

		// --- the angle keys are the CAMERAS, by name ------------------
		// ── ONE PICTURE PER CONFIGURED CAMERA, AND IT IS THE ANGLE KEY ──
		//
		// The camera matrix — two rows of eight keys — is gone. It was the
		// widest thing on the panel and the single reason it could not be
		// docked down the side of an OBS window; the angle is chosen by
		// clicking the PICTURE now, which is where the operator is already
		// looking and which no key can do better ("C5" against what C5 is
		// pointing at).
		//
		// So what has to be true is about the TILES: one per configured
		// camera, on screen, each with a live display behind it and each
		// NAMED. A black rectangle with no caption is a camera the operator
		// has to identify by remembering where it is.
		{
			const Config kc = ReplayCore::instance().getConfig();
			int configured = 0;
			for (int i = 0; i < kEventAngles && i < kMaxCameras; i++)
				if (!kc.cameras[i].sourceName.empty())
					configured = i + 1;
			int visible = 0, named = 0, clickable = 0;
			runOnUi([&]() {
				// By objectName rather than by class: AspectBox is a
				// plain QWidget subclass with no Q_OBJECT, and giving
				// it one would put a moc'd type in a header the
				// mockup also compiles.
				for (QWidget *box :
				     dock->findChildren<QWidget *>()) {
					if (box->objectName() !=
					    QStringLiteral("mrTile"))
						continue;
					if (!box->isVisibleTo(dock))
						continue;
					visible++;
					if (box->cursor().shape() ==
					    Qt::PointingHandCursor)
						clickable++;
					for (QLabel *l :
					     box->findChildren<QLabel *>())
						if (l->objectName() ==
							    QStringLiteral(
								    "mrTileCap") &&
						    !l->text().isEmpty())
							named++;
				}
			});
			c.angleKeysFollowCameras = configured > 0 &&
						   visible == configured &&
						   named == visible &&
						   clickable == visible;
			c.visibleAngleKeys = visible;
			obs_log(c.angleKeysFollowCameras ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: %d camera tile(s) for %d configured "
				"camera(s), %d named, %d clickable",
				visible, configured, named, clickable);
		}

		// --- the green band spans the SEQUENCE ------------------------
		// With two angles queued the fill used to be the progress through
		// the clip on air, so it filled up and started again twice and
		// nothing said when the replay would be over. Now it spans the
		// queue and draws the join between the clips.
		{
			for (int a = 1; a <= kEventAngles; a++)
				store.setAngle(evId, a, false);
			store.setAngle(evId, a1, true);
			store.setAngle(evId, a2, true);
			std::string berr;
			if (pc.playEvents({evId}, firstCam, false, berr)) {
				for (int i = 0; i < 40 && chan.stats().framesPushed < 3;
				     i++)
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				size_t joins = 0;
				double frac = 1.0;
				runOnUi([&]() {
					if (ClipBar *cb =
						    dock->findChild<ClipBar *>()) {
						joins = cb->clipJoinCount();
						frac = cb->progress();
					}
				});
				// One join for two clips, and the fill is still in
				// the first half: scaled to one clip it would
				// already be most of the way across.
				c.clipBarSpansSequence = joins == 1 && frac < 0.5;
				obs_log(c.clipBarSpansSequence ? LOG_INFO : LOG_ERROR,
					"[selftest] dock: green band shows %d join(s) "
					"for a 2-clip queue, fill %.3f",
					(int)joins, frac);
			}
			pc.stopEvents();
		}

		// --- CHANNEL B: it exists, it plays, and it is not A ----------
		// Everything above drives A. A second channel that is never
		// played is a second channel that is broken with nobody the
		// wiser — so this plays the same event on B THROUGH THE PANEL
		// (the A|B selector, then play) and demands frames out of B's
		// own engine while A stays exactly where it was left.
		{
			auto &chB = ReplayChannel::instance(Which::B);
			auto &pcB = PlaybackCoordinator::instance(Which::B);
			const uint64_t aBefore = chan.stats().framesPushed;
			QPushButton *selB = nullptr;
			runOnUi([&]() {
				for (QPushButton *b :
				     dock->findChildren<QPushButton *>())
					if (b->property(kKeyProperty).toString() ==
					    QStringLiteral("bay1"))
						selB = b;
				if (selB)
					selB->click(); // the keys now drive B
			});
			std::string berr;
			bool startedB = false;
			if (selB) {
				for (int a = 1; a <= kEventAngles; a++)
					store.setAngle(evId, a, a == a1);
				runOnUi([&]() {
					startedB = pcB.playEvents(
						{evId}, firstCam, false, berr,
						PlaybackCoordinator::AngleMode::
							Single);
				});
			}
			for (int i = 0; i < 60 && chB.stats().framesPushed == 0; i++)
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			c.channelBFrames = (int)chB.stats().framesPushed;
			// A must NOT have moved: two channels sharing a worker
			// are one channel with extra buttons.
			c.channelBIndependent =
				c.channelBFrames > 0 &&
				chan.stats().framesPushed == aBefore;
			obs_log(c.channelBIndependent ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: channel B played %d frame(s) "
				"(started=%s), A untouched at %llu: %s",
				c.channelBFrames, startedB ? "yes" : "no",
				(unsigned long long)aBefore,
				c.channelBIndependent ? "yes" : "NO");

			// --- ⇄ hands the clip across ----------------------------
			// B is playing and A is idle, so after the swap the clip
			// has to be on A. That is the whole point of the key.
			QPushButton *swapBtn = nullptr;
			runOnUi([&]() {
				for (QPushButton *b :
				     dock->findChildren<QPushButton *>())
					if (b->property(kKeyProperty).toString() ==
					    QStringLiteral("swapBays"))
						swapBtn = b;
				if (swapBtn)
					swapBtn->click();
			});
			for (int i = 0; i < 60; i++) {
				if (pc.playState().active &&
				    pc.playState().eventId == evId)
					break;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			c.swapMovesClip = swapBtn && pc.playState().active &&
					  pc.playState().eventId == evId;
			obs_log(c.swapMovesClip ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: ⇄ moved event %d from B to A: %s",
				evId, c.swapMovesClip ? "yes" : "NO");

			pcB.stopEvents();
			pc.stopEvents();
			// Hand the panel back to A, or every check after this one
			// would be driving a channel it does not know about.
			runOnUi([&]() {
				for (QPushButton *b :
				     dock->findChildren<QPushButton *>())
					if (b->property(kKeyProperty).toString() ==
					    QStringLiteral("bay0"))
						b->click();
			});
		}

		// --- FIRST RUN: the wizard must know it is not wanted ---------
		// The harness has a session folder and armed cameras, which is a
		// configured rig by any reading — so needsSetup() must say no. It
		// fails loudly if the question is ever inverted or widened, and
		// what that would cost is a modal dialog in front of an operator
		// every single launch, on a panel that has been working for
		// months. The dialogs themselves are suppressed while a self-test
		// drives OBS (see selfTestIsDriving), because a modal exec() on
		// this thread would park the whole run.
		runOnUi([&]() {
			c.setupNotNeededWhenConfigured = !dock->needsSetup();
		});
		obs_log(c.setupNotNeededWhenConfigured ? LOG_INFO : LOG_ERROR,
			"[selftest] dock: guided setup considers itself %s",
			c.setupNotNeededWhenConfigured ? "not needed (right)"
						       : "NEEDED on a configured rig");

		// --- the trim keys move the point they say they move ----------
		// ⇤IN with the playhead parked one second later must move the
		// event's IN by about a second — and not touch its OUT. A trim
		// that quietly moved the wrong end would be found on air.
		{
			ReplayEvent before;
			store.get(evId, before);
			bool clicked = false;
			runOnUi([&]() {
				QTableWidget *t = dock->findChild<QTableWidget *>();
				if (t) {
					for (int r = 0; r < t->rowCount(); r++) {
						QTableWidgetItem *idIt = t->item(
							r, MultiReplayDock::kColId);
						if (idIt &&
						    idIt->data(Qt::UserRole).toInt() ==
							    evId) {
							t->setCurrentCell(
								r,
								MultiReplayDock::kColId,
								QItemSelectionModel::
										ClearAndSelect |
									QItemSelectionModel::
										Rows);
							break;
						}
					}
				}
				for (QPushButton *b :
				     dock->findChildren<QPushButton *>())
					if (b->property(kKeyProperty).toString() ==
					    QStringLiteral("trimIn")) {
						b->click();
						clicked = true;
					}
			});
			ReplayEvent after;
			store.get(evId, after);
			c.trimDeltaMs = (after.tInNs - before.tInNs) / 1'000'000;
			// The playhead is wherever the checks above left it, so the
			// claim is "IN moved to the playhead and OUT did not
			// move", not a particular number of milliseconds.
			c.trimMovedIn = clicked && after.tOutNs == before.tOutNs &&
					after.tInNs != before.tInNs;
			obs_log(c.trimMovedIn ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: ⇤IN moved event %d's IN by %lld ms "
				"(OUT %s)",
				evId, (long long)c.trimDeltaMs,
				after.tOutNs == before.tOutNs ? "unchanged"
							      : "MOVED TOO");
			// Put it back: the checks after this one play this event.
			store.movePoint(evId, true, before.tInNs - after.tInNs);
		}

		// --- an event edge can be dragged on the bar ------------------
		// The operator grabs the IN of a marker and pulls it: no
		// selection, no keys. Driven as real mouse events, at the pixel
		// the bar itself says that edge is on.
		{
			ReplayEvent before;
			store.get(evId, before);
			bool sent = false;
			runOnUi([&]() {
				SeekBar *bar = dock->findChild<SeekBar *>();
				if (!bar || bar->markerCount() == 0)
					return;
				const auto mk = bar->markerAt(0);
				const int x0 = bar->xForFraction(mk.first);
				const int x1 = std::min(bar->width() - 3, x0 + 12);
				const int y = bar->height() / 2;
				QMouseEvent press(QEvent::MouseButtonPress,
						  QPointF(x0, y),
						  bar->mapToGlobal(QPoint(x0, y)),
						  Qt::LeftButton, Qt::LeftButton,
						  Qt::NoModifier);
				QMouseEvent move(QEvent::MouseMove, QPointF(x1, y),
						 bar->mapToGlobal(QPoint(x1, y)),
						 Qt::NoButton, Qt::LeftButton,
						 Qt::NoModifier);
				QMouseEvent rel(QEvent::MouseButtonRelease,
						QPointF(x1, y),
						bar->mapToGlobal(QPoint(x1, y)),
						Qt::LeftButton, Qt::NoButton,
						Qt::NoModifier);
				QCoreApplication::sendEvent(bar, &press);
				QCoreApplication::sendEvent(bar, &move);
				QCoreApplication::sendEvent(bar, &rel);
				sent = true;
			});
			ReplayEvent after;
			store.get(evId, after);
			c.dragMovesMarker = sent && after.tInNs > before.tInNs &&
					    after.tOutNs == before.tOutNs;
			obs_log(c.dragMovesMarker ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: dragging the IN edge moved it by "
				"%lld ms (OUT %s)",
				(long long)((after.tInNs - before.tInNs) / 1000000),
				after.tOutNs == before.tOutNs ? "unchanged"
							      : "MOVED TOO");
			store.movePoint(evId, true, before.tInNs - after.tInNs);
		}

		// --- and the same point, by SECONDS, from a key ---------------
		{
			ReplayEvent before;
			store.get(evId, before);
			runOnUi([&]() {
				QTableWidget *t = dock->findChild<QTableWidget *>();
				if (!t)
					return;
				for (int r = 0; r < t->rowCount(); r++) {
					QTableWidgetItem *idIt =
						t->item(r, MultiReplayDock::kColId);
					if (idIt &&
					    idIt->data(Qt::UserRole).toInt() == evId) {
						t->setCurrentCell(
							r, MultiReplayDock::kColId,
							QItemSelectionModel::
									ClearAndSelect |
								QItemSelectionModel::Rows);
						break;
					}
				}
				dock->nudgeSelectedPointNs(true, -1'000'000'000LL);
			});
			ReplayEvent after;
			store.get(evId, after);
			const int64_t movedMs =
				(after.tInNs - before.tInNs) / 1'000'000;
			c.secondsHotkeyMovesPoint = movedMs <= -900 && movedMs >= -1100;
			obs_log(c.secondsHotkeyMovesPoint ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: the one-second key moved IN by "
				"%lld ms",
				(long long)movedMs);
			store.movePoint(evId, true, before.tInNs - after.tInNs);
		}

		// --- the position bar zooms -----------------------------------
		// A wheel notch over the bar has to change the scale, and the key
		// beside it has to put it back. Driven as a real wheel event, so
		// the widget's own handler is what is being tested.
		{
			double zoomed = 1.0, reset = 1.0;
			runOnUi([&]() {
				SeekBar *bar = dock->findChild<SeekBar *>();
				if (!bar)
					return;
				QWheelEvent ev(QPointF(bar->width() / 2.0,
						       bar->height() / 2.0),
					       bar->mapToGlobal(QPoint(
						       bar->width() / 2,
						       bar->height() / 2)),
					       QPoint(0, 0), QPoint(0, 120),
					       Qt::NoButton, Qt::NoModifier,
					       Qt::NoScrollPhase, false);
				for (int i = 0; i < 4; i++)
					QCoreApplication::sendEvent(bar, &ev);
				zoomed = bar->zoom();
				// The key is a MENU of spans now, not a reset, and
				// clicking it would park this thread inside
				// QMenu::exec(). So the check calls what the
				// menu's own "100%" entry calls — the same
				// function, one frame short of the mouse.
				dock->zoomWholeTimeline();
				reset = bar->zoom();
			});
			c.zoomReached = zoomed;
			c.seekbarZooms = zoomed > 1.5 && reset <= 1.001;
			obs_log(c.seekbarZooms ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: four wheel notches took the bar to "
				"%.2f×, the key put it back to %.2f×",
				zoomed, reset);
		}

		// --- the angle button wins ------------------------------------
		// Only the SECOND angle is flagged, and the operator presses the
		// FIRST. What he asked for is what must play: the old fallback to
		// "the first enabled angle" played another camera without a word,
		// which is the behaviour that made the angle model unguessable.
		{
			for (int a = 1; a <= kEventAngles; a++)
				store.setAngle(evId, a, false);
			store.setAngle(evId, a2, true);
			std::string serr;
			const bool started = pc.playEvents(
				{evId}, firstCam, false, serr,
				PlaybackCoordinator::AngleMode::Single);
			const auto ps = pc.playState();
			c.singleHonoursRequestedAngle =
				started && ps.queuedAngles ==
						   std::vector<int>{a1};
			std::string got;
			for (int a : ps.queuedAngles)
				got += std::to_string(a) + " ";
			obs_log(c.singleHonoursRequestedAngle ? LOG_INFO
							      : LOG_ERROR,
				"[selftest] dock: angle button wins — asked for %d "
				"with only %d flagged, queued [%s]%s%s",
				a1, a2, got.c_str(),
				started ? "" : " — refused: ",
				started ? "" : serr.c_str());
			pc.stopEvents();

			// ...and a camera with nothing behind it is REFUSED, with
			// something to say. Silence here is what left the operator
			// hearing a camera he had not asked for.
			std::string herr;
			const bool startedHole = pc.playEvents(
				{evId}, hole - 1, false, herr,
				PlaybackCoordinator::AngleMode::Single);
			c.singleReportsUnplayableAngle =
				!startedHole && !herr.empty();
			obs_log(c.singleReportsUnplayableAngle ? LOG_INFO
							       : LOG_ERROR,
				"[selftest] dock: unplayable angle %d — started=%s, "
				"reason '%s'",
				hole, startedHole ? "yes" : "no", herr.c_str());
			pc.stopEvents();
		}

		// --- a mark flags the angle being WATCHED ---------------------
		// Marking on angle 2 must produce an event enabled on angle 2 and
		// on nothing else. Deliberately not the first angle: an engine
		// that always flags camera 1 passes every check taken on camera 1,
		// and that is exactly what used to happen from a Stream Deck.
		{
			ReplayCore::instance().setCurrentAngle(secondCam);
			// The dock copies the angle from the core on its own tick.
			std::this_thread::sleep_for(
				std::chrono::milliseconds(200));
			const int before = store.lastEventId();
			runOnUi([&]() { markBtn->click(); });
			const int id2 = store.lastEventId();
			ReplayEvent ev2;
			if (id2 != before && store.get(id2, ev2)) {
				c.markInheritsAngle =
					ev2.angles[secondCam].enabled &&
					!ev2.angles[firstCam].enabled;
				obs_log(c.markInheritsAngle ? LOG_INFO : LOG_ERROR,
					"[selftest] dock: mark taken on angle %d — "
					"flagged %d:%s %d:%s",
					a2, a1,
					ev2.angles[firstCam].enabled ? "yes" : "no",
					a2,
					ev2.angles[secondCam].enabled ? "yes"
								      : "no");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] dock: the mark button created no "
					"event on angle %d", a2);
			}
			if (id2 != before)
				store.remove(id2);
			ReplayCore::instance().setCurrentAngle(firstCam);
			std::this_thread::sleep_for(
				std::chrono::milliseconds(200));
		}
	}

	// --- scrubbing shows the FOOTAGE, and keeps showing it ----------------
	// Dragging the seekbar is "review from here". The dock used to hand the
	// preview back to the live camera the moment that review ran out, so a
	// position chosen by hand in the recorded timeline ended up displaying the
	// camera as it is now — presented as the footage of then.
	{
		auto &chan = ReplayChannel::instance();
		SeekBar *bar = nullptr;
		runOnUi([&]() {
			// value() rather than a loop that breaks on the first
			// item: clang rejects that as an unreachable loop
			// increment under -Werror, and the dock has exactly one
			// seekbar anyway.
			bar = dock->findChild<SeekBar *>();
		});
		if (!bar) {
			obs_log(LOG_ERROR, "[selftest] dock: no seekbar found");
		} else {
			// Near the live edge, i.e. inside the ring — the part of the
			// timeline the gate can guarantee is servable.
			runOnUi([&]() { emit bar->seekRequested(0.90); });
			bool played = false;
			for (int i = 0; i < 60 && !played; i++) {
				played = chan.playing() ||
					 chan.stats().framesPushed > 0;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			// End the review and let the dock tick a few times: THIS is
			// where it used to jump back to the camera.
			chan.stop();
			std::this_thread::sleep_for(
				std::chrono::milliseconds(400));
			const bool stillFootage =
				dock->previewShowsReplay() &&
				!ReplayCore::instance().followLive();
			c.scrubShowsFootage = played && stillFootage;
			obs_log(c.scrubShowsFootage ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: scrub review played=%s, preview "
				"still on the footage afterwards=%s",
				played ? "yes" : "NO",
				stillFootage ? "yes" : "NO");
		}
	}

	// --- v1.4: FOOTAGE NOBODY MARKED --------------------------------------
	// Park the bar on a stretch no event covers, press ▶, and three things
	// have to be true at once: it plays, it plays THAT stretch (a queue with
	// no event behind it, starting where the operator armed), and Program is
	// untouched. Then Stop gives it up, and Play events — the key's second
	// function — puts the same stretch up again from the same instant.
	//
	// The middle claim is the one that needs the queue rather than the
	// engine: "something is playing" is equally true of the selected event
	// being replayed from somewhere else on the timeline, which is exactly
	// what this key used to do.
	{
		auto &pc = PlaybackCoordinator::instance();
		auto &chan = ReplayChannel::instance();
		SeekBar *bar = nullptr;
		QPushButton *playKey = nullptr;  // ▶ / ⏸
		QPushButton *stopKey = nullptr;  // ■
		QPushButton *playSelKey = nullptr; // "Play events"
		QPushButton *outKey = nullptr;   // "In output"
		// ── FOUND BY IDENTITY, NOT BY LABEL ──────────────────────────
		// The object names here are STYLE roles and two different keys can
		// legitimately share one; the text is a translation and, for the
		// keys that are marks now, gone entirely. The mrKey id is what a key
		// IS (see dock-icons.hpp).
		const QString stopGlyph = QStringLiteral("stop");
		const QString playSelText = QStringLiteral("playEvents");
		const QString outText = QStringLiteral("toOutput");
		runOnUi([&]() {
			bar = dock->findChild<SeekBar *>();
			playKey = dock->findChild<QPushButton *>(
				QStringLiteral("mrPlay"));
			for (QPushButton *b : dock->findChildren<QPushButton *>()) {
				const QString id =
					b->property(kKeyProperty).toString();
				if (id == stopGlyph)
					stopKey = b;
				else if (id == playSelText)
					playSelKey = b;
				else if (id == outText)
					outKey = b;
			}
		});

		// ARM IT. seekToFraction only arms when the instant it lands on is
		// outside the selected event, which is the point — so try a few
		// places rather than assuming any one of them is unmarked. The gate
		// has marked events of its own by now, and where they fall depends
		// on how long the take has been running.
		int64_t armed = kNoInstant;
		double armedAt = 0.0;
		if (bar) {
			for (double f : {0.85, 0.65, 0.45, 0.25}) {
				runOnUi([&]() { emit bar->seekRequested(f); });
				std::this_thread::sleep_for(
					std::chrono::milliseconds(250));
				armed = dock->freeReviewInNs();
				if (armed != kNoInstant) {
					armedAt = f;
					break;
				}
			}
		}
		// Let the scrub's own review finish; ▶ on a playing clip is a
		// pause, and that is a different key entirely.
		runOnUi([&]() { pc.stopEvents(); });
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		if (!bar || !playKey || !stopKey || !playSelKey ||
		    armed == kNoInstant) {
			obs_log(LOG_ERROR,
				"[selftest] dock: free review not testable "
				"(bar=%p play=%p stop=%p playSel=%p armed=%s)",
				(void *)bar, (void *)playKey, (void *)stopKey,
				(void *)playSelKey,
				armed == kNoInstant ? "NO" : "yes");
		} else {
			// The Program scene as it stands. A review that takes it
			// fails, whatever else it does right.
			std::string sceneBefore, sceneAfter;
			runOnUi([&]() {
				if (obs_source_t *sc =
					    obs_frontend_get_current_scene()) {
					sceneBefore = obs_source_get_name(sc);
					obs_source_release(sc);
				}
			});

			runOnUi([&]() { playKey->click(); });
			PlaybackCoordinator::PlayState st;
			for (int i = 0; i < 60; i++) {
				st = pc.playState();
				if (st.active && chan.stats().framesPushed > 0)
					break;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			runOnUi([&]() {
				if (obs_source_t *sc =
					    obs_frontend_get_current_scene()) {
					sceneAfter = obs_source_get_name(sc);
					obs_source_release(sc);
				}
			});
			// WHERE IT REALLY STARTED, read back from the dock.
			//
			// Not `armed`: the scrub that armed this also played a
			// short review, and that review walks the bar on while it
			// runs — so by the time ▶ is pressed the bar is a few
			// hundred milliseconds past the instant the scrub landed
			// on, and ▶ means "from where the bar is". Re-reading is
			// the claim being made anyway: whatever instant the free
			// review started from is the one the arm holds, and the
			// one Play events has to come back to after a Stop.
			const int64_t startedAt = dock->freeReviewInNs();
			const bool ranFree = st.active && st.eventId == 0 &&
					     startedAt != kNoInstant &&
					     st.clipInNs == startedAt;
			const bool stayedOffAir = sceneBefore == sceneAfter;
			c.freeReviewPlaysUnmarked = ranFree && stayedOffAir &&
						    chan.stats().framesPushed > 0;
			obs_log(c.freeReviewPlaysUnmarked ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: ▶ on unmarked footage at %.0f%% "
				"— queue active=%s event=%d in=%lld (armed %lld), "
				"frames=%llu, program '%s' → '%s'",
				armedAt * 100.0, st.active ? "yes" : "NO",
				st.eventId, (long long)st.clipInNs,
				(long long)startedAt,
				(unsigned long long)chan.stats().framesPushed,
				sceneBefore.c_str(), sceneAfter.c_str());

			// --- v1.4: THE ANGLE BOXES ARE ON THE REVIEW ---------
			// Sampled HERE, while the free review is running: the
			// boxes have to be showing that moment on every lens,
			// which is what makes choosing an angle possible at all.
			// Feeds are decoded on threads of their own, so this
			// waits for a picture rather than demanding one instantly.
			MultiReplayDock::MultiviewState mv;
			for (int i = 0; i < 60; i++) {
				mv = dock->multiviewState();
				if (mv.followingReview && mv.feedsWithPicture > 0)
					break;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(100));
			}
			c.multiviewFeeds = mv.feeds;
			c.multiviewFeedsWithPicture = mv.feedsWithPicture;
			c.multiviewFollowsReview = mv.followingReview &&
						   mv.feeds > 0 &&
						   mv.feedsWithPicture > 0;
			obs_log(c.multiviewFollowsReview ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: angle boxes during a review — "
				"following=%s, %d feed(s), %d with a picture, "
				"cued [%lld, %lld]",
				mv.followingReview ? "yes" : "NO", mv.feeds,
				mv.feedsWithPicture, (long long)mv.cueInNs,
				(long long)mv.cueOutNs);

			// --- Stop is a KEY, and it gives Program back --------
			runOnUi([&]() { stopKey->click(); });
			bool stopped = false;
			for (int i = 0; i < 40 && !stopped; i++) {
				stopped = !pc.playState().active;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			std::string sceneStopped;
			runOnUi([&]() {
				if (obs_source_t *sc =
					    obs_frontend_get_current_scene()) {
					sceneStopped = obs_source_get_name(sc);
					obs_source_release(sc);
				}
			});
			c.stopKeyEndsPlayback = stopped &&
						sceneStopped == sceneBefore;
			obs_log(c.stopKeyEndsPlayback ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: ■ stopped the free review=%s, "
				"program back to '%s' (was '%s')",
				stopped ? "yes" : "NO", sceneStopped.c_str(),
				sceneBefore.c_str());

			// --- ...AND PLAY EVENTS IS WHAT AIRS IT --------------
			// The armed instant has to survive the Stop: the operator
			// watched a stretch, stopped when he had seen enough, and
			// what he wants up is what he watched — from its
			// beginning, not from wherever the picture stopped.
			const bool stillArmed =
				startedAt != kNoInstant &&
				dock->freeReviewInNs() == startedAt;
			// "In output" ON, because that is the panel's own on-air
			// switch and this key has no private route past it.
			bool outWasChecked = false;
			runOnUi([&]() {
				if (outKey) {
					outWasChecked = outKey->isChecked();
					if (!outWasChecked)
						outKey->setChecked(true);
				}
				playSelKey->click();
			});
			PlaybackCoordinator::PlayState st2;
			for (int i = 0; i < 60; i++) {
				st2 = pc.playState();
				if (st2.active)
					break;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			std::string sceneAired;
			runOnUi([&]() {
				if (obs_source_t *sc =
					    obs_frontend_get_current_scene()) {
					sceneAired = obs_source_get_name(sc);
					obs_source_release(sc);
				}
			});
			c.freeReviewIsSecondPlayFunction =
				stillArmed && st2.active && st2.eventId == 0 &&
				st2.clipInNs == startedAt;
			obs_log(c.freeReviewIsSecondPlayFunction ? LOG_INFO
								 : LOG_ERROR,
				"[selftest] dock: Play events aired the free "
				"section — still armed=%s, queue active=%s "
				"event=%d in=%lld (armed %lld), program '%s'",
				stillArmed ? "yes" : "NO",
				st2.active ? "yes" : "NO", st2.eventId,
				(long long)st2.clipInNs, (long long)startedAt,
				sceneAired.c_str());

			runOnUi([&]() {
				pc.stopEvents();
				if (outKey && !outWasChecked)
					outKey->setChecked(false);
			});
			std::this_thread::sleep_for(
				std::chrono::milliseconds(300));

			// --- Live puts the whole panel back on the front -----
			// Including the boxes: they go back to mirroring their
			// cameras, and the feeds are let go rather than left
			// holding a still.
			QPushButton *liveKey = nullptr;
			const QString liveText = QString::fromUtf8(
				obs_module_text("Dock.LiveMode"));
			runOnUi([&]() {
				// By text again: Monitors shares Live's style
				// role, so the object name cannot pick between
				// them.
				for (QPushButton *b :
				     dock->findChildren<QPushButton *>())
					if (b->property(kKeyProperty).toString() ==
					    QStringLiteral("live"))
						liveKey = b;
				if (liveKey) {
					// setChecked(true) on an already-checked
					// key emits nothing, and it is the
					// TOGGLE that carries the behaviour.
					liveKey->setChecked(false);
					liveKey->setChecked(true);
				}
			});
			MultiReplayDock::MultiviewState mvLive;
			for (int i = 0; i < 40; i++) {
				mvLive = dock->multiviewState();
				if (!mvLive.followingReview && mvLive.feeds == 0)
					break;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			c.multiviewReturnsToLive = liveKey != nullptr &&
						   !mvLive.followingReview &&
						   mvLive.feeds == 0 &&
						   dock->freeReviewInNs() ==
							   kNoInstant;
			obs_log(c.multiviewReturnsToLive ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: Live returned the panel — "
				"following review=%s, %d feed(s) left, free "
				"review armed=%s",
				mvLive.followingReview ? "STILL" : "no",
				mvLive.feeds,
				dock->freeReviewInNs() == kNoInstant ? "no"
								     : "STILL");

			// PUT THE PANEL BACK WHERE THE CHECKS AFTER THIS ONE
			// EXPECT IT, and this is not tidiness.
			//
			// Live has just parked the transport ON THE LIVE EDGE,
			// and a frame step refuses there — correctly: there is no
			// next frame yet. The two step checks that follow then
			// measured two refusals against each other and called the
			// step broken. A check that changes the panel's mode owes
			// the next check the mode it found.
			runOnUi([&]() { emit bar->seekRequested(armedAt); });
			std::this_thread::sleep_for(
				std::chrono::milliseconds(300));
			runOnUi([&]() { pc.stopEvents(); });
			std::this_thread::sleep_for(
				std::chrono::milliseconds(200));
		}
	}

	// --- one frame forward really moves the picture on --------------------
	// The engine has no playhead to advance: a step is a two-frame range played
	// from just past where the bar stands. Proving it in the gate means proving
	// the range was servable and the input got the frame — a step that silently
	// does nothing looks identical to a step at the live edge.
	{
		auto &chan = ReplayChannel::instance();
		// TWO steps, compared against each other. The engine's position is
		// its last pushed frame and play() zeroes it at the start of every
		// clip, so "it is not 0 any more" would also be true of a step that
		// keeps replaying the same frame. Only the second step landing after
		// the first says the picture is really walking forward.
		const auto stepAndSettle = [&](int64_t floorNs) {
			runOnUi([&]() { stepBtn->click(); });
			int64_t pos = 0;
			for (int i = 0; i < 40; i++) {
				pos = chan.positionNs();
				if (pos > floorNs)
					break;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			// Let the two-frame clip finish, or the next play() would
			// join a worker mid-push.
			for (int i = 0; i < 20 && chan.playing(); i++)
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			return pos;
		};
		const int64_t first = stepAndSettle(0);
		const int64_t second = stepAndSettle(first);
		c.frameStepAdvances = first > 0 && second > first;
		obs_log(c.frameStepAdvances ? LOG_INFO : LOG_ERROR,
			"[selftest] dock: two frame steps landed on %lld ms then "
			"%lld ms (+%lld ms)",
			(long long)(first / 1000000), (long long)(second / 1000000),
			(long long)((second - first) / 1000000));

		// --- ...and one frame BACK really moves the picture back (v1.3) ---
		// Measured against the frame the forward steps left on screen, which
		// is what makes this a step and not just "something played": the
		// obvious implementation — play a short range forwards starting one
		// frame earlier — comes to rest on the frame ALREADY on screen and
		// would pass any check that only asked whether a clip ran.
		if (c.frameStepAdvances) {
			runOnUi([&]() { stepBackBtn->click(); });
			int64_t back = second;
			for (int i = 0; i < 60; i++) {
				const auto st = chan.stats();
				if (st.framesPushed > 0 && st.reverse &&
				    st.lastFrameNs < second) {
					back = st.lastFrameNs;
					break;
				}
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			for (int i = 0; i < 20 && chan.playing(); i++)
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			const auto st = chan.stats();
			// Exactly two pictures: the one on screen and the one
			// before it. More would mean the step became a short
			// reverse CLIP, which is a different (and jarring) gesture.
			c.stepBackMovesPlayhead = back < second && st.reverse &&
						  st.framesPushed >= 1 &&
						  st.framesPushed <= 2;
			obs_log(c.stepBackMovesPlayhead ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: one frame back went %lld ms → "
				"%lld ms (-%lld ms) in %llu picture(s), reverse=%s",
				(long long)(second / 1000000),
				(long long)(back / 1000000),
				(long long)((second - back) / 1000000),
				(unsigned long long)st.framesPushed,
				st.reverse ? "yes" : "NO");
		}
		chan.stop();
	}

	// --- the ◀ key plays the event backwards, through the queue ------------
	// Not straight to the engine: the point of routing reverse through the
	// coordinator is that it keeps the queue, the "to output" scene switch and
	// the finish callback, so what is checked is that the QUEUE reports a
	// reverse clip on air and that the instants really descend.
	{
		auto &chan = ReplayChannel::instance();
		auto &pc = PlaybackCoordinator::instance();
		pc.stopEvents();
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		runOnUi([&]() { revBtn->click(); });

		bool sawReverse = false;
		int64_t highest = 0, lowest = 0;
		int samples = 0;
		for (int i = 0; i < 80; i++) {
			const auto ps = pc.playState();
			const auto st = chan.stats();
			if (ps.active && ps.reverse)
				sawReverse = true;
			if (st.framesPushed > 0 && st.reverse) {
				if (samples == 0)
					highest = st.firstFrameNs;
				lowest = st.lastFrameNs;
				samples++;
			}
			if (sawReverse && samples > 0 && highest > lowest)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		c.reverseButtonPlaysBackwards = sawReverse && samples > 0 &&
						highest > lowest;
		obs_log(c.reverseButtonPlaysBackwards ? LOG_INFO : LOG_ERROR,
			"[selftest] dock: ◀ played %lld ms → %lld ms, queue says "
			"reverse=%s",
			(long long)(highest / 1000000),
			(long long)(lowest / 1000000), sawReverse ? "yes" : "NO");
		pc.stopEvents();
		chan.stop();
	}

	// --- THE KEYBOARD IS THE SAME LAYER AS THE KEYS -----------------------
	// A real QKeyEvent into the dock, not a call to the handler: what has to be
	// true is that the panel ROUTES the key, and the two ways that fails are both
	// invisible to a direct call — the table swallowing it for its own navigation,
	// and a focused button in a QButtonGroup taking the arrows for focus travel.
	{
		auto &chan = ReplayChannel::instance();
		auto &pc = PlaybackCoordinator::instance();
		pc.stopEvents();
		chan.stop();
		std::this_thread::sleep_for(std::chrono::milliseconds(150));

		const auto sendKey = [&](int key) {
			runOnUi([&]() {
				QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
				QCoreApplication::sendEvent(dock, &press);
			});
		};

		// ← FIRST, and not for symmetry: the sequence that just ended put the
		// playhead back on the LIVE EDGE (poll() does that whenever the live
		// front is still being fed), and at the live edge there is no next frame
		// — → refuses, rightly, and the check would be measuring that refusal.
		// One step back moves the playhead off the edge, so the forward step
		// below has somewhere to go. It also means both arrows are exercised.
		sendKey(Qt::Key_Left);
		for (int i = 0; i < 40; i++) {
			const auto st = chan.stats();
			if (st.framesPushed > 0 && st.reverse)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		for (int i = 0; i < 20 && chan.playing(); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));

		// → is one frame forward: the same thing the ⏭ key does, so the proof
		// is a frame really pushed FORWARDS (play() zeroes the stats, so a
		// non-reverse push here belongs to this step and not to the one above).
		sendKey(Qt::Key_Right);
		int64_t keyFrameNs = 0;
		for (int i = 0; i < 60; i++) {
			const auto st = chan.stats();
			if (st.framesPushed > 0 && !st.reverse) {
				keyFrameNs = st.lastFrameNs;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		for (int i = 0; i < 20 && chan.playing(); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));

		// ↓ or ↑ walks the event list. Only meaningful with two rows, and by
		// this point in the run there is more than one mark.
		int rows = 0, rowBefore = -1, rowAfter = -1;
		runOnUi([&]() {
			QTableWidget *t = dock->findChild<QTableWidget *>();
			if (!t)
				return;
			rows = t->rowCount();
			const auto sel = t->selectionModel()->selectedRows();
			rowBefore = sel.empty() ? -1 : sel.first().row();
		});
		if (rows >= 2) {
			// Down from the top, up from anywhere else, so the check does
			// not depend on which row the auto-selection left behind.
			sendKey(rowBefore <= 0 ? Qt::Key_Down : Qt::Key_Up);
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			runOnUi([&]() {
				QTableWidget *t = dock->findChild<QTableWidget *>();
				if (!t)
					return;
				const auto sel = t->selectionModel()->selectedRows();
				rowAfter = sel.empty() ? -1 : sel.first().row();
			});
		}
		const bool selectionMoved =
			rows < 2 || (rowAfter >= 0 && rowAfter != rowBefore);
		c.keyboardLayerWorks = keyFrameNs != 0 && selectionMoved;
		obs_log(c.keyboardLayerWorks ? LOG_INFO : LOG_ERROR,
			"[selftest] dock: → stepped to %lld ms, ↑/↓ moved row %d → %d "
			"of %d",
			(long long)(keyFrameNs / 1000000), rowBefore, rowAfter, rows);
		pc.stopEvents();
		chan.stop();
	}

	// --- CONTINUING PAST THE OUT lengthens the last clip -------------------
	// The gate's own project is configured with Config.continuePastOutMs set (see
	// the test config it writes before REC), so what is checked here is the QUEUE:
	// the last item must run longer than the event was marked. Measured off
	// queuedWallNs, which is what the green band counts down — a queue that
	// quietly kept the event's own OUT would leave the band and the picture
	// disagreeing about when the replay ends.
	{
		auto &pc = PlaybackCoordinator::instance();
		pc.stopEvents();
		std::this_thread::sleep_for(std::chrono::milliseconds(150));

		ReplayEvent ev;
		const bool haveEvent = EventStore::instance().get(evId, ev) &&
				       ev.tOutNs != kNoInstant;
		const int64_t markedNs = haveEvent ? ev.tOutNs - ev.tInNs : 0;

		// THE COORDINATOR DIRECTLY, not the Play key — and this cost a hung
		// gate to learn. playSelected() opens a modal QMessageBox when a play
		// is refused, so a refusal inside runOnUi() parks the UI thread in
		// exec() and every later step of the gate waits on it: the take carried
		// on recording for minutes and no report was ever written. What is
		// under test here is the QUEUE's extension anyway; the key itself is
		// covered by dock_plays_mark and the double-click check.
		std::string perr;
		runOnUi([&]() {
			PlaybackCoordinator::instance().playEvents(
				{evId}, 0, false, perr,
				PlaybackCoordinator::AngleMode::AllEnabled);
		});
		int64_t lastClipWallNs = 0;
		for (int i = 0; i < 60; i++) {
			const auto ps = pc.playState();
			if (ps.active && !ps.queuedWallNs.empty()) {
				lastClipWallNs = ps.queuedWallNs.back();
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		// A full second of the 1.5 s asked for, not all of it: the extension
		// stops where the footage stops, and at the instant of the play the live
		// edge may be closer than that. Anything past the OUT is the property;
		// a second of it means the setting is really being applied.
		c.continueExtraMs = (lastClipWallNs - markedNs) / 1000000;
		c.continuePastOutExtends = haveEvent && lastClipWallNs > 0 &&
					   c.continueExtraMs >= 1000;
		obs_log(c.continuePastOutExtends ? LOG_INFO : LOG_ERROR,
			"[selftest] dock: event %d marked %lld ms, queued %lld ms "
			"(+%lld ms past the OUT)%s%s",
			evId, (long long)(markedNs / 1000000),
			(long long)(lastClipWallNs / 1000000),
			(long long)c.continueExtraMs, perr.empty() ? "" : " — ",
			perr.c_str());
		pc.stopEvents();
	}

	// --- double-clicking a row puts that event on air ---------------------
	// the reference controller's fastest path from "that one" to "on air". Emitted on a column that
	// is NOT the speed cell, which is the one exception (double-click edits it).
	{
		auto &pc = PlaybackCoordinator::instance();
		pc.stopEvents();
		int row = -1;
		runOnUi([&]() {
			QTableWidget *t = dock->findChild<QTableWidget *>();
			if (!t)
				return;
			for (int r = 0; r < t->rowCount(); r++) {
				QTableWidgetItem *it = t->item(r, 0);
				if (it && it->data(Qt::UserRole).toInt() == evId)
					row = r;
			}
			if (row >= 0)
				emit t->cellDoubleClicked(row, 1); // the In column
		});
		for (int i = 0; i < 40 && !c.doubleClickPlays; i++) {
			const auto ps = pc.playState();
			c.doubleClickPlays = ps.active && ps.eventId == evId;
			if (!c.doubleClickPlays)
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
		}
		obs_log(c.doubleClickPlays ? LOG_INFO : LOG_ERROR,
			"[selftest] dock: double-click on row %d started event %d: "
			"%s",
			row, evId, c.doubleClickPlays ? "yes" : "NO");
		pc.stopEvents();
	}

	// --- the running order is the operator's ------------------------------
	// The ▲/▼ keys have to move the ROW, not just a field nobody draws: the
	// order the table shows IS the order a sequence plays in. Checked on the
	// real table, through the real key, by comparing the ids before and after.
	{
		// A second mark, so there is an order to change at all.
		runOnUi([&]() { markBtn->click(); });
		std::this_thread::sleep_for(std::chrono::milliseconds(200));

		QPushButton *upBtn = nullptr;
		const QString up = QStringLiteral("moveUp");
		std::vector<int> before, after;
		const auto idsInTableOrder = [&](std::vector<int> &out) {
			out.clear();
			QTableWidget *t = dock->findChild<QTableWidget *>();
			if (!t)
				return;
			for (int r = 0; r < t->rowCount(); r++) {
				QTableWidgetItem *it =
					t->item(r, MultiReplayDock::kColId);
				if (it)
					out.push_back(
						it->data(Qt::UserRole).toInt());
			}
		};
		runOnUi([&]() {
			for (QPushButton *b : dock->findChildren<QPushButton *>())
				if (b->property(kKeyProperty).toString() == up)
					upBtn = b;
			idsInTableOrder(before);
		});
		// What the handler was actually given, and the click, in ONE trip
		// to the GUI thread. Selecting in an earlier trip leaves a ~33 ms
		// window in which the dock's own poll can rebuild the table, so
		// the selection the handler reads is not necessarily the one that
		// was set — which is a property of the test, not of the dock, and
		// it has no business deciding the verdict.
		std::vector<int> selectedAtClick;
		if (upBtn && before.size() >= 2) {
			runOnUi([&]() {
				QTableWidget *t =
					dock->findChild<QTableWidget *>();
				// The LAST row, pushed up: the one move that
				// cannot be confused with the table's own
				// auto-selection of the newest mark.
				//
				// setCurrentCell with Rows|ClearAndSelect, not
				// selectRow(): one run in five arrived at the
				// click with an EMPTY selection ("selected at the
				// click: []"), so ▲ correctly did nothing and the
				// gate blamed the dock for a selection the test
				// had failed to make. This is what a click on the
				// row actually does — current index and full-row
				// selection together.
				if (t && t->rowCount() >= 2)
					t->setCurrentCell(
						t->rowCount() - 1,
						MultiReplayDock::kColId,
						QItemSelectionModel::ClearAndSelect |
							QItemSelectionModel::Rows);
				if (t)
					for (const auto &idx :
					     t->selectionModel()->selectedRows()) {
						QTableWidgetItem *it = t->item(
							idx.row(),
							MultiReplayDock::kColId);
						if (it)
							selectedAtClick.push_back(
								it->data(Qt::UserRole)
									.toInt());
					}
				upBtn->click();
			});
			for (int i = 0; i < 20; i++) {
				runOnUi([&]() { idsInTableOrder(after); });
				if (after.size() == before.size() &&
				    after.back() != before.back())
					break;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(50));
			}
			const size_t n = before.size();
			c.manualReorderMovesRow =
				after.size() == n && after[n - 2] == before[n - 1] &&
				after[n - 1] == before[n - 2];
			std::string b1, a1s, sel, storeOrder;
			for (int id : before)
				b1 += std::to_string(id) + " ";
			for (int id : after)
				a1s += std::to_string(id) + " ";
			for (int id : selectedAtClick)
				sel += std::to_string(id) + " ";
			// ...and what the STORE says, which is the whole
			// difference between "the running order did not change"
			// and "the table did not draw it". From outside the two
			// look identical and they need opposite fixes; this check
			// has failed intermittently without saying which it was.
			for (int id : before) {
				ReplayEvent ev;
				if (EventStore::instance().get(id, ev))
					storeOrder += std::to_string(id) + ":" +
						      std::to_string(ev.order) +
						      " ";
			}
			obs_log(c.manualReorderMovesRow ? LOG_INFO : LOG_ERROR,
				"[selftest] dock: ▲ moved the last row — order was "
				"[%s] now [%s] (selected at the click: [%s], store "
				"says [%s])",
				b1.c_str(), a1s.c_str(), sel.c_str(),
				storeOrder.c_str());
		} else {
			obs_log(LOG_ERROR,
				"[selftest] dock: no ▲ key (%p) or fewer than two "
				"rows (%zu) — cannot test the running order",
				(void *)upBtn, before.size());
		}
		// Whatever it was before, a manual move must leave the
		// chronological auto-sort OFF: with both in force the row snaps
		// back and nothing tells the operator why.
		c.manualReorderDisablesAutoSort =
			!ReplayCore::instance().getConfig().sortEventsByTime;

		// Take the extra mark back out.
		for (int id : after.empty() ? before : after)
			if (id != evId)
				store.remove(id);
	}

	// --- a renamed list is a name you can read ----------------------------
	// The complaint this answers is "PAR…": a list renamed to something
	// meaningful was drawn cut off. A tab is cut off exactly when it is laid
	// out narrower than it asked for, so that — and not "is there a name" — is
	// what is checked, on a name long enough to have needed the room.
	{
		const int list = store.selectedList();
		const std::string kept = store.listName(list);
		store.setListName(list, "Falli in area avversaria");
		// The dock re-labels the tabs on the store's version counter, which
		// its own poll picks up on the next tick.
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		int tooNarrow = -1;
		int visible = 0;
		runOnUi([&]() {
			QTabBar *tabs = dock->findChild<QTabBar *>();
			if (!tabs)
				return;
			tooNarrow = 0;
			// tabRect is the box Qt paints the label into, and it
			// elides whatever does not fit. tabSizeHint would be the
			// exact comparison but it is protected, so the label's own
			// width plus a few pixels of frame is the honest proxy: a
			// tab that has been shrunk below its text fails it.
			const QFontMetrics fm(tabs->font());
			for (int i = 0; i < tabs->count(); i++) {
				if (!tabs->isTabVisible(i))
					continue;
				visible++;
				const int need =
					fm.horizontalAdvance(tabs->tabText(i)) + 8;
				if (tabs->tabRect(i).width() < need)
					tooNarrow++;
			}
		});
		c.visibleListTabs = visible;
		c.listTabsFitTheirNames = tooNarrow == 0 && visible > 0;
		c.listTabCountFollowsConfig =
			visible == std::clamp(ReplayCore::instance()
						      .getConfig()
						      .eventListCount,
					      1, kEventLists);
		obs_log((c.listTabsFitTheirNames && c.listTabCountFollowsConfig)
				? LOG_INFO
				: LOG_ERROR,
			"[selftest] dock: %d list tab(s) visible (config says %d), "
			"%d drawn narrower than their name",
			visible, ReplayCore::instance().getConfig().eventListCount,
			tooNarrow);
		store.setListName(list, kept);
	}

	// Leave the operator's own project exactly as it was found.
	if (evId > 0)
		store.remove(evId);
	store.setSessionFolder(ReplayCore::instance().recordingFolder());
	return c;
}

// ---------------------------------------------------------------------------
// SOAK (M4): the same rig, for as long as a match lasts.
//
// Everything else in this file measures tens of seconds, and the faults M4 is
// about do not exist there: a ring that leaks a few MB a minute, a disk that
// fills, an encoder that hiccups once an hour, a counter that wraps. So this
// pass records for -SoakMinutes minutes and samples every 15 s, then judges the
// SHAPE of the run rather than any single number — packets kept flowing on
// every angle, the ring stayed inside its budget, resident memory did not climb
// away from it, nothing malformed, no discontinuity, and OBS kept its frames.
//
// It runs in its own project (deleted at the end: an hour of two cameras is
// gigabytes) and puts the operator's project and camera configuration back.
// Opt-in: nothing here runs unless the runner asks for it.
// ---------------------------------------------------------------------------
constexpr const char *kSoakProject = "MRSoakTest";

void runSoakPass(int minutes, const std::string &outPath)
{
	std::this_thread::sleep_for(std::chrono::seconds(3));

	auto &core = ReplayCore::instance();
	auto &tap = PacketTap::instance();
	auto &monitor = HealthMonitor::instance();

	const std::vector<std::string> realNames =
		splitCsv(envStr("OBS_MULTIREPLAY_SELFTEST_SOURCES"));
	const bool useRealSources = !realNames.empty();
	const int camCount = useRealSources
				     ? (int)std::min<size_t>(realNames.size(),
							     kMaxTapChannels)
				     : std::clamp(envInt("OBS_MULTIREPLAY_SELFTEST_CAMS",
							 2),
						  1, kMaxTapChannels);

	obs_video_info ovi{};
	const uint32_t cx = obs_get_video_info(&ovi) ? ovi.base_width : 1920;
	const uint32_t cy = obs_get_video_info(&ovi) ? ovi.base_height : 1080;

	const Config original = core.getConfig();
	const std::string prevProject = original.currentProjectName;
	std::string perr;
	runOnUi([&]() { core.newProject(kSoakProject, perr); });
	const std::string projectFolder = core.recordingFolder();

	std::vector<obs_source_t *> cams(camCount, nullptr);
	std::vector<bool> owned(camCount, false);
	obs_scene_t *scene = nullptr;
	std::array<bool, kMaxTapChannels> want{};
	int armed = 0;

	Config cfg = core.getConfig();
	for (auto &cam : cfg.cameras)
		cam.sourceName.clear();

	runOnUi([&]() {
		for (int i = 0; i < camCount; i++) {
			if (useRealSources)
				cams[i] = obs_get_source_by_name(realNames[i].c_str());
			else {
				cams[i] = createSyntheticCamera(i, cx, cy);
				owned[i] = cams[i] != nullptr;
				if (cams[i]) {
					obs_source_inc_showing(cams[i]);
					obs_source_inc_active(cams[i]);
				}
			}
		}
		if (!useRealSources) {
			scene = obs_scene_create("MRSoak Scene");
			for (int i = 0; i < camCount; i++)
				if (scene && cams[i])
					obs_scene_add(scene, cams[i]);
		}
		for (int i = 0; i < camCount && i < kMaxCameras; i++) {
			if (!cams[i])
				continue;
			const char *n = obs_source_get_name(cams[i]);
			cfg.cameras[i].sourceName = n ? n : "";
			want[i] = true;
			armed++;
		}
		core.setConfig(cfg);
	});

	bool started = false;
	std::string err;
	runOnUi([&]() { started = core.startRecording(err); });
	obs_log(started ? LOG_INFO : LOG_ERROR,
		"[soak] %d min on %d camera(s), project '%s': %s%s", minutes, armed,
		kSoakProject, started ? "recording" : "REFUSED: ", err.c_str());

	// --- the long middle --------------------------------------------------
	struct Sample {
		int64_t tSec = 0;
		int64_t rssMb = 0;
		int64_t ringMb = 0;
		int64_t worstAgeMs = 0;
		int64_t diskFreeMb = 0;
		uint64_t videoPackets = 0;
		int findings = 0;
	};
	std::vector<Sample> samples;
	std::vector<uint64_t> lastPackets(camCount, 0);
	bool everyIntervalFlowed = started;
	uint64_t malformed = 0, discontinuities = 0;
	const uint32_t laggedBefore = obs_get_lagged_frames();
	const uint32_t totalBefore = obs_get_total_frames();
	const int64_t t0 = (int64_t)os_gettime_ns();
	const int64_t rssStart = (int64_t)os_get_proc_resident_size();
	int64_t rssPeak = rssStart;
	health::Level worstSeen = health::Level::Ok;
	std::string worstFinding;

	const int intervals = std::max(1, minutes * 4); // one every 15 s
	for (int k = 0; started && k < intervals; k++) {
		std::this_thread::sleep_for(std::chrono::seconds(15));

		Sample s;
		s.tSec = ((int64_t)os_gettime_ns() - t0) / 1'000'000'000LL;
		s.rssMb = (int64_t)os_get_proc_resident_size() / (1024 * 1024);
		rssPeak = std::max(rssPeak, s.rssMb * 1024 * 1024);
		int64_t ringBytes = 0;
		for (int i = 0; i < camCount; i++) {
			if (!want[i])
				continue;
			const TapStats st = tap.stats(i);
			ringBytes += (int64_t)st.ringBytes;
			s.videoPackets += st.videoPackets;
			s.worstAgeMs = std::max(s.worstAgeMs, st.maxAgeUsec / 1000);
			malformed += st.malformedPackets;
			discontinuities += st.discontinuities;
			// The claim that matters over an hour: EVERY angle was
			// still producing in EVERY interval. An average hides a
			// camera that died forty minutes ago.
			if (st.videoPackets <= lastPackets[i])
				everyIntervalFlowed = false;
			lastPackets[i] = st.videoPackets;
		}
		s.ringMb = ringBytes / (1024 * 1024);
		std::error_code ec;
		auto space = std::filesystem::space(projectFolder, ec);
		s.diskFreeMb = ec ? -1 : (int64_t)(space.available / (1024 * 1024));

		const auto findings = monitor.findings();
		s.findings = (int)findings.size();
		if (health::worstOf(findings) > worstSeen) {
			worstSeen = health::worstOf(findings);
			worstFinding = findings.empty() ? "" : findings.front().id;
		}
		samples.push_back(s);
		obs_log(LOG_INFO,
			"[soak] t=%llds rss=%lld MB ring=%lld MB age=%lld ms "
			"disk=%lld MB findings=%d",
			(long long)s.tSec, (long long)s.rssMb, (long long)s.ringMb,
			(long long)s.worstAgeMs, (long long)s.diskFreeMb, s.findings);
	}

	const uint32_t laggedDelta = obs_get_lagged_frames() - laggedBefore;
	const uint32_t totalDelta = obs_get_total_frames() - totalBefore;
	const double laggedPct = totalDelta ? 100.0 * laggedDelta / totalDelta : 0.0;
	const int anchored = SegmentIndex::instance().anchoredCount();
	const int unanchored = SegmentIndex::instance().unanchoredCount();
	const int64_t ringPeakMb =
		samples.empty()
			? 0
			: std::max_element(samples.begin(), samples.end(),
					   [](const Sample &a, const Sample &b) {
						   return a.ringMb < b.ringMb;
					   })
				  ->ringMb;
	const int64_t rssGrowthMb =
		(rssPeak - rssStart) / (1024 * 1024);

	runOnUi([&]() { core.stopRecording(); });
	std::this_thread::sleep_for(std::chrono::seconds(3));

	// --- verdict ----------------------------------------------------------
	// Memory: the ring is not a leak however big it is, so the growth is
	// judged against the ring plus the same slack the runtime rules use.
	const bool memoryStable =
		rssGrowthMb <=
		ringPeakMb + health::kMemorySlackBytes / (1024 * 1024);
	const bool clean = malformed == 0 && discontinuities == 0;
	const bool filesOk = anchored >= armed && unanchored == 0;
	const bool obsOk = laggedPct <= 1.0;
	const bool healthOk = worstSeen < health::Level::Blocker;
	const bool pass = started && armed > 0 && everyIntervalFlowed && clean &&
			  memoryStable && filesOk && obsOk && healthOk &&
			  (int)samples.size() >= intervals;

	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "verdict", pass ? "PASS" : "FAIL");
	obs_data_set_int(root, "minutes", minutes);
	obs_data_set_int(root, "cameras_armed", armed);
	obs_data_set_bool(root, "synthetic_sources", !useRealSources);
	obs_data_t *checks = obs_data_create();
	obs_data_set_bool(checks, "recording_started", started);
	obs_data_set_bool(checks, "every_angle_flowed_every_interval",
			  everyIntervalFlowed);
	obs_data_set_bool(checks, "timeline_clean", clean);
	obs_data_set_bool(checks, "memory_stable", memoryStable);
	obs_data_set_bool(checks, "segments_anchored", filesOk);
	obs_data_set_bool(checks, "obs_impact_ok", obsOk);
	obs_data_set_bool(checks, "health_never_blocked", healthOk);
	obs_data_set_obj(root, "checks", checks);
	obs_data_release(checks);
	obs_data_set_int(root, "samples", (long long)samples.size());
	obs_data_set_int(root, "ring_peak_mb", ringPeakMb);
	obs_data_set_int(root, "rss_growth_mb", rssGrowthMb);
	obs_data_set_int(root, "segments_anchored", anchored);
	obs_data_set_int(root, "segments_unanchored", unanchored);
	obs_data_set_double(root, "obs_lagged_pct", laggedPct);
	obs_data_set_int(root, "malformed_packets", (long long)malformed);
	obs_data_set_int(root, "discontinuities", (long long)discontinuities);
	obs_data_set_string(root, "worst_health_level", health::levelName(worstSeen));
	obs_data_set_string(root, "worst_health_finding", worstFinding.c_str());
	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &s : samples) {
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "t_sec", s.tSec);
		obs_data_set_int(d, "rss_mb", s.rssMb);
		obs_data_set_int(d, "ring_mb", s.ringMb);
		obs_data_set_int(d, "worst_age_ms", s.worstAgeMs);
		obs_data_set_int(d, "disk_free_mb", s.diskFreeMb);
		obs_data_set_int(d, "video_packets", (long long)s.videoPackets);
		obs_data_set_int(d, "findings", s.findings);
		obs_data_array_push_back(arr, d);
		obs_data_release(d);
	}
	obs_data_set_array(root, "timeline", arr);
	obs_data_array_release(arr);

	// --- clean up: the soak's own footage is gigabytes --------------------
	runOnUi([&]() {
		for (int i = 0; i < camCount; i++) {
			if (!cams[i])
				continue;
			const std::string fname =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(i + 1);
			if (obs_source_t *f = obs_source_get_filter_by_name(
				    cams[i], fname.c_str())) {
				branch_output::setEnabled(f, false);
				obs_source_filter_remove(cams[i], f);
				obs_source_release(f);
			}
			if (owned[i]) {
				obs_source_dec_active(cams[i]);
				obs_source_dec_showing(cams[i]);
				obs_source_remove(cams[i]);
			}
			obs_source_release(cams[i]);
		}
		if (scene) {
			obs_source_remove(obs_scene_get_source(scene));
			obs_scene_release(scene);
		}
		core.setConfig(original);
		if (!prevProject.empty()) {
			std::string e2;
			core.openProject(prevProject, e2);
		}
	});
	// The index has to be stopped before the folder can go (its watcher holds
	// the files open) — the same trap the first pass documents.
	SegmentIndex::instance().stop();
	std::error_code ec;
	std::filesystem::remove_all(projectFolder, ec);
	if (ec)
		obs_log(LOG_ERROR, "[soak] could not delete %s: %s",
			projectFolder.c_str(), ec.message().c_str());

	if (!obs_data_save_json_safe(root, outPath.c_str(), "tmp", "bak"))
		obs_log(LOG_ERROR, "[soak] could not write report to %s",
			outPath.c_str());
	obs_log(LOG_INFO, "[soak] VERDICT=%s — report written to %s",
		pass ? "PASS" : "FAIL", outPath.c_str());
	obs_data_release(root);
}

// ---------------------------------------------------------------------------
// SECOND PASS, SECOND PROCESS: the project as it is REOPENED.
//
// Everything above runs inside one take, and inside a take the ring is full, so
// a live edge exists no matter what the files say. The bug Angelo reported does
// not live there: it appears when OBS is closed and started again on yesterday's
// project — the ring is empty, masterNs has restarted from a new zero, and the
// only thing that can still say where the footage begins and ends is
// anchors.json plus the recordings themselves.
//
// So this pass runs in a SEPARATE OBS process launched by the runner script on
// the folder the first pass left behind, and it never records. It asserts, in
// this order:
//   1. nothing is feeding a live edge (otherwise the rest proves nothing);
//   2. the index reloads the anchors and reaches the END of the footage;
//   3. the dock's own position bar spans it.
// The third is the one the operator sees, and the first two are what make its
// failure readable.
// ---------------------------------------------------------------------------
void runReopenPass(const std::string &outPath)
{
	// Let OBS settle exactly as the first pass does.
	std::this_thread::sleep_for(std::chrono::seconds(3));

	// Open the gate's own project the way the operator would — by name,
	// through the same call the Open Project menu makes — and remember his so
	// it can be put back. This is also what makes the pass honest: it reaches
	// the recordings through a project that was OPENED, not through a path
	// this process was handed.
	const std::string prevProject =
		ReplayCore::instance().getConfig().currentProjectName;
	{
		std::string perr;
		bool opened = false;
		runOnUi([&]() {
			opened = ReplayCore::instance().openProject(
				kSelfTestProject, perr);
		});
		if (!opened)
			obs_log(LOG_ERROR,
				"[selftest] reopen: cannot open project '%s': %s",
				kSelfTestProject, perr.c_str());
	}
	const std::string folder = ReplayCore::instance().recordingFolder();
	obs_log(LOG_INFO, "[selftest] REOPEN pass — project folder %s",
		folder.c_str());

	MultiReplayDock *dock = nullptr;
	runOnUi([&]() {
		if (QMainWindow *mw =
			    (QMainWindow *)obs_frontend_get_main_window())
			dock = mw->findChild<MultiReplayDock *>();
	});
	if (!dock)
		obs_log(LOG_ERROR,
			"[selftest] reopen: the dock is not in the main window — "
			"nothing to read the position bar off");

	// Nothing may be feeding the front. If a tap were attached this whole
	// pass would be measuring the live path again under another name.
	const bool liveEdgeDead = !PacketTap::instance().anyAttached();

	// One measurement, run twice against two different session epochs (see
	// below). Returns the span the index reports and the span the dock's bar
	// actually draws.
	struct Measured {
		int64_t footageMs = 0;
		int64_t barMs = 0;
		bool ok = false;
	};
	const auto measure = [&](const char *what,
				 int64_t epochMasterNs) -> Measured {
		Measured m;
		// Every slot: which cameras this project used is a property of
		// the files on disk, and the point of reopening is that we are
		// told rather than told beforehand. scanFolder reads the camera
		// out of each filename.
		std::array<bool, kMaxSegmentCameras> segCams{};
		segCams.fill(true);
		const int64_t epochWallNs =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::system_clock::now()
					.time_since_epoch())
				.count();
		SegmentIndex::instance().start(folder, segCams, epochMasterNs,
					       epochWallNs);

		// The recordings are re-anchored from anchors.json immediately,
		// but their LENGTHS are demuxed one file per watcher pass and
		// only accepted once two reads agree — tens of seconds by
		// design, not milliseconds.
		int64_t originNs = 0, endNs = 0;
		for (int i = 0; i < 120; i++) {
			originNs = SegmentIndex::instance().projectOriginNs();
			endNs = SegmentIndex::instance().projectEndNs();
			if (endNs > originNs && originNs != 0)
				break;
			std::this_thread::sleep_for(
				std::chrono::milliseconds(500));
		}
		if (originNs != 0 && endNs > originNs)
			m.footageMs = (endNs - originNs) / 1000000;

		// ...and the widget the operator looks at, which is the one that
		// was flat. Its poll is what pushes the length in, so give it
		// ticks rather than reading once.
		bool barHasTimeline = false;
		for (int i = 0; i < 40 && dock; i++) {
			runOnUi([&]() {
				if (SeekBar *bar = dock->findChild<SeekBar *>()) {
					m.barMs = bar->timelineNs() / 1000000;
					barHasTimeline = bar->hasTimeline();
				}
			});
			if (barHasTimeline)
				break;
			std::this_thread::sleep_for(
				std::chrono::milliseconds(250));
		}

		// A take of the length the first pass records is tens of
		// seconds; one second is far below that and far above a
		// fragment's rounding.
		m.ok = liveEdgeDead && m.footageMs >= 1000 && barHasTimeline &&
		       m.barMs >= 1000;
		obs_log(m.ok ? LOG_INFO : LOG_ERROR,
			"[selftest] reopen (%s): footage spans %lld ms, the "
			"position bar spans %lld ms (live edge dead: %s)", what,
			(long long)m.footageMs, (long long)m.barMs,
			liveEdgeDead ? "yes" : "NO");
		SegmentIndex::instance().stop();
		return m;
	};

	// 1. Reopened in a later OBS run on the SAME boot. os_gettime_ns() counts
	//    from boot, so the footage maps to a master instant near this one.
	const Measured sameBoot = measure("same boot", (int64_t)os_gettime_ns());

	// 2. Reopened AFTER A REBOOT — the ordinary case, and the one the whole
	//    feature is for: yesterday's match, opened this morning. It is
	//    simulated rather than waited for, by claiming this session started
	//    one minute after boot: every anchor in the file is then older than
	//    the session's own zero, which is precisely what a reboot does to
	//    wallToMasterNs. Nothing live is running in this pass, so faking the
	//    epoch cannot disturb anything.
	const Measured rebooted = measure("after a reboot", 60'000'000'000LL);

	// --- ⛶ FULL SCREEN, AND ONLY WHILE THE PANEL FLOATS -------------------
	//
	// Run in THIS pass on purpose. Floating a dock destroys and rebuilds the
	// native window of every OBSQTDisplay under it — both bays and every
	// multiview tile — and the take pass is where that churn would be measured
	// as a stranded display or a starved preview. Here nothing is recording,
	// nothing is playing, and nothing is being timed.
	//
	// THE CHECK IS THE GEOMETRY, NOT QT'S FLAG. showFullScreen() sets the state
	// on any window; whether the window then actually grows to the monitor
	// depends on the window flags a QDockWidget floats with, which are the
	// platform's business and differ between them. A run that asked only
	// isFullScreen() would go green on a panel that had not moved one pixel —
	// which is the entire failure this key can have.
	//
	// And it is driven through the REAL key, with click(): the visibility rule
	// ("hidden while docked, not greyed out") lives in the dock's slow beat,
	// and a check that called setPanelFullScreen() directly would prove the
	// window state and nothing about the key that reaches it.
	bool fsKeyHiddenWhenDocked = false;
	// Read below, inside the floating-panel block, and reported outside it.
	bool tilesWideOk = false, tilesTallOk = false;
	bool fsKeyShownWhenFloating = false;
	bool fsCoversTheScreen = false;
	bool fsRestoresTheWindow = false;
	bool fsWindowOffersMaximise = false;
	bool fsDoubleClickIsInert = false;
	{
		QDockWidget *host = nullptr;
		QPushButton *fsKey = nullptr;
		bool wasFloating = false;
		bool wasVisible = false;
		QRect windowed;
		QRect fullGeom;
		QRect screenGeom;
		Qt::WindowFlags floatFlags;
		int maxWidth = 0;
		int availWidth = 0;
		runOnUi([&]() {
			if (!dock)
				return;
			for (QWidget *w = dock->parentWidget(); w;
			     w = w->parentWidget())
				if (auto *d = qobject_cast<QDockWidget *>(w)) {
					host = d;
					break;
				}
			// By glyph, the way the transport keys are found: exactly
			// one button in this dock carries it.
			for (QPushButton *b : dock->findChildren<QPushButton *>())
				if (b->property(kKeyProperty).toString() ==
				    QStringLiteral("fullscreen"))
					fsKey = b;
			if (!host)
				return;
			wasFloating = host->isFloating();
			wasVisible = host->isVisible();
			// START FROM A KNOWN STATE. The operator's saved layout
			// decides how this dock comes up, and both halves of the
			// rule are being tested — so neither may be assumed.
			// The dock is shown because a hidden dock has no window
			// to make full screen, and put back exactly as found.
			if (!wasVisible)
				host->show();
			if (wasFloating)
				host->setFloating(false);
		});
		if (host && fsKey) {
			// The key follows the floating state on the dock's slow
			// beat (~264 ms), so every wait here is that beat plus
			// room for the window manager.
			std::this_thread::sleep_for(
				std::chrono::milliseconds(900));
			runOnUi([&]() {
				fsKeyHiddenWhenDocked =
					!fsKey->isVisibleTo(fsKey->parentWidget());
				host->setFloating(true);
			});
			std::this_thread::sleep_for(
				std::chrono::milliseconds(900));
			runOnUi([&]() {
				fsKeyShownWhenFloating =
					fsKey->isVisibleTo(fsKey->parentWidget());
				windowed = host->geometry();
				fsKey->click();
			});
			std::this_thread::sleep_for(
				std::chrono::milliseconds(700));
			runOnUi([&]() {
				fullGeom = host->geometry();
				if (QScreen *sc = host->screen())
					screenGeom = sc->geometry();
				fsCoversTheScreen =
					host->isFullScreen() &&
					!screenGeom.isEmpty() &&
					fullGeom.width() >= screenGeom.width() &&
					fullGeom.height() >=
						screenGeom.height();
				fsKey->click();
			});
			std::this_thread::sleep_for(
				std::chrono::milliseconds(700));
			runOnUi([&]() {
				const QRect back = host->geometry();
				// Back to the size it had, not to whatever Qt
				// remembered: coming out of full screen into a
				// postage stamp in a corner is worse than not
				// having the key.
				fsRestoresTheWindow =
					!host->isFullScreen() &&
					std::abs(back.width() -
						 windowed.width()) <= 8 &&
					std::abs(back.height() -
						 windowed.height()) <= 8;
			});

			// --- what the floating window itself owes --------------
			// MAXIMISE YES, MINIMISE NO, and the NO is the half that
			// earns its place. A minimise box on this window is a
			// one-way door — an OBS dock is owned by the OBS main
			// window, so Windows gives it no taskbar button and the
			// panel goes somewhere with no handle on it. It shipped
			// once; this is what stops it shipping twice.
			//
			// Read off the FLAGS, because that is what the window
			// manager draws the buttons from, and the type as well:
			// Qt floats a dock as a Qt::Tool, whose small caption has
			// no maximise box for the hint to turn on.
			runOnUi([&]() {
				floatFlags = host->windowFlags();
			});
			fsWindowOffersMaximise =
				floatFlags.testFlag(Qt::WindowMaximizeButtonHint) &&
				!floatFlags.testFlag(Qt::WindowMinimizeButtonHint) &&
				(floatFlags & Qt::WindowType_Mask) != Qt::Tool;

			// DOUBLE CLICK ON THE TITLE BAR — and what this can
			// honestly assert changed, which is worth writing down.
			//
			// The maximise itself is now the PLATFORM's: a non-client
			// message falls through to DefWindowProc, and Windows
			// toggles maximise because the window has a maximise box
			// (asserted just above). A synthetic Qt event never
			// reaches DefWindowProc, so this check could not see that
			// half even when it claimed to — it was measuring our own
			// handler, and our own handler is exactly what had to
			// stop acting. That is how the shipped fault got past a
			// green check: the panel shrank and grew straight back,
			// because both actors were toggling.
			//
			// So what is on trial is OUR contract, and both halves
			// are faults that have actually shipped: the panel must
			// NOT re-dock (Qt's answer, which hid it behind another
			// dock's tab), and the state must NOT change (ours, the
			// double toggle). The operator's own eye is the only
			// thing that can watch Windows do its half.
			bool stateBefore = false;
			runOnUi([&]() {
				maxWidth = host->width();
				if (QScreen *sc = host->screen())
					availWidth =
						sc->availableGeometry().width();
				stateBefore = host->isMaximized();
				const QPointF pos(host->width() / 2.0, -8.0);
				QMouseEvent ev(
					QEvent::NonClientAreaMouseButtonDblClick,
					pos, host->mapToGlobal(pos),
					Qt::LeftButton, Qt::LeftButton,
					Qt::NoModifier);
				QCoreApplication::sendEvent(host, &ev);
			});
			std::this_thread::sleep_for(
				std::chrono::milliseconds(700));
			runOnUi([&]() {
				fsDoubleClickIsInert =
					host->isFloating() &&
					!host->isFullScreen() &&
					host->isMaximized() == stateBefore;
				// Put the panel back exactly as it was found.
				host->setFloating(wasFloating);
				host->setVisible(wasVisible);
			});
			std::this_thread::sleep_for(
				std::chrono::milliseconds(400));
		}
		// ── THE CAMERAS ARE ON SCREEN IN BOTH ARRANGEMENTS ────────────────────
		// A tile has NO SIZE OF ITS OWN — AspectBox declares neither a floor nor
		// a hint, deliberately, because a floor there becomes the whole panel's —
		// so its width comes entirely from the grid column it sits in. A column
		// rule that hands the row's stretch somewhere else therefore does not
		// make the strip narrow, it makes it ZERO: the panel shows an empty box
		// where the cameras should be.
		//
		// That shipped. The column arrangement's rule had been wrong from the
		// day it was written and an early-out kept it from ever running, so the
		// fault only appeared when the early-out was corrected — which is the
		// shape of bug this check exists for: nothing else here can tell a tile
		// that is not there from a tile that is there and has no width.
		//
		// Measured on the FLOATING window, because its size is the only one this
		// process may set: a docked panel's size belongs to OBS.
		int wideTileW = 0, tallTileW = 0, wideTiles = 0, tallTiles = 0;
		const char *wideMode = "?", *tallMode = "?";
		if (host && dock) {
			auto measure = [&](int w, int h, bool &ok, int &narrowest,
					   int &seen, const char *&modeName) {
				runOnUi([&]() {
					host->setFloating(true);
					host->resize(w, h);
				});
				std::this_thread::sleep_for(
					std::chrono::milliseconds(700));
				// TWICE. A mode change rewrites how small the panel may
				// be, so a size only the NEW arrangement can hold is
				// reached on the second resize (see applyPanelMode).
				runOnUi([&]() { host->resize(w, h); });
				std::this_thread::sleep_for(
					std::chrono::milliseconds(700));
				runOnUi([&]() {
					modeName = panelModeName(dock->panelMode());
					int nw = 1 << 20, n = 0;
					for (QWidget *t : dock->findChildren<QWidget *>(
						     QStringLiteral("mrTile"))) {
						if (!t->isVisible())
							continue;
						n++;
						nw = std::min(nw, t->width());
					}
					seen = n;
					narrowest = n ? nw : 0;
					// 40 px is not a taste: below that a picture is
					// not a confidence monitor any more, and zero —
					// the fault this catches — is far below it.
					ok = n > 0 && narrowest >= 40;
				});
			};
			measure(1100, 700, tilesWideOk, wideTileW, wideTiles, wideMode);
			measure(340, 900, tilesTallOk, tallTileW, tallTiles, tallMode);
			runOnUi([&]() {
				host->setFloating(wasFloating);
				host->setVisible(wasVisible);
			});
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
		}
		obs_log((tilesWideOk && tilesTallOk) ? LOG_INFO : LOG_ERROR,
			"[selftest] reopen: camera tiles — %s: %d on screen, narrowest "
			"%d px: %s; %s: %d on screen, narrowest %d px: %s",
			wideMode, wideTiles, wideTileW, tilesWideOk ? "yes" : "NO",
			tallMode, tallTiles, tallTileW, tilesTallOk ? "yes" : "NO");

		const bool fsOk = fsKeyHiddenWhenDocked && fsKeyShownWhenFloating &&
				  fsCoversTheScreen && fsRestoresTheWindow &&
				  fsWindowOffersMaximise && fsDoubleClickIsInert;
		obs_log(fsOk ? LOG_INFO : LOG_ERROR,
			"[selftest] reopen: floating panel — key hidden docked: "
			"%s, key shown floating: %s, full screen covers %dx%d of "
			"a %dx%d screen: %s, restores %dx%d: %s, window flags "
			"0x%08x offer maximise and no minimise: %s, title double "
			"click neither re-docks nor toggles (window %d of %d "
			"available): %s",
			fsKeyHiddenWhenDocked ? "yes" : "NO",
			fsKeyShownWhenFloating ? "yes" : "NO", fullGeom.width(),
			fullGeom.height(), screenGeom.width(),
			screenGeom.height(), fsCoversTheScreen ? "yes" : "NO",
			windowed.width(), windowed.height(),
			fsRestoresTheWindow ? "yes" : "NO",
			(unsigned)floatFlags.toInt(),
			fsWindowOffersMaximise ? "yes" : "NO", maxWidth,
			availWidth, fsDoubleClickIsInert ? "yes" : "NO");
	}

	const bool pass = sameBoot.ok && rebooted.ok && fsKeyHiddenWhenDocked &&
			  fsWindowOffersMaximise && fsDoubleClickIsInert &&
			  fsKeyShownWhenFloating && fsCoversTheScreen &&
			  fsRestoresTheWindow && tilesWideOk && tilesTallOk;

	// --- Put everything back ----------------------------------------------
	// The operator's project first (so nothing is pointing into the test one),
	// then the test project goes away entirely — which is what makes the whole
	// gate re-runnable without leaving a trail of MRSelfTest folders and
	// without any run being judged against the previous run's footage.
	if (!prevProject.empty()) {
		std::string perr;
		bool restored = false;
		runOnUi([&]() {
			restored = ReplayCore::instance().openProject(prevProject,
								      perr);
		});
		obs_log(restored ? LOG_INFO : LOG_ERROR,
			"[selftest] operator's project '%s' restored: %s",
			prevProject.c_str(), restored ? "yes" : perr.c_str());
	}
	std::error_code rec;
	const uintmax_t removed = std::filesystem::remove_all(folder, rec);
	obs_log(rec ? LOG_ERROR : LOG_INFO,
		"[selftest] test project deleted: %s (%llu entries)%s",
		folder.c_str(), (unsigned long long)removed,
		rec ? rec.message().c_str() : "");

	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "verdict", pass ? "PASS" : "FAIL");
	obs_data_set_string(root, "pass_name", "reopened-project");
	obs_data_set_string(root, "project_folder", folder.c_str());
	obs_data_t *checks = obs_data_create();
	obs_data_set_bool(checks, "reopen_live_edge_is_dead", liveEdgeDead);
	obs_data_set_bool(checks, "reopen_timeline_from_disk", sameBoot.ok);
	obs_data_set_bool(checks, "reopen_survives_a_reboot", rebooted.ok);
	// Four booleans, not one: "the full-screen key is broken" is four
	// different faults with four different fixes, and a single flag would
	// make the log the only place that says which.
	obs_data_set_bool(checks, "fullscreen_key_hidden_when_docked",
			  fsKeyHiddenWhenDocked);
	obs_data_set_bool(checks, "fullscreen_key_shown_when_floating",
			  fsKeyShownWhenFloating);
	obs_data_set_bool(checks, "fullscreen_covers_the_screen",
			  fsCoversTheScreen);
	obs_data_set_bool(checks, "fullscreen_restores_the_window",
			  fsRestoresTheWindow);
	obs_data_set_bool(checks, "floating_window_offers_maximise",
			  fsWindowOffersMaximise);
	obs_data_set_bool(checks, "title_double_click_leaves_the_panel_alone",
			  fsDoubleClickIsInert);
	obs_data_set_bool(checks, "camera_tiles_have_width_when_wide", tilesWideOk);
	obs_data_set_bool(checks, "camera_tiles_have_width_in_a_column", tilesTallOk);
	obs_data_set_obj(root, "checks", checks);
	obs_data_release(checks);
	obs_data_set_int(root, "reopen_footage_span_ms", sameBoot.footageMs);
	obs_data_set_int(root, "reopen_bar_span_ms", sameBoot.barMs);
	obs_data_set_int(root, "reopen_rebooted_footage_span_ms",
			 rebooted.footageMs);
	obs_data_set_int(root, "reopen_rebooted_bar_span_ms", rebooted.barMs);

	if (!obs_data_save_json_safe(root, outPath.c_str(), "tmp", "bak"))
		obs_log(LOG_ERROR, "[selftest] could not write report to %s",
			outPath.c_str());
	obs_log(LOG_INFO, "[selftest] REOPEN VERDICT=%s — report written to %s",
		pass ? "PASS" : "FAIL", outPath.c_str());
	obs_data_release(root);
}

void runSelfTest()
{
	const int durationSecs = envInt("OBS_MULTIREPLAY_SELFTEST_SECS", 25);
	const std::string sourcesCsv = envStr("OBS_MULTIREPLAY_SELFTEST_SOURCES");
	const bool useRealSources = !sourcesCsv.empty();
	std::vector<std::string> realNames = splitCsv(sourcesCsv);

	int camCount = useRealSources
			       ? (int)realNames.size()
			       : envInt("OBS_MULTIREPLAY_SELFTEST_CAMS", 2);
	camCount = std::max(1, std::min(camCount, kMaxCameras));

	std::string outPath = envStr("OBS_MULTIREPLAY_SELFTEST_OUT");
	if (outPath.empty()) {
		std::error_code ec;
		outPath = (std::filesystem::temp_directory_path(ec) /
			   "obs-multireplay-selftest.json")
				  .string();
	}

	obs_log(LOG_INFO,
		"[selftest] START — cams=%d duration=%ds sources=%s out=%s",
		camCount, durationSecs,
		useRealSources ? sourcesCsv.c_str() : "(synthetic)",
		outPath.c_str());

	// Let OBS finish settling (scene collection, devices, encoders).
	std::this_thread::sleep_for(std::chrono::seconds(3));

	struct obs_video_info ovi = {};
	const bool haveOvi = obs_get_video_info(&ovi);
	const uint32_t cx = haveOvi ? ovi.base_width : 1920;
	const uint32_t cy = haveOvi ? ovi.base_height : 1080;
	const double canvasFps =
		haveOvi && ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 30.0;

	// --- The gate gets a PROJECT OF ITS OWN -------------------------------
	// It records, marks, renames lists, edits speeds and deletes events. None
	// of that may land in a project of the operator's, and none of it may be
	// judged against one either: an old project already has declared events
	// and anchored files, so a check reading "is there footage" would pass on
	// yesterday's material rather than on what this run produced.
	//
	// So: create MRSelfTest under the configured session folder, work there,
	// and put the operator's project back afterwards (kSelfTestProject is
	// deleted at the end of the reopen pass, which needs the recordings this
	// one leaves behind). Restoring is in a scope guard because every early
	// return between here and the end would otherwise leave OBS pointing at
	// the test project.
	std::error_code ec;
	const std::string prevProject =
		ReplayCore::instance().getConfig().currentProjectName;
	{
		std::string perr;
		bool made = false;
		runOnUi([&]() {
			made = ReplayCore::instance().newProject(
				kSelfTestProject, perr);
		});
		obs_log(made ? LOG_INFO : LOG_ERROR,
			"[selftest] project '%s' (was '%s')%s%s", kSelfTestProject,
			prevProject.c_str(), made ? "" : " — FAILED: ",
			made ? "" : perr.c_str());
	}

	Config cfg = ReplayCore::instance().getConfig();
	const std::filesystem::path folder =
		ReplayCore::instance().recordingFolder();

	// Start from an EMPTY project. Leftovers from an earlier run are files
	// whose openings are long gone from the ring, so they can never be
	// anchored — and worse, they are older than this take, so they become the
	// project's origin and every event timecode is measured from a recording
	// that has nothing to do with the run.
	//
	// The index has to be stopped FIRST. newProject() points it at this folder
	// and its watcher immediately opens the files to demux their durations, so
	// remove_all could not delete them — and it says so only through an
	// error_code nobody was reading, which is why three runs in a row silently
	// inherited the previous one's footage (8 anchored segments where the take
	// wrote 2, and a mark sitting 8 minutes "after the project origin").
	SegmentIndex::instance().stop();
	// Retried: a background thread of ours (the disk-bandwidth probe was one,
	// until it stopped writing inside project folders) or the OS still
	// holding a handle for a moment must not be the difference between "this
	// run measures its own take" and "this run measures the last one".
	for (int attempt = 0; attempt < 5; attempt++) {
		ec.clear();
		std::filesystem::remove_all(folder, ec);
		if (!ec)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
	}
	if (ec)
		obs_log(LOG_ERROR,
			"[selftest] could not empty the test project %s: %s — "
			"this run would measure the PREVIOUS run's footage",
			folder.string().c_str(), ec.message().c_str());
	ec.clear();
	std::filesystem::create_directories(folder, ec);
	cfg.splitMinutes = 20;
	// Where the recordings actually land. NOT cfg.sessionFolder any more:
	// that is now the PARENT of every project, and Branch Output writes into
	// sessionFolder/currentProjectName. Watching the parent found no files at
	// all — 0 anchored and 0 unanchored, which reads like a broken anchor and
	// is really a wrong folder.
	const std::string projectFolder = folder.string();

	// TWO BAYS for this run. The second one is optional now and off by default,
	// but half a dozen checks below drive it (it plays, it takes its own scene,
	// the swap moves a clip across), and with it off its input is never even
	// created. Set HERE, before REC: setConfig() re-points the segment index, so
	// doing it mid-take would disturb the anchoring the run is about to measure.
	// The operator's own config is restored at the end of the health pass.
	//
	// CONTINUE PAST THE OUT rides along here for the same reason, and it has to
	// ride HERE and nowhere else: the `cfg` copy above is never written back (the
	// gate arms the filters from it directly), so setting the field on it was a
	// dead assignment — the run reported "+0 ms past the OUT" and the log had no
	// line about continuing at all, which is exactly what a config that never
	// arrived looks like. This is the block that really calls setConfig().
	{
		Config abCfg = ReplayCore::instance().getConfig();
		const bool needB = !abCfg.enableChannelB;
		const bool needPastOut = abCfg.continuePastOutMs != 1500;
		if (needB || needPastOut) {
			abCfg.enableChannelB = true;
			abCfg.continuePastOutMs = 1500;
			runOnUi([&]() { ReplayCore::instance().setConfig(abCfg); });
			obs_log(LOG_INFO,
				"[selftest] channel B enabled for this run (the "
				"default is off) and continue-past-out set to 1500 ms");
		}
	}

	// --- Acquire the cameras and arm Branch Output ------------------------
	// All of this runs on the UI thread (see runOnUi): a Branch Output filter
	// created off it never starts recording.
	std::vector<obs_source_t *> cams(camCount, nullptr);
	std::vector<bool> owned(camCount, false);
	obs_scene_t *testScene = nullptr;
	std::array<bool, kMaxTapChannels> want{};
	int armed = 0;

	runOnUi([&]() {
		for (int i = 0; i < camCount; i++) {
			if (useRealSources) {
				cams[i] = obs_get_source_by_name(
					realNames[i].c_str());
				if (!cams[i])
					obs_log(LOG_ERROR,
						"[selftest] source '%s' not found",
						realNames[i].c_str());
			} else {
				cams[i] = createSyntheticCamera(i, cx, cy);
				owned[i] = cams[i] != nullptr;
				if (cams[i]) {
					obs_source_inc_showing(cams[i]);
					obs_source_inc_active(cams[i]);
				}
			}
		}

		// Branch Output ignores a filter whose parent is not part of the
		// frontend scene collection (sourceInFrontend()), so the synthetic
		// cameras need a scene to live in. It does NOT have to be the
		// program scene — Branch Output renders through its own obs_view —
		// so we never touch what the operator has on air.
		if (!useRealSources) {
			testScene = obs_scene_create("MRSelfTest Scene");
			if (testScene) {
				for (int i = 0; i < camCount; i++)
					if (cams[i])
						obs_scene_add(testScene, cams[i]);
				obs_log(LOG_INFO,
					"[selftest] synthetic cameras placed in "
					"'MRSelfTest Scene'");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] could not create the test scene");
			}
		}

		for (int i = 0; i < camCount; i++) {
			if (!cams[i])
				continue;
			// Created, NOT armed: this is exactly what New Project /
			// Open Project / Settings do, and it must not record.
			obs_source_t *filter =
				branch_output::ensureFilter(cams[i], i, cfg);
			if (!filter) {
				obs_log(LOG_ERROR,
					"[selftest] cam%d: filter creation failed",
					i + 1);
				continue;
			}
			obs_source_release(filter);
			want[i] = true;
			armed++;
		}

		// THE OUTPUT SCENES HAVE TO CONTAIN THE REPLAY, or "to output"
		// switches the Program to an empty scene: black, on air, and every
		// check about it still green because the switch itself happened.
		// The scenes come from the collection the harness generated; the
		// sources cannot be written into that file because they do not
		// exist until this plugin has loaded, so they are put in here.
		ensureChannelInScene(cfg.outputSceneName.c_str(), Which::A);
		ensureChannelInScene(cfg.outputSceneNameB.c_str(), Which::B);
	});

	// --- A configured filter must NOT be a recording filter ---------------
	// Branch Output re-evaluates its start conditions once a second, so give it
	// three ticks with the filters merely CONFIGURED and check that nothing is
	// writing. This is the regression that made creating a project start the
	// take: the filter was born enabled, BO obliged, and the recording began
	// before the tap existed to anchor it against.
	bool filtersIdleOutsideRec = armed > 0;
	std::this_thread::sleep_for(std::chrono::milliseconds(3000));
	runOnUi([&]() {
		for (int i = 0; i < camCount; i++) {
			if (!want[i] || !cams[i])
				continue;
			const std::string fname =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(i + 1);
			bool enabled = true;
			if (obs_source_t *f = obs_source_get_filter_by_name(
				    cams[i], fname.c_str())) {
				enabled = obs_source_enabled(f);
				obs_source_release(f);
			}
			const bool writing = branch_output::recordingOutputActive(i);
			if (enabled || writing) {
				filtersIdleOutsideRec = false;
				obs_log(LOG_ERROR,
					"[selftest] cam%d: filter armed itself outside "
					"REC (enabled=%s, recording output active=%s)",
					i + 1, enabled ? "yes" : "no",
					writing ? "yes" : "no");
			}
		}
	});
	obs_log(filtersIdleOutsideRec ? LOG_INFO : LOG_ERROR,
		"[selftest] filters idle while only configured: %s",
		filtersIdleOutsideRec ? "yes" : "NO");

	// Now arm them, which is what REC does and the only thing that may.
	runOnUi([&]() {
		for (int i = 0; i < camCount; i++) {
			if (!want[i] || !cams[i])
				continue;
			const std::string fname =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(i + 1);
			if (obs_source_t *f = obs_source_get_filter_by_name(
				    cams[i], fname.c_str())) {
				branch_output::setEnabled(f, true);
				obs_source_release(f);
			}
		}
	});
	obs_log(LOG_INFO, "[selftest] armed %d Branch Output filter(s)", armed);

	// Watch the recording folder so the gate also exercises file anchoring.
	{
		std::array<bool, kMaxSegmentCameras> segCams{};
		for (int i = 0; i < camCount && i < kMaxSegmentCameras; i++)
			segCams[i] = want[i];
		const int64_t epochWallNs =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::system_clock::now().time_since_epoch())
				.count();
		SegmentIndex::instance().start(projectFolder, segCams,
					       (int64_t)os_gettime_ns(), epochWallNs);
	}

	// Keep the synthetic cameras moving. A flat colour compresses to nearly
	// identical frames, which is exactly the case segment anchoring refuses
	// as ambiguous - so without motion the gate could never exercise it.
	std::atomic<bool> animate{!useRealSources};
	std::thread animator;
	if (animate.load()) {
		animator = std::thread([&]() {
			int tick = 0;
			while (animate.load()) {
				for (int i = 0; i < camCount; i++) {
					if (!cams[i])
						continue;
					obs_data_t *s = obs_data_create();
					const uint32_t c =
						0xFF000000u |
						(uint32_t)((tick * 2654435761u +
							    (uint32_t)i * 40503u) &
							   0x00FFFFFFu);
					obs_data_set_int(s, "color", c);
					obs_data_set_int(s, "width", cx);
					obs_data_set_int(s, "height", cy);
					obs_source_update(cams[i], s);
					obs_data_release(s);
				}
				tick++;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(66));
			}
		});
	}

	const uint32_t laggedBefore = obs_get_lagged_frames();
	const uint32_t totalBefore = obs_get_total_frames();

	RingBudget budget;
	budget.kbpsPerCamera = cfg.videoBitrateKbps + cfg.audioBitrateKbps;
	PacketTap::instance().armAsync(want, budget);

	// --- Measure ---------------------------------------------------------
	for (int s = 0; s < durationSecs; s++) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (s == 5 || s == durationSecs - 1)
			obs_log(LOG_INFO, "%s",
				PacketTap::instance().report().c_str());
	}

	animate.store(false);
	if (animator.joinable())
		animator.join();

	const int segmentsAnchored = SegmentIndex::instance().anchoredCount();
	const int segmentsUnanchored = SegmentIndex::instance().unanchoredCount();
	// Every recording file that appeared must have been placed on the
	// timeline exactly; anything left unanchored is footage we would refuse
	// to play rather than guess the position of.
	const bool segmentsOk =
		segmentsAnchored >= armed && segmentsUnanchored == 0;
	obs_log(LOG_INFO, "[selftest] segments anchored=%d unanchored=%d",
		segmentsAnchored, segmentsUnanchored);

	// --- The anchors must be ON DISK before the take ends -----------------
	// anchors.json is the only evidence a later run has: a file whose opening
	// has left the ring can never be re-anchored. It used to be written only
	// by stop(), so a take that ended with OBS killed or crashed left its
	// recordings with no anchor at all - which is why the operator's 18:27
	// take (183 MB per camera) can never be replayed. Read the real file,
	// mid-take, and count what is in it.
	int anchorsOnDisk = 0;
	{
		const std::string ap =
			(std::filesystem::path(projectFolder) / "anchors.json")
				.string();
		obs_data_t *root = obs_data_create_from_json_file(ap.c_str());
		if (root) {
			obs_data_array_t *arr = obs_data_get_array(root, "segments");
			if (arr) {
				anchorsOnDisk = (int)obs_data_array_count(arr);
				obs_data_array_release(arr);
			}
			obs_data_release(root);
		}
	}
	const bool anchorsPersisted = anchorsOnDisk >= armed;
	obs_log(anchorsPersisted ? LOG_INFO : LOG_ERROR,
		"[selftest] anchors.json holds %d segment(s) mid-take (%d armed) - "
		"this is what a project reopened tomorrow reads",
		anchorsOnDisk, armed);

	// --- Event timecodes have an origin that exists -----------------------
	// A mark is an absolute instant on a clock that started with OBS; it only
	// becomes a timecode relative to where the project's footage begins. That
	// origin must come from the anchored recordings (every camera, not the
	// selected one), or a reopened project prints marks as raw monotonic time
	// - five-digit minute counts, which is exactly what the operator saw.
	const int64_t projectOrigin = SegmentIndex::instance().projectOriginNs();
	const bool projectOriginOk = projectOrigin != kNoInstant;
	obs_log(projectOriginOk ? LOG_INFO : LOG_ERROR,
		"[selftest] project origin (oldest anchor on any camera) = %lld ms",
		(long long)(projectOrigin / 1000000));

	const uint32_t laggedAfter = obs_get_lagged_frames();
	const uint32_t totalAfter = obs_get_total_frames();
	const int64_t skewNs = PacketTap::instance().crossAngleSkewNs();
	const int attached = PacketTap::instance().attachedCount();

	std::vector<TapStats> stats;
	for (int i = 0; i < camCount; i++)
		stats.push_back(PacketTap::instance().stats(i));

	// --- M1 acceptance: the exact use case that was broken ----------------
	// Take the last 5 seconds off the live ring on every angle, with no file
	// access at all, and check that one marker resolves to the same instant
	// everywhere. This is "mark it and put it on air now" in miniature.
	const double frameMs = 1000.0 / (canvasFps > 0 ? canvasFps : 30.0);
	bool ringLast5s = armed > 0;
	bool decodeOk = armed > 0;
	bool startsOnMarkedFrame = armed > 0;
	int64_t crossAnglePresentDeltaMs = 0;
	std::vector<int64_t> presentIns;
	std::vector<int> decodedFrames;
	int64_t clipInNs = 0, clipOutNs = 0; // reused by the playback check below
	{
		auto &tap = PacketTap::instance();
		int64_t commonEdge = INT64_MAX;
		for (int i = 0; i < camCount; i++) {
			if (!want[i])
				continue;
			const int64_t n = tap.newestNs(i);
			if (n <= 0) {
				commonEdge = 0;
				break;
			}
			commonEdge = std::min(commonEdge, n);
		}

		if (commonEdge <= 0 || commonEdge == INT64_MAX) {
			ringLast5s = false;
			obs_log(LOG_ERROR, "[selftest] no live edge on the rings");
		} else {
			// Stay a couple of frames inside the edge: the newest
			// packet held may be audio, slightly ahead of the newest
			// decodable video frame.
			const int64_t marginNs =
				(int64_t)(2.0 * 1e9 / (canvasFps > 0 ? canvasFps : 30.0));
			const int64_t outNs = commonEdge - marginNs;
			const int64_t inNs = outNs - 5'000'000'000LL;
			clipInNs = inNs;
			clipOutNs = outNs;

			int64_t lo = INT64_MAX, hi = INT64_MIN;
			for (int i = 0; i < camCount; i++) {
				if (!want[i])
					continue;
				std::vector<LivePacket> clip;
				int64_t pIn = 0, pOut = 0;
				if (!tap.resolveRange(i, inNs, outNs, clip, pIn,
						      pOut)) {
					obs_log(LOG_ERROR,
						"[selftest] cam%d: the last 5 s did "
						"NOT resolve from the ring",
						i + 1);
					ringLast5s = false;
					continue;
				}
				presentIns.push_back(pIn);
				lo = std::min(lo, pIn);
				hi = std::max(hi, pIn);
				obs_log(LOG_INFO,
					"[selftest] cam%d: last 5 s resolved from the "
					"ring - %zu packets, in=%lld ms out=%lld ms",
					i + 1, clip.size(),
					(long long)(pIn / 1000000),
					(long long)(pOut / 1000000));

				// Decode it. Frames before the IN are decoded and
				// dropped on purpose: they only exist to build up
				// reference state from the keyframe, and dropping
				// them is what makes the clip start on the marked
				// frame instead of snapping back to the keyframe.
				ReplayDecoder dec;
				std::string derr;
				if (!dec.open(tap.streamConfig(i), derr)) {
					obs_log(LOG_ERROR,
						"[selftest] cam%d: decoder open failed: %s",
						i + 1, derr.c_str());
					decodeOk = false;
					continue;
				}

				int presented = 0, preroll = 0;
				int64_t firstPresentedNs = 0;
				ReplayDecoder::Frame f;
				const auto pull = [&]() {
					while (dec.receive(f)) {
						if (f.masterNs < pIn) {
							preroll++;
							continue;
						}
						if (presented == 0)
							firstPresentedNs = f.masterNs;
						presented++;
					}
				};
				bool sendOk = true;
				for (const auto &p : clip) {
					if (!dec.send(p, derr)) {
						obs_log(LOG_ERROR,
							"[selftest] cam%d: decode failed: %s",
							i + 1, derr.c_str());
						sendOk = false;
						break;
					}
					pull();
				}
				if (sendOk && dec.drain(derr))
					pull();

				decodedFrames.push_back(presented);
				// The whole point: the first frame we would put on
				// air is the frame that was marked, to the exact
				// timestamp - not the keyframe before it.
				if (presented == 0 || firstPresentedNs != pIn)
					startsOnMarkedFrame = false;

				const int expected =
					(int)(5.0 * (canvasFps > 0 ? canvasFps : 30.0));
				if (!sendOk || presented < expected * 9 / 10)
					decodeOk = false;

				obs_log(LOG_INFO,
					"[selftest] cam%d: decoded %d frames (%d dropped "
					"as pre-roll), first presented=%lld ms, "
					"requested in=%lld ms, %ux%u",
					i + 1, presented, preroll,
					(long long)(firstPresentedNs / 1000000),
					(long long)(pIn / 1000000), dec.width(),
					dec.height());
			}
			if (lo != INT64_MAX && hi != INT64_MIN)
				crossAnglePresentDeltaMs = (hi - lo) / 1000000;
		}
	}
	// One marker, one frame, every angle.
	const bool passRingCrossAngle =
		presentIns.size() < 2 ||
		(double)crossAnglePresentDeltaMs <= frameMs;

	// --- M2 acceptance: through the real OBS input, in slow motion --------
	// Play the same 5 seconds into "Replay A" at 50%. Two things get proven
	// at once: OBS accepts our frames (it reports the picture size back), and
	// the pacing is right (5 s of footage at half speed takes ~10 s).
	bool playsIntoObs = false;
	bool audioPlays = false;
	bool slowMotionPaced = false;
	bool filePlaysFromDisk = false;
	bool fileMatchesRing = false;
	// v1.3: backwards. Nothing decodes backwards, so this is a bounded GOP
	// cache shown newest-first — and both halves of that sentence are checks.
	bool reversePlaysBackwards = false;
	bool reverseCacheWithinBudget = false;
	int reverseFrames = 0;
	int reversePeakMiB = 0;
	int fileFrames = 0;
	int playedFrames = 0;
	int audioBuffers = 0;
	int64_t playElapsedMs = 0;
	int64_t slowElapsedMs = 0;
	if (ringLast5s && clipOutNs > clipInNs) {
		auto &chan = ReplayChannel::instance();
		int firstCam = -1;
		for (int i = 0; i < camCount; i++) {
			if (want[i]) {
				firstCam = i;
				break;
			}
		}

		obs_source_t *src = chan.acquireSource();
		if (firstCam < 0 || !src) {
			obs_log(LOG_ERROR,
				"[selftest] no replay source to play into");
		} else {
			// The source is in no scene, so OBS would not tick it and
			// would never consume the async frames we push.
			obs_source_inc_active(src);
			obs_source_inc_showing(src);

			// Pass 1 - the full five seconds at 1x, which is where
			// audio rides along.
			std::string perr;
			const uint64_t t0 = os_gettime_ns();
			if (!chan.play(firstCam, clipInNs, clipOutNs, 100, perr)) {
				obs_log(LOG_ERROR, "[selftest] play failed: %s",
					perr.c_str());
			} else {
				while (chan.playing())
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				playElapsedMs =
					(int64_t)((os_gettime_ns() - t0) / 1000000);

				const auto st = chan.stats();
				playedFrames = (int)st.framesPushed;
				audioBuffers = (int)st.audioPushed;
				const uint32_t w = obs_source_get_width(src);
				const uint32_t h = obs_source_get_height(src);

				// ~5 s expected; a wide band so a loaded laptop
				// cannot produce a false failure.
				const bool pacedRight = playElapsedMs > 4200 &&
							playElapsedMs < 7000;
				// Against the CAMERA's size, not the canvas. A
				// replay is the camera's own picture - OBS scales
				// it when it is composited - so a 720p source in a
				// 1080p project is correct, not a failure. The
				// synthetic cameras happen to be canvas-sized,
				// which is why this only showed up the moment the
				// gate was pointed at real footage.
				const StreamConfig scfg =
					PacketTap::instance().streamConfig(firstCam);
				playsIntoObs = st.lastRunCompleted &&
					       playedFrames > 0 &&
					       w == scfg.width &&
					       h == scfg.height && pacedRight;
				audioPlays = audioBuffers > 0;

				obs_log(LOG_INFO,
					"[selftest] played %d frames + %d audio buffers "
					"into '%s' at 1x in %lld ms (OBS reports %ux%u, "
					"%d preroll)",
					playedFrames, audioBuffers,
					ReplayChannel::sourceNameOf(Which::A),
					(long long)playElapsedMs, w, h,
					(int)st.framesPreroll);
			}

			// Pass 2 - two seconds at 50%, purely to prove the
			// slow-motion cadence is the speed we asked for.
			const int64_t slowInNs = clipOutNs - 2'000'000'000LL;
			const uint64_t t1 = os_gettime_ns();
			if (chan.play(firstCam, slowInNs, clipOutNs, 50, perr)) {
				while (chan.playing())
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				slowElapsedMs =
					(int64_t)((os_gettime_ns() - t1) / 1000000);
				slowMotionPaced = slowElapsedMs > 3400 &&
						  slowElapsedMs < 5200;
				obs_log(LOG_INFO,
					"[selftest] 2 s of footage at 50%% took %lld ms "
					"(%d frames)",
					(long long)slowElapsedMs,
					(int)chan.stats().framesPushed);
			} else {
				obs_log(LOG_ERROR,
					"[selftest] slow-motion pass failed: %s",
					perr.c_str());
			}

			// Pass 3 - the same footage again, but read from the
			// recording files instead of the ring. Two independent
			// paths must land on the same frame, or one of them is
			// lying. The window sits well back from the live edge so
			// it is certainly flushed to disk.
			const int64_t fileInNs = clipOutNs - 15'000'000'000LL;
			const int64_t fileOutNs = fileInNs + 5'000'000'000LL;

			std::vector<LivePacket> refClip;
			int64_t refIn = 0, refOut = 0;
			const bool haveRef = PacketTap::instance().resolveRange(
				firstCam, fileInNs, fileOutNs, refClip, refIn,
				refOut);

			if (chan.play(firstCam, fileInNs, fileOutNs, 100, perr,
				      ReplayChannel::Source::Segments)) {
				while (chan.playing())
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				const auto st = chan.stats();
				fileFrames = (int)st.framesPushed;
				const int expected =
					(int)(5.0 * (canvasFps > 0 ? canvasFps : 30.0));
				filePlaysFromDisk =
					st.lastRunCompleted &&
					fileFrames >= expected * 9 / 10;
				// The decisive comparison: the file path must
				// present the very frame the ring would have.
				// Within one frame, not to the nanosecond. The
				// reference is resolved a moment before playback
				// starts, and the live edge keeps advancing in
				// between, so demanding exact equality made this
				// fail roughly one run in three on footage that
				// was in fact identical. One frame still catches
				// a genuinely misplaced anchor, which is what
				// this is here to catch - those are seconds out,
				// not milliseconds.
				const int64_t frameNs =
					(int64_t)(1e9 / (canvasFps > 0 ? canvasFps
								       : 30.0));
				const int64_t delta = st.firstFrameNs > refIn
							      ? st.firstFrameNs - refIn
							      : refIn - st.firstFrameNs;
				fileMatchesRing = haveRef && delta <= frameNs;
				obs_log(LOG_INFO,
					"[selftest] from disk: %d frames, first=%lld ms; "
					"ring would present %lld ms (match: %s)",
					fileFrames,
					(long long)(st.firstFrameNs / 1000000),
					(long long)(refIn / 1000000),
					fileMatchesRing ? "yes" : "NO");
			} else {
				obs_log(LOG_ERROR,
					"[selftest] file playback failed: %s",
					perr.c_str());
			}

			// Pass 4 (v1.3) — the same two seconds, BACKWARDS.
			//
			// Three things are asserted at once, and none of them is
			// visible from outside the engine. That the pictures come
			// out newest-first (first > last, over more than one
			// picture, so it cannot be a one-frame coincidence). That
			// the schedule loses nothing: what the plan said it would
			// show is what OBS was handed, which is THE failure mode of
			// a GOP-cached reverse — a run that skips a slice still
			// plays, still runs backwards, and simply drops a third of
			// a second. And that the picture cache stayed inside its
			// budget, because "it works on a 32 GB desktop" is not the
			// claim.
			const int64_t revInNs = clipOutNs - 2'000'000'000LL;
			ReplayChannel::PlayRequest rev;
			rev.camIndex = firstCam;
			rev.inNs = revInNs;
			rev.outNs = clipOutNs;
			rev.speedPct = 100;
			rev.direction = ReplayChannel::Direction::Reverse;
			if (chan.play(rev, perr)) {
				while (chan.playing())
					std::this_thread::sleep_for(
						std::chrono::milliseconds(50));
				const auto st = chan.stats();
				reverseFrames = (int)st.framesPushed;
				reversePeakMiB =
					(int)(st.cacheBytesPeak / (1024 * 1024));
				reversePlaysBackwards =
					st.lastRunCompleted && st.reverse &&
					reverseFrames > 1 &&
					st.firstFrameNs > st.lastFrameNs &&
					st.framesPlanned > 1 &&
					reverseFrames == st.framesPlanned;
				reverseCacheWithinBudget =
					st.cacheBytesPeak > 0 &&
					st.cacheBytesPeak <=
						ReplayChannel::kReverseCacheBudgetBytes;
				obs_log(reversePlaysBackwards ? LOG_INFO : LOG_ERROR,
					"[selftest] reverse: %d picture(s) of %d "
					"planned, %lld ms → %lld ms, cache peak "
					"%d MiB (budget %d MiB)",
					reverseFrames, st.framesPlanned,
					(long long)(st.firstFrameNs / 1000000),
					(long long)(st.lastFrameNs / 1000000),
					reversePeakMiB,
					(int)(ReplayChannel::kReverseCacheBudgetBytes /
					      (1024 * 1024)));
			} else {
				obs_log(LOG_ERROR, "[selftest] reverse failed: %s",
					perr.c_str());
			}

			obs_source_dec_showing(src);
			obs_source_dec_active(src);
		}
		if (src)
			obs_source_release(src);
	}

	// --- The dock, on the same live ring ---------------------------------
	// Deliberately here, while everything is still armed and the ring is
	// full: the dock is only interesting against a running session, which is
	// exactly the state an operator has when he touches it.
	DockChecks dockChecks;
	{
		int firstArmed = -1, secondArmed = -1;
		for (int i = 0; i < camCount; i++) {
			if (!want[i])
				continue;
			if (firstArmed < 0)
				firstArmed = i;
			else if (secondArmed < 0)
				secondArmed = i;
		}
		if (firstArmed >= 0)
			dockChecks = runDockChecks(firstArmed, secondArmed,
						   projectFolder, canvasFps);
		else
			obs_log(LOG_ERROR, "[selftest] no armed camera — dock "
					   "checks skipped");
	}

	// The mark the dock just took, expressed the way the event table expresses
	// it: after the project's footage begins, and inside this take. A mark that
	// lands before the origin (or hours after it) is the "absurd timecode" a
	// reopened project showed, caught here instead of by eye.
	const int64_t markOffsetNs =
		projectOriginOk ? dockChecks.markInNs - projectOrigin : -1;
	const bool eventTimecodeSane =
		projectOriginOk && dockChecks.markInNs > 0 && markOffsetNs > 0 &&
		markOffsetNs < (int64_t)(durationSecs + 120) * 1000000000LL;
	obs_log(eventTimecodeSane ? LOG_INFO : LOG_ERROR,
		"[selftest] marked event sits %lld ms after the project origin",
		(long long)(markOffsetNs / 1000000));

	// --- The highlights reel: many clips, ONE file ------------------------
	// Exported from the events this run marked, into the test project's own
	// export folder. The claim is deliberately narrow — a file appears, it is
	// not a stub, and it is BIGGER than the single-clip export of the same
	// event, which is the cheapest way to say "more than one clip went in"
	// without decoding it here.
	bool reelWritten = false;
	// ...and it OPENS and plays back. See the read-back below: a reel with the
	// right size and unusable audio passed the old check.
	bool reelPlayable = false;
	std::string reelPath;
	int64_t reelBytes = 0;
	{
		// TWO events of its own, inside the window the playback checks
		// already proved is servable. Not the ones the dock checks made:
		// those are deleted again on purpose (they must not be left in
		// anybody's project), so reading them here found nothing — and a
		// reel of one clip would not be testing a reel anyway.
		auto &store = EventStore::instance();
		std::vector<int> ids;
		std::vector<int> madeHere;
		// The first camera this run armed: the one the playback checks
		// proved has footage in that window.
		int reelCam = 0;
		for (int i = 0; i < camCount; i++)
			if (want[i]) { reelCam = i; break; }
		if (clipInNs > 0 && clipOutNs > clipInNs + 3'000'000'000LL) {
			const int64_t mid = clipInNs + 2'000'000'000LL;
			const int first = store.markIn(clipInNs, reelCam);
			store.markOut(mid);
			const int second = store.markIn(mid, reelCam);
			store.markOut(mid + 1'500'000'000LL);
			for (int id : {first, second}) {
				if (id <= 0)
					continue;
				store.setAngle(id, reelCam + 1, true);
				ids.push_back(id);
				madeHere.push_back(id);
			}
		}
		const std::string reelFolder =
			(std::filesystem::path(projectFolder) / "reel").string();
		std::string rerr;
		if (!ids.empty() &&
		    ExportManager::instance().exportSequence(ids, false,
							     reelFolder, rerr)) {
			// WAIT FOR THE EXPORT, NOT FOR THE BYTES. The muxer
			// writes the trailer last — for MP4 the moov atom, which
			// is what makes the file openable at all — so a file that
			// is already there and already megabytes long is not yet
			// a file. Measured: this loop called "reel will not open"
			// 200 ms before the reel logged that it had written it,
			// and failed a run on a reel that was perfectly good.
			for (int i = 0; i < 120 &&
					ExportManager::instance().reelRunning();
			     i++)
				std::this_thread::sleep_for(
					std::chrono::milliseconds(250));
			{
				std::error_code fec;
				for (const auto &e : std::filesystem::directory_iterator(
					     reelFolder, fec)) {
					if (e.path().extension() != ".mp4")
						continue;
					const auto sz = std::filesystem::file_size(
						e.path(), fec);
					if (!fec && sz > 200 * 1024) {
						reelBytes = (int64_t)sz;
						reelPath = pathToUtf8(e.path());
						reelWritten = true;
						break;
					}
				}
			}
		} else if (!ids.empty()) {
			obs_log(LOG_ERROR, "[selftest] reel refused: %s",
				rerr.c_str());
		}
		// ...and now OPEN IT. "A file appeared and it is big" was the whole
		// claim, and it was not enough: the reel's audio timestamps were
		// rebased in the VIDEO stream's timebase and offset in it too, so
		// every audio packet came out of a subtraction between two different
		// units. The file was the right size, this check was green, and the
		// operator got a damaged reel. So the packets are read back and the
		// two properties a container must have are asserted: every stream's
		// DTS increases, and the video track is as long as the clips that
		// went into it. Neither can be true of a broken file.
		if (reelWritten && !reelPath.empty()) {
			AVFormatContext *in = nullptr;
			if (avformat_open_input(&in, reelPath.c_str(), nullptr,
						nullptr) < 0 ||
			    avformat_find_stream_info(in, nullptr) < 0) {
				obs_log(LOG_ERROR, "[selftest] reel will not open: %s",
					reelPath.c_str());
				if (in)
					avformat_close_input(&in);
			} else {
				std::vector<int64_t> lastDts(in->nb_streams,
							     INT64_MIN);
				bool monotonic = true;
				int64_t vEndUs = 0;
				int vIdx = -1;
				for (unsigned i = 0; i < in->nb_streams; i++)
					if (in->streams[i]->codecpar->codec_type ==
						    AVMEDIA_TYPE_VIDEO &&
					    vIdx < 0)
						vIdx = (int)i;
				AVPacket *pkt = av_packet_alloc();
				int64_t pkts = 0;
				while (pkt && av_read_frame(in, pkt) >= 0) {
					const int si = pkt->stream_index;
					if (pkt->dts != AV_NOPTS_VALUE) {
						if (pkt->dts < lastDts[si])
							monotonic = false;
						lastDts[si] = pkt->dts;
					}
					if (si == vIdx && pkt->pts != AV_NOPTS_VALUE)
						vEndUs = av_rescale_q(
							pkt->pts,
							in->streams[si]->time_base,
							AVRational{1, 1000000});
					pkts++;
					av_packet_unref(pkt);
				}
				if (pkt)
					av_packet_free(&pkt);
				const int nStreams = (int)in->nb_streams;
				avformat_close_input(&in);
				// The two events this check marks are 2.0 s and 1.5 s
				// of footage: ~3.5 s, and each clip may open up to a
				// GOP early (stated in runReel). A file a fraction of
				// that long is a file that lost a clip.
				reelPlayable = monotonic && pkts > 0 &&
					       nStreams > 0 && vEndUs > 2'500'000;
				obs_log(reelPlayable ? LOG_INFO : LOG_ERROR,
					"[selftest] reel opens: %d stream(s), %lld "
					"packet(s), video ends at %lld ms, DTS "
					"monotonic: %s",
					nStreams, (long long)pkts,
					(long long)(vEndUs / 1000),
					monotonic ? "yes" : "NO");
			}
		}
		obs_log(reelWritten ? LOG_INFO : LOG_ERROR,
			"[selftest] highlights reel from %zu event(s): %s (%lld KB)",
			ids.size(), reelWritten ? "written" : "NOT WRITTEN",
			(long long)(reelBytes / 1024));
		// The gate's own marks go away with it: this project is deleted
		// at the end of the reopen pass, but an event list left with two
		// events nobody made is still a lie while it lasts.
		for (int id : madeHere)
			store.remove(id);
	}

	// --- Tear down in the order the lifecycle demands ---------------------
	// Detach BEFORE disabling the filters: Branch Output frees its encoder
	// in releaseInfrastructureIfIdle() once its own outputs go idle.
	PacketTap::instance().detachAll();

	// --- Where does the footage on disk END? ------------------------------
	// A reopened project has anchors (beginnings) and no live edge, so until
	// the files were asked how long they are the timeline had no end at all:
	// the bar collapsed to zero and told the operator there was nothing to
	// scrub over hours of usable footage. That end is measured here, off the
	// real Branch Output recordings this run just wrote.
	//
	// Note what this check does NOT lean on: the dock's own bar is not
	// consulted, because the ring still holds this take's packets and would
	// hand it a live edge anyway. Only SegmentIndex::newestNs — which knows
	// nothing about the ring — can make the claim honestly.
	bool diskTimelineOk = false;
	int64_t diskSpanMs = 0;
	{
		int firstArmed = -1;
		for (int i = 0; i < camCount; i++)
			if (want[i]) {
				firstArmed = i;
				break;
			}
		const int64_t start = firstArmed >= 0
					      ? SegmentIndex::instance().oldestNs(
							firstArmed)
					      : kNoInstant;
		// The lengths are demuxed one file per ~2 s watcher pass, and a
		// length counts as settled only when two reads agree, so give it
		// a few passes rather than one.
		for (int i = 0; i < 20 && !diskTimelineOk; i++) {
			const int64_t end =
				firstArmed >= 0
					? SegmentIndex::instance().newestNs(firstArmed)
					: kNoInstant;
			if (start != kNoInstant && end != kNoInstant &&
			    end > start) {
				diskSpanMs = (end - start) / 1000000;
				// A take this gate runs is tens of seconds; one
				// second of span is far below that and far above
				// the rounding of a single fragment.
				diskTimelineOk = diskSpanMs >= 1000;
				if (diskTimelineOk)
					break;
			}
			std::this_thread::sleep_for(
				std::chrono::milliseconds(500));
		}
		obs_log(diskTimelineOk ? LOG_INFO : LOG_ERROR,
			"[selftest] footage on disk spans %lld ms measured from the "
			"files themselves (no live edge involved)",
			(long long)diskSpanMs);
	}

	SegmentIndex::instance().stop();

	// --- M4: hardening, on its own short take -----------------------------
	// After the measurement take is finished with (the tap is detached and
	// every disk measurement above is already made) and before the cameras
	// are taken away. The filters of this run are ended first, so the health
	// pass starts from the same state an operator does: nothing recording.
	HealthChecks healthChecks;
	{
		runOnUi([&]() {
			for (int i = 0; i < camCount; i++) {
				if (!want[i] || !cams[i])
					continue;
				const std::string fname =
					std::string(branch_output::kFilterNamePrefix) +
					std::to_string(i + 1);
				if (obs_source_t *f = obs_source_get_filter_by_name(
					    cams[i], fname.c_str())) {
					branch_output::setEnabled(f, false);
					obs_source_release(f);
				}
			}
		});
		// Branch Output closes its outputs on its own 1 s timer; starting a
		// new take on top of a half-closed one would measure the teardown.
		std::this_thread::sleep_for(std::chrono::seconds(3));
		healthChecks = runHealthChecks(cams, camCount, want);
	}

	// Tearing down a Branch Output filter destroys its QTimer, so this goes
	// back on the UI thread too.
	runOnUi([&]() {
		if (testScene) {
			obs_source_remove(obs_scene_get_source(testScene));
			obs_scene_release(testScene);
		}

		for (int i = 0; i < camCount; i++) {
			if (!cams[i])
				continue;
			const std::string fname =
				std::string(branch_output::kFilterNamePrefix) +
				std::to_string(i + 1);
			if (obs_source_t *f = obs_source_get_filter_by_name(
				    cams[i], fname.c_str())) {
				branch_output::setEnabled(f, false);
				obs_source_filter_remove(cams[i], f);
				obs_source_release(f);
			}
			if (owned[i]) {
				obs_source_dec_active(cams[i]);
				obs_source_dec_showing(cams[i]);
				obs_source_remove(cams[i]);
			}
			obs_source_release(cams[i]);
		}
	});

	// Hand the operator his project back. The test project itself is left on
	// disk on purpose: the reopen pass, in the next OBS process, is the one
	// that measures it and then deletes it. If that pass never runs, the next
	// first pass wipes the folder anyway.
	if (!prevProject.empty()) {
		std::string perr;
		bool restored = false;
		runOnUi([&]() {
			restored = ReplayCore::instance().openProject(prevProject,
								      perr);
		});
		obs_log(restored ? LOG_INFO : LOG_ERROR,
			"[selftest] operator's project '%s' restored: %s",
			prevProject.c_str(),
			restored ? "yes" : perr.c_str());
	}

	// --- Verdict ----------------------------------------------------------
	bool passAttached = attached == armed && armed > 0;
	bool passNoNewEncoder = armed > 0;
	bool passPackets = armed > 0;
	bool passClean = true;
	int64_t worstMaxAgeMs = 0;
	int64_t worstAvgAgeMs = 0;
	for (const auto &s : stats) {
		if (!want[s.camIndex])
			continue;
		if (!s.encoderWasAlreadyActive)
			passNoNewEncoder = false;
		if (s.videoPackets == 0)
			passPackets = false;
		// A packet we could not place on the shared clock, or an
		// unexplained break in a healthy session, means the timeline is
		// not trustworthy - which is the whole point of the rewrite.
		if (s.malformedPackets > 0 || s.discontinuities > 0)
			passClean = false;
		worstMaxAgeMs = std::max(worstMaxAgeMs, s.maxAgeUsec / 1000);
		worstAvgAgeMs = std::max(worstAvgAgeMs, s.avgAgeUsec / 1000);
	}
	// The live edge must beat the ~1 s fragment flush by a wide margin — that
	// flush, plus a reopen, is what this whole rewrite removed.
	//
	// Judged on the AVERAGE, and only sanity-bounded on the maximum. The
	// average is the engineering number: it has sat at 138-166 ms across
	// every run, on every source, and a real regression moves it. The maximum
	// is one worst packet in thirty seconds, and on a ULV laptop running its
	// third back-to-back gate it drifts with the machine's mood — 412 ms on a
	// run whose average was 149 ms and whose footage was perfect. A threshold
	// tight enough to catch that is a threshold that fails on load, and a gate
	// that cries wolf is a gate people stop reading. So: the average carries
	// the verdict at 250 ms, and the maximum only has to stay clear of the
	// fragment flush it replaced.
	const bool passLatency =
		passPackets && worstAvgAgeMs <= 250 && worstMaxAgeMs <= 700;
	// Sampling is asynchronous, so allow two frame times of apparent skew.
	const bool passSkew = attached < 2 ||
			      (double)(skewNs / 1000000) <= frameMs * 2.0;
	const uint32_t laggedDelta = laggedAfter - laggedBefore;
	const uint32_t totalDelta = totalAfter - totalBefore;
	const double laggedPct =
		totalDelta ? 100.0 * laggedDelta / totalDelta : 0.0;
	const bool passImpact = laggedPct <= 1.0;

	const bool pass = passAttached && passNoNewEncoder && passPackets &&
			  passLatency && passSkew && passImpact && passClean &&
			  ringLast5s && passRingCrossAngle && decodeOk &&
			  startsOnMarkedFrame && playsIntoObs && audioPlays &&
			  slowMotionPaced && segmentsOk && filePlaysFromDisk &&
			  fileMatchesRing && reversePlaysBackwards &&
			  reverseCacheWithinBudget &&
			  dockChecks.stepBackMovesPlayhead &&
			  dockChecks.reverseButtonPlaysBackwards &&
			  dockChecks.keyboardLayerWorks &&
			  dockChecks.continuePastOutExtends &&
			  dockChecks.pauseHoldsAndResumes &&
			  dockChecks.speedChangeKeepsPosition &&
			  dockChecks.selectionCuesEvent &&
			  dockChecks.angleKeysFollowCameras &&
			  dockChecks.clipBarSpansSequence &&
			  dockChecks.channelBIsOptional &&
			  dockChecks.releasesSourcesOnCleanup &&
			  dockChecks.found &&
			  dockChecks.pollRuns && dockChecks.pollResponsive &&
				  dockChecks.repaintRateSane &&
			  dockChecks.pollQuiet && dockChecks.markOnTimeline &&
			  dockChecks.playsMark && dockChecks.playheadInsideClip &&
			  dockChecks.multiAngleQueue &&
			  dockChecks.angleCombinations &&
			  dockChecks.angleChoiceRepeatable &&
			  dockChecks.singleNonFirstAnglePlays &&
			  dockChecks.queueAdvancesToSecond &&
			  dockChecks.previewHoldsSequence &&
			  dockChecks.followsLiveAfterSequence &&
			  dockChecks.scrubShowsFootage &&
			  dockChecks.singleHonoursRequestedAngle &&
			  dockChecks.singleReportsUnplayableAngle &&
			  dockChecks.markInheritsAngle && dockChecks.idPadded &&
			  dockChecks.doubleClickPlays &&
			  dockChecks.frameStepAdvances &&
			  dockChecks.multiviewBuilt &&
			  dockChecks.multiviewDisplaysLive &&
			  dockChecks.previewsNeverStarved &&
			  dockChecks.displaysNeverStranded &&
			  dockChecks.angleColumnsInTable &&
			  dockChecks.tableEditsAngleSpeed &&
			  dockChecks.searchRestoresCellValues &&
			  dockChecks.clipBarReportsOnAir &&
			  dockChecks.skipAdvancesQueue &&
			  dockChecks.freeReviewPlaysUnmarked &&
			  dockChecks.freeReviewIsSecondPlayFunction &&
			  dockChecks.stopKeyEndsPlayback &&
			  dockChecks.multiviewFollowsReview &&
			  dockChecks.multiviewReturnsToLive &&
			  dockChecks.manualReorderMovesRow &&
			  dockChecks.manualReorderDisablesAutoSort &&
			  dockChecks.listTabsFitTheirNames &&
			  dockChecks.listTabCountFollowsConfig &&
			  dockChecks.layoutOrderTopToBottom &&
			  dockChecks.seekbarGraduated && dockChecks.channelBIndependent &&
			  dockChecks.setupNotNeededWhenConfigured &&
			  dockChecks.swapMovesClip && dockChecks.trimMovedIn &&
			  dockChecks.seekbarZooms && healthChecks.bTakesItsOwnScene &&
			  dockChecks.dragMovesMarker &&
			  dockChecks.secondsHotkeyMovesPoint &&
			  anchorsPersisted && projectOriginOk &&
			  eventTimecodeSane && filtersIdleOutsideRec &&
			  diskTimelineOk && reelWritten && reelPlayable &&
			  healthChecks.ran &&
			  healthChecks.recRefusesImpossibleTake &&
			  healthChecks.preflightClearsTheRig &&
			  healthChecks.monitorSamplesTheTake &&
			  healthChecks.deadAngleReported &&
			  healthChecks.deadAngleIsNotFatal &&
			  healthChecks.findingsClearedAtStop;

	// --- Report -----------------------------------------------------------
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "verdict", pass ? "PASS" : "FAIL");
	obs_data_set_string(root, "plugin_version", PLUGIN_VERSION);
	obs_data_set_int(root, "cameras_armed", armed);
	obs_data_set_int(root, "cameras_attached", attached);
	obs_data_set_int(root, "duration_secs", durationSecs);
	obs_data_set_bool(root, "synthetic_sources", !useRealSources);
	obs_data_set_int(root, "canvas_width", cx);
	obs_data_set_int(root, "canvas_height", cy);
	obs_data_set_double(root, "canvas_fps", canvasFps);

	obs_data_t *checks = obs_data_create();
	// Configuring a camera is not arming it: only REC records.
	obs_data_set_bool(checks, "filters_idle_outside_rec", filtersIdleOutsideRec);
	obs_data_set_bool(checks, "all_channels_attached", passAttached);
	obs_data_set_bool(checks, "no_new_encoder_created", passNoNewEncoder);
	obs_data_set_bool(checks, "packets_received", passPackets);
	obs_data_set_bool(checks, "live_edge_latency_ok", passLatency);
	obs_data_set_bool(checks, "cross_angle_skew_ok", passSkew);
	obs_data_set_bool(checks, "obs_impact_ok", passImpact);
	obs_data_set_bool(checks, "timeline_clean", passClean);
	obs_data_set_bool(checks, "ring_serves_last_5s", ringLast5s);
	obs_data_set_bool(checks, "ring_cross_angle_same_frame", passRingCrossAngle);
	obs_data_set_bool(checks, "clip_decodes", decodeOk);
	obs_data_set_bool(checks, "clip_starts_on_marked_frame", startsOnMarkedFrame);
	obs_data_set_bool(checks, "plays_into_obs_source", playsIntoObs);
	obs_data_set_bool(checks, "audio_plays", audioPlays);
	obs_data_set_bool(checks, "slow_motion_paced", slowMotionPaced);
	// Every recording file that appeared must have been placed on the
	// timeline exactly; anything left unanchored is footage we would refuse
	// to play rather than guess the position of.
	obs_data_set_bool(checks, "segments_anchored", segmentsOk);
	// Written to anchors.json as they are established, not at STOP: a take
	// that ends with OBS killed still leaves replayable footage behind.
	obs_data_set_bool(checks, "anchors_persisted_during_take",
			  anchorsPersisted);
	// The footage on disk has an END, read from the files and not from the
	// live edge — which is what a reopened project has to scrub over.
	obs_data_set_bool(checks, "disk_footage_has_measured_end", diskTimelineOk);
	obs_data_set_int(root, "disk_footage_span_ms", diskSpanMs);
	// Event timecodes have an origin, and it is the project's footage.
	obs_data_set_bool(checks, "project_origin_from_anchors", projectOriginOk);
	obs_data_set_bool(checks, "event_timecode_sane", eventTimecodeSane);
	obs_data_set_bool(checks, "plays_from_disk", filePlaysFromDisk);
	obs_data_set_bool(checks, "disk_matches_ring", fileMatchesRing);
	obs_data_set_int(root, "disk_played_frames", fileFrames);
	// v1.3: backwards. The pictures descend and the plan loses none of them...
	obs_data_set_bool(checks, "reverse_plays_backwards", reversePlaysBackwards);
	// ...and the picture cache stayed inside the budget it was sized for. A
	// reverse that works by holding the whole clip works on the machine it was
	// written on and nowhere else.
	obs_data_set_bool(checks, "reverse_cache_within_budget",
			  reverseCacheWithinBudget);
	obs_data_set_int(root, "reverse_played_frames", reverseFrames);
	obs_data_set_int(root, "reverse_cache_peak_mib", reversePeakMiB);
	// The dock: found and clicked, not just compiled (see runDockChecks).
	obs_data_set_bool(checks, "dock_registered", dockChecks.found);
	obs_data_set_bool(checks, "dock_poll_runs", dockChecks.pollRuns);
	obs_data_set_bool(checks, "dock_repaint_rate_sane",
			  dockChecks.repaintRateSane);
	// The numbers beside the verdict, because "sane" is a threshold and the
	// next person to look at this will want to know how much room was left.
	obs_data_set_double(checks, "dock_seek_repaints_per_sec",
			    dockChecks.seekRepaintsPerSec);
	obs_data_set_double(checks, "dock_seek_suppressed_per_sec",
			    dockChecks.seekSuppressedPerSec);
	obs_data_set_double(checks, "dock_clip_repaints_per_sec",
			    dockChecks.clipRepaintsPerSec);
	obs_data_set_bool(checks, "dock_poll_responsive",
			  dockChecks.pollResponsive);
	obs_data_set_bool(checks, "dock_poll_no_errors", dockChecks.pollQuiet);
	obs_data_set_bool(checks, "dock_mark_on_timeline",
			  dockChecks.markOnTimeline);
	obs_data_set_bool(checks, "dock_plays_marked_event",
			  dockChecks.playsMark);
	obs_data_set_bool(checks, "dock_playhead_inside_clip",
			  dockChecks.playheadInsideClip);
	// One clip per enabled angle: a two-angle event is two clips, not one.
	obs_data_set_bool(checks, "dock_multi_angle_queue",
			  dockChecks.multiAngleQueue);
	// ...and the exact angle sequence, for every combination that matters:
	// both, a set with a hole in it, and one angle that is not the first.
	obs_data_set_bool(checks, "dock_angle_combinations",
			  dockChecks.angleCombinations);
	// The same event, replayed with a different choice each time.
	obs_data_set_bool(checks, "dock_angle_choice_repeatable",
			  dockChecks.angleChoiceRepeatable);
	obs_data_set_bool(checks, "dock_single_non_first_angle_plays",
			  dockChecks.singleNonFirstAnglePlays);
	// Clip 1 ends and clip 2 goes on air by itself, across the finish
	// callback and the generation filter.
	obs_data_set_bool(checks, "dock_queue_advances_to_second_angle",
			  dockChecks.queueAdvancesToSecond);
	// The preview belongs to the sequence: no live-camera flash between angles.
	obs_data_set_bool(checks, "dock_preview_holds_sequence",
			  dockChecks.previewHoldsSequence);
	// ...and the transport goes back to the live edge on its own afterwards.
	obs_data_set_bool(checks, "dock_follows_live_after_sequence",
			  dockChecks.followsLiveAfterSequence);
	// A scrub shows the footage at that instant, and still does once the
	// review has run out.
	obs_data_set_bool(checks, "dock_scrub_shows_footage",
			  dockChecks.scrubShowsFootage);
	// The angle button wins, and an angle with nothing behind it says so.
	obs_data_set_bool(checks, "single_mode_honours_requested_angle",
			  dockChecks.singleHonoursRequestedAngle);
	obs_data_set_bool(checks, "single_mode_reports_unplayable_angle",
			  dockChecks.singleReportsUnplayableAngle);
	// A mark flags the angle the operator is watching, not always the first.
	obs_data_set_bool(checks, "dock_mark_inherits_current_angle",
			  dockChecks.markInheritsAngle);
	// reference parity (M3): ids are padded to the configured width, a double-click
	// puts the row on air, and the frame step really advances the picture.
	obs_data_set_bool(checks, "dock_event_id_padded", dockChecks.idPadded);
	obs_data_set_bool(checks, "dock_double_click_plays_event",
			  dockChecks.doubleClickPlays);
	obs_data_set_bool(checks, "dock_frame_step_advances",
			  dockChecks.frameStepAdvances);
	// v1.3: and the two backwards keys, clicked for real. The step back is
	// measured against the frame the forward steps left on screen — the naive
	// implementation lands on that very frame, and would pass a check that only
	// asked whether a clip ran.
	obs_data_set_bool(checks, "dock_step_back_moves_playhead",
			  dockChecks.stepBackMovesPlayhead);
	obs_data_set_bool(checks, "dock_reverse_button_plays_backwards",
			  dockChecks.reverseButtonPlaysBackwards);
	// ONE layer of input: a real key event into the dock steps a frame and walks
	// the list. Sent as an event on purpose — the table and the focused buttons
	// are what a direct call to the handler cannot see.
	obs_data_set_bool(checks, "dock_keyboard_layer_works",
			  dockChecks.keyboardLayerWorks);
	// Continuing past the OUT lengthens the LAST clip of the queue, by what the
	// green band is counting down.
	obs_data_set_bool(checks, "continue_past_out_extends",
			  dockChecks.continuePastOutExtends);
	obs_data_set_int(root, "continue_past_out_extra_ms",
			 dockChecks.continueExtraMs);
	// The transport, after the fixes: pause holds the frame and play carries on
	// FROM it, a speed change re-spaces the clip instead of restarting it,
	// picking a row loads that event, the angle keys are the configured cameras
	// by name, and the green band spans the whole queue.
	obs_data_set_bool(checks, "dock_pause_holds_and_resumes",
			  dockChecks.pauseHoldsAndResumes);
	obs_data_set_bool(checks, "dock_speed_change_keeps_position",
			  dockChecks.speedChangeKeepsPosition);
	obs_data_set_bool(checks, "dock_selection_cues_event",
			  dockChecks.selectionCuesEvent);
	obs_data_set_bool(checks, "dock_angle_keys_follow_cameras",
			  dockChecks.angleKeysFollowCameras);
	obs_data_set_int(root, "dock_visible_angle_keys",
			 dockChecks.visibleAngleKeys);
	obs_data_set_bool(checks, "dock_clip_bar_spans_sequence",
			  dockChecks.clipBarSpansSequence);
	// The second bay is optional and off by default: absent, not greyed out.
	obs_data_set_bool(checks, "dock_channel_b_is_optional",
			  dockChecks.channelBIsOptional);
	// ...and it lets go of every source before OBS clears scene data.
	obs_data_set_bool(checks, "dock_releases_sources_on_cleanup",
			  dockChecks.releasesSourcesOnCleanup);
	// M5: a preview per angle plus the replay, each with a live display, and
	// none of them stranded on a dead native window.
	obs_data_set_bool(checks, "dock_multiview_built",
			  dockChecks.multiviewBuilt);
	obs_data_set_bool(checks, "dock_multiview_displays_live",
			  dockChecks.multiviewDisplaysLive);
	// M6: ...and none of them spent the run waiting for a display that never
	// came. The check above cannot see that on its own — see PreviewStats.
	obs_data_set_bool(checks, "dock_previews_never_starved",
			  dockChecks.previewsNeverStarved);
	obs_data_set_bool(checks, "dock_displays_never_stranded",
			  dockChecks.displaysNeverStranded);
	// M5: the per-angle triplet is in the table, and there is no per-event
	// speed column left for it to be confused with.
	obs_data_set_bool(checks, "dock_angle_columns_in_table",
			  dockChecks.angleColumnsInTable);
	obs_data_set_bool(checks, "dock_table_edits_angle_speed",
			  dockChecks.tableEditsAngleSpeed);
	obs_data_set_bool(checks, "dock_search_restores_cell_values",
			  dockChecks.searchRestoresCellValues);
	// The two sides of it, because "false" alone does not say whether the
	// cell went blank or showed somebody else's value.
	obs_data_set_string(checks, "dock_search_cell_note",
			    dockChecks.searchCellNote.c_str());
	obs_data_set_string(checks, "dock_search_stored_note",
			    dockChecks.searchStoredNote.c_str());
	obs_data_set_int(checks, "dock_search_cell_pct",
			 dockChecks.searchCellPct);
	obs_data_set_int(checks, "dock_search_stored_pct",
			 dockChecks.searchStoredPct);
	// M5: the green band describes the clip ON AIR, and >> takes the next
	// item of the queue instead of killing the sequence.
	obs_data_set_bool(checks, "dock_clip_bar_reports_on_air",
			  dockChecks.clipBarReportsOnAir);
	obs_data_set_bool(checks, "dock_skip_advances_queue",
			  dockChecks.skipAdvancesQueue);
	// v1.4: footage nobody marked can be watched, and only Play events can
	// put it up. The off-air half is the one that matters: it is what keeps a
	// review of an unmarked action from reaching Program by itself.
	obs_data_set_bool(checks, "free_review_plays_unmarked_footage",
			  dockChecks.freeReviewPlaysUnmarked);
	obs_data_set_bool(checks, "free_review_is_the_second_play_function",
			  dockChecks.freeReviewIsSecondPlayFunction);
	obs_data_set_bool(checks, "stop_key_ends_playback",
			  dockChecks.stopKeyEndsPlayback);
	// v1.4: the angle boxes show the reviewed moment on every lens, and Live
	// is what gives them back to the cameras.
	obs_data_set_bool(checks, "multiview_follows_review",
			  dockChecks.multiviewFollowsReview);
	obs_data_set_bool(checks, "multiview_returns_to_live",
			  dockChecks.multiviewReturnsToLive);
	// Measurements, deliberately NOT in `checks` — see the fields for why
	// neither can be asserted from here. `..._gap_ms` is how far apart the bay
	// and the boxes get their picture on a cue; `..._dark_ms` is how long a
	// tile's source stays withdrawn while it happens. Both should be at or
	// near zero; a run where they are not is worth reading the log over.
	obs_data_set_int(root, "multiview_cue_dark_ms",
			 dockChecks.multiviewCueDarkMs);
	obs_data_set_int(root, "multiview_cue_gap_ms",
			 dockChecks.multiviewCueGapMs);
	obs_data_set_int(root, "cue_bay_min_ms", dockChecks.cueBayMinMs);
	obs_data_set_int(root, "cue_bay_max_ms", dockChecks.cueBayMaxMs);
	obs_data_set_int(root, "cue_tile_min_ms", dockChecks.cueTileMinMs);
	obs_data_set_int(root, "cue_tile_max_ms", dockChecks.cueTileMaxMs);
	obs_data_set_int(root, "multiview_feeds", dockChecks.multiviewFeeds);
	obs_data_set_int(root, "multiview_feeds_with_picture",
			 dockChecks.multiviewFeedsWithPicture);
	// M5: the running order is arranged by hand and it really moves the row.
	obs_data_set_bool(checks, "dock_manual_reorder_moves_row",
			  dockChecks.manualReorderMovesRow);
	obs_data_set_bool(checks, "dock_manual_reorder_leaves_autosort_off",
			  dockChecks.manualReorderDisablesAutoSort);
	// M5: a renamed list is drawn in full, and the tab count is the operator's.
	obs_data_set_bool(checks, "dock_list_tabs_fit_their_names",
			  dockChecks.listTabsFitTheirNames);
	obs_data_set_bool(checks, "dock_list_tab_count_follows_config",
			  dockChecks.listTabCountFollowsConfig);
	obs_data_set_int(root, "dock_visible_list_tabs", dockChecks.visibleListTabs);
	// M6: the pictures come first and the list header sits between them and
	// the table; the position bar is a graduated scale, not a rectangle.
	obs_data_set_bool(checks, "dock_layout_order_top_to_bottom",
			  dockChecks.layoutOrderTopToBottom);
	obs_data_set_bool(checks, "dock_seekbar_graduated",
			  dockChecks.seekbarGraduated);
	obs_data_set_int(root, "dock_seekbar_graduations", dockChecks.seekGraduations);
	obs_data_set_int(root, "dock_seekbar_height_px", dockChecks.seekHeight);
	// M4: pre-flight refuses what cannot work and clears what can; the monitor
	// watches the take; a camera that dies is named, and nothing else happens.
	// Channel B, the swap, the trim keys and the zoom.
	obs_data_set_bool(checks, "channel_b_plays_on_its_own",
			  dockChecks.channelBIndependent);
	obs_data_set_bool(checks, "setup_not_needed_when_configured",
			  dockChecks.setupNotNeededWhenConfigured);
	obs_data_set_bool(checks, "swap_moves_the_clip_across",
			  dockChecks.swapMovesClip);
	obs_data_set_bool(checks, "trim_moves_the_in_point", dockChecks.trimMovedIn);
	obs_data_set_bool(checks, "seekbar_zooms", dockChecks.seekbarZooms);
	obs_data_set_bool(checks, "channel_b_takes_its_own_scene",
			  healthChecks.bTakesItsOwnScene);
	obs_data_set_bool(checks, "seekbar_drag_moves_event_point",
			  dockChecks.dragMovesMarker);
	obs_data_set_bool(checks, "trim_hotkey_moves_by_seconds",
			  dockChecks.secondsHotkeyMovesPoint);
	obs_data_set_int(root, "channel_b_frames", dockChecks.channelBFrames);
	obs_data_set_double(root, "seekbar_zoom_reached", dockChecks.zoomReached);
	obs_data_set_bool(checks, "reel_exports_one_file", reelWritten);
	// ...and the file it wrote actually opens, with monotonic timestamps and the
	// length the clips add up to.
	obs_data_set_bool(checks, "reel_plays_back", reelPlayable);
	obs_data_set_int(root, "reel_bytes", reelBytes);
	obs_data_set_bool(checks, "preflight_refuses_impossible_take",
			  healthChecks.recRefusesImpossibleTake);
	obs_data_set_bool(checks, "preflight_clears_the_rig",
			  healthChecks.preflightClearsTheRig);
	obs_data_set_bool(checks, "health_monitor_samples_the_take",
			  healthChecks.monitorSamplesTheTake);
	obs_data_set_bool(checks, "dead_angle_is_reported",
			  healthChecks.deadAngleReported);
	// The M4 rule itself: degradation is visible and NEVER touches Program.
	obs_data_set_bool(checks, "dead_angle_never_touches_program",
			  healthChecks.deadAngleIsNotFatal);
	obs_data_set_bool(checks, "health_findings_cleared_at_stop",
			  healthChecks.findingsClearedAtStop);
	obs_data_set_obj(root, "checks", checks);
	obs_data_release(checks);

	obs_data_set_int(root, "health_samples", healthChecks.samples);
	obs_data_set_int(root, "health_dead_angle_detect_ms", healthChecks.detectMs);
	obs_data_set_string(root, "health_dead_angle_finding",
			    healthChecks.deadFinding.c_str());
	obs_data_set_string(root, "health_rec_refusal", healthChecks.refusal.c_str());
	obs_data_set_string(root, "health_program_scene",
			    healthChecks.sceneAfter.c_str());

	obs_data_set_int(root, "segments_anchored", segmentsAnchored);
	obs_data_set_int(root, "anchors_on_disk_mid_take", anchorsOnDisk);
	obs_data_set_int(root, "project_origin_ms",
			 projectOriginOk ? projectOrigin / 1000000 : 0);
	obs_data_set_int(root, "mark_offset_from_origin_ms",
			 markOffsetNs / 1000000);
	obs_data_set_int(root, "segments_unanchored", segmentsUnanchored);
	obs_data_set_int(root, "played_frames", playedFrames);
	obs_data_set_int(root, "played_audio_buffers", audioBuffers);
	obs_data_set_int(root, "played_elapsed_ms_at_1x", playElapsedMs);
	obs_data_set_int(root, "slow_2s_elapsed_ms_at_50pct", slowElapsedMs);

	obs_data_array_t *decArr = obs_data_array_create();
	for (int n : decodedFrames) {
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "frames", n);
		obs_data_array_push_back(decArr, d);
		obs_data_release(d);
	}
	obs_data_set_array(root, "decoded_frames_per_angle", decArr);
	obs_data_array_release(decArr);

	obs_data_set_int(root, "ring_cross_angle_present_delta_ms",
			 crossAnglePresentDeltaMs);

	obs_data_set_int(root, "dock_poll_ticks_in_2s", dockChecks.ticks);
	obs_data_set_int(root, "dock_worst_poll_gap_ms", dockChecks.worstGapMs);
	obs_data_set_int(root, "dock_plugin_errors_logged", dockChecks.logErrors);
	obs_data_set_int(root, "dock_marked_in_ms", dockChecks.markInNs / 1000000);
	obs_data_set_int(root, "dock_marked_out_ms",
			 dockChecks.markOutNs / 1000000);
	obs_data_set_int(root, "dock_played_frames", dockChecks.playedFrames);
	obs_data_set_int(root, "dock_two_angle_clips_queued",
			 dockChecks.queuedClips);
	obs_data_set_int(root, "dock_preview_live_samples_during_sequence",
			 dockChecks.previewLiveSamples);
	obs_data_set_int(root, "dock_preview_tiles", dockChecks.previewTiles);
	obs_data_set_int(root, "dock_preview_tiles_visible",
			 dockChecks.previewTilesVisible);
	obs_data_set_int(root, "dock_preview_tiles_with_display",
			 dockChecks.previewTilesWithDisplay);
	obs_data_set_int(root, "dock_preview_tiles_starved",
			 dockChecks.previewTilesStarved);
	obs_data_set_int(root, "dock_worst_display_wait_ms",
			 dockChecks.worstDisplayWaitMs);
	obs_data_set_int(root, "obs_displays_created", dockChecks.displaysCreated);
	obs_data_set_int(root, "obs_displays_forced_past_exposure",
			 dockChecks.displaysForced);
	obs_data_set_int(root, "obs_displays_stranded",
			 dockChecks.displaysStranded);
	obs_data_set_int(root, "obs_displays_reparented",
			 dockChecks.displaysReparented);
	obs_data_set_int(root, "dock_event_table_columns",
			 dockChecks.eventTableColumns);

	obs_data_set_int(root, "worst_max_packet_age_ms", worstMaxAgeMs);
	// The number the verdict is actually made on — see passLatency.
	obs_data_set_int(root, "worst_avg_packet_age_ms", worstAvgAgeMs);
	obs_data_set_int(root, "cross_angle_skew_ms", skewNs / 1000000);
	obs_data_set_int(root, "obs_lagged_frames_delta", laggedDelta);
	obs_data_set_int(root, "obs_total_frames_delta", totalDelta);
	obs_data_set_double(root, "obs_lagged_pct", laggedPct);

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &s : stats) {
		if (!want[s.camIndex])
			continue;
		obs_data_t *c = obs_data_create();
		obs_data_set_int(c, "cam", s.camIndex + 1);
		obs_data_set_string(c, "bo_output_name", s.outputName.c_str());
		obs_data_set_string(c, "encoder_id", s.encoderId.c_str());
		obs_data_set_string(c, "encoder_name", s.encoderName.c_str());
		obs_data_set_bool(c, "encoder_was_already_active",
				  s.encoderWasAlreadyActive);
		obs_data_set_int(c, "video_packets", (long long)s.videoPackets);
		obs_data_set_int(c, "keyframes", (long long)s.keyframes);
		obs_data_set_int(c, "audio_packets", (long long)s.audioPackets);
		obs_data_set_int(c, "video_bytes", (long long)s.videoBytes);
		obs_data_set_int(c, "first_packet_wait_ms", s.firstPacketWaitMs);
		obs_data_set_int(c, "packet_age_avg_ms", s.avgAgeUsec / 1000);
		obs_data_set_int(c, "packet_age_max_ms", s.maxAgeUsec / 1000);
		obs_data_set_int(c, "ring_packets", (long long)s.ringPackets);
		obs_data_set_int(c, "ring_mb", (long long)(s.ringBytes / (1024 * 1024)));
		obs_data_set_int(c, "ring_span_ms", s.ringSpanNs / 1000000);
		obs_data_set_int(c, "ring_evicted", (long long)s.evictedPackets);
		obs_data_set_int(c, "malformed_packets", (long long)s.malformedPackets);
		obs_data_set_int(c, "discontinuities", (long long)s.discontinuities);
		obs_data_array_push_back(arr, c);
		obs_data_release(c);
	}
	obs_data_set_array(root, "channels", arr);
	obs_data_array_release(arr);

	if (!obs_data_save_json_safe(root, outPath.c_str(), "tmp", "bak"))
		obs_log(LOG_ERROR, "[selftest] could not write report to %s",
			outPath.c_str());
	obs_log(LOG_INFO, "[selftest] VERDICT=%s — report written to %s",
		pass ? "PASS" : "FAIL", outPath.c_str());
	obs_data_release(root);

	// The harness takes its own furniture back out of the operator's scenes.
	// Left in, it killed OBS on the way out of every run since the day it was
	// added — see the note on g_seededItems. After the verdict, so a crash
	// here could never cost the report, and on the UI thread because that is
	// where scene items are touched.
	runOnUi([]() { removeSeededSceneItems(); });

	// There is no public obs_frontend quit API, so the runner script closes
	// OBS once this report file appears. Writing it is the "done" signal.
}

} // namespace

void maybeRunSelfTest()
{
	if (!envOn("OBS_MULTIREPLAY_SELFTEST"))
		return;
	if (g_started.exchange(true))
		return; // FINISHED_LOADING can fire more than once

	// FINISHED_LOADING IS NOT "OBS HAS FINISHED LOADING". It is raised from
	// inside OBSBasic::OBSInit, which then carries on building docks — and one
	// of those, the YouTube panel, spins a NESTED Qt event loop
	// (InitBrowserPanelSafeBlock → ExecThreadedWithoutBlocking). Every UI task
	// this gate posts is delivered inside that loop, so the project setup below
	// reconfigures Branch Output filters while OBSBasic is still half-built,
	// and Branch Output's own status dock — which rebuilds its table from
	// queued filter add/remove signals, holding raw source pointers across the
	// queue — dereferences a source we have already let go:
	//
	//   obs.dll!obs_source_get_name
	//   osi-branch-output.dll!OutputTableRow::OutputTableRow
	//   osi-branch-output.dll!BranchOutputStatusDock::addRow
	//
	// Three OBS processes died that way in a row on the day the YouTube panel
	// was slow to answer (the log shows "No functional TLS backend was found"
	// before each). The defect is Branch Output's, and it is not ours to fix,
	// but a gate that cannot survive its own start-up is a gate that cannot
	// tell anyone anything. Waiting a beat costs the run two seconds and puts
	// the setup after the nested loop has drained.
	//
	// The wait happens on the GATE's thread, never here: this function is
	// called from the frontend event handler, i.e. on the UI thread, and
	// sleeping on the UI thread to avoid a UI-thread race would be a fine
	// piece of comedy.
	const auto settle = []() {
		std::this_thread::sleep_for(std::chrono::seconds(2));
	};

	// The soak pass (M4) is its own OBS process too, and it is the long one:
	// it records for as many minutes as it is given, in a project of its own,
	// which it deletes afterwards. Opt-in only.
	if (envOn("OBS_MULTIREPLAY_SELFTEST_SOAK")) {
		const int minutes =
			std::max(1, envInt("OBS_MULTIREPLAY_SELFTEST_SOAK_MIN", 30));
		std::string out = envStr("OBS_MULTIREPLAY_SELFTEST_OUT");
		if (out.empty()) {
			std::error_code ec;
			out = (std::filesystem::temp_directory_path(ec) /
			       "obs-multireplay-selftest-soak.json")
				      .string();
		}
		std::thread([minutes, out, settle]() {
			settle();
			runSoakPass(minutes, out);
		}).detach();
		return;
	}

	// The reopen pass is a second OBS process on the folder the first one
	// left behind (see runReopenPass). It records nothing, so it must not go
	// anywhere near the code above.
	if (envOn("OBS_MULTIREPLAY_SELFTEST_REOPEN")) {
		std::string out = envStr("OBS_MULTIREPLAY_SELFTEST_OUT");
		if (out.empty()) {
			std::error_code ec;
			out = (std::filesystem::temp_directory_path(ec) /
			       "obs-multireplay-selftest-reopen.json")
				      .string();
		}
		std::thread([out, settle]() {
			settle();
			runReopenPass(out);
		}).detach();
		return;
	}

	std::thread([settle]() {
		settle();
		runSelfTest();
	}).detach();
}

} // namespace multireplay
