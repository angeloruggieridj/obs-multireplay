/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Text drawn INTO the picture of an obs_display, the way a replay server prints
its status on the monitor image (event, time of day, camera, speed, REC).

Why this is not a QLabel: an OBSQTDisplay is a native window that libobs
presents into. Qt widgets cannot be stacked over it. The text therefore has to
be drawn inside the libobs draw callback, with gs_* calls, on the one graphics
thread — after the source has been rendered.

Two threads, one rule each:
  * setContent() runs on the UI thread. It does all the QPainter work (text ->
    small premultiplied images) and only takes the mutex to swap them in.
  * draw() runs on the graphics thread. It never touches Qt: it uploads a
    pending image as a texture (only when something changed) and draws sprites.

The time of day changes every frame, so it is NOT re-rendered: eleven small images
(digits and colon) are rendered once per size, and the time is composed from
them character by character.
*/

#pragma once

#include <obs.h>

#include <QImage>
#include <QString>
#include <array>
#include <mutex>

namespace multireplay {

// The rectangle (in display pixels, origin top-left) the picture occupies
// after letterboxing. {0,0,0,0} = nothing was drawn.
struct FitRect {
	int x = 0, y = 0, w = 0, h = 0;
	bool valid() const { return w > 0 && h > 0; }
};

class DisplayOverlay {
public:
	struct Content {
		bool visible = false;
		QString topLeft;  // event id (bays) / camera number (tiles)
		bool rec = false; // red dot, top right
		QString time;     // "HH:MM:SS:FF", empty = nothing
		QString name;     // camera name, bottom centre
		QString rate;     // "100%", bottom right
	};

	DisplayOverlay() = default;
	~DisplayOverlay();
	DisplayOverlay(const DisplayOverlay &) = delete;
	DisplayOverlay &operator=(const DisplayOverlay &) = delete;

	// UI thread. `pictureHeightPx` is the height of the picture in device
	// pixels; the text scales with it.
	void setContent(const Content &c, int pictureHeightPx);

	// Graphics thread, inside the display's draw callback, after the source.
	void draw(const FitRect &r);

private:
	// Slots from Glyph0 on hold "0".."9" and ":", one small texture each.
	enum Slot { TopLeft, Dot, Name, Rate, Glyph0, kSlots = Glyph0 + 11 };
	struct Tex {
		QImage pending; // set by the UI thread, consumed by draw()
		bool dirty = false;
		gs_texture_t *tex = nullptr;
		int w = 0, h = 0;
	};

	void setImage(Slot s, const QImage &img); // mutex held
	void freeTextures();                      // graphics thread

	std::mutex mu_;
	Content cur_;
	int px_ = 0; // font pixel size the images were rendered for
	std::array<Tex, kSlots> t_{};
	int cellW_ = 0; // atlas cell width (one digit)
	int cellH_ = 0;
};

} // namespace multireplay
