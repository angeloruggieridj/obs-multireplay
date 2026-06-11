/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

Registers the "Replay A" / "Replay B" async video sources. Add them to any
OBS scene; the ReplayEngine pushes decoded frames into them.
*/

#include "replay-player.hpp"
#include "plugin-support.h"

#include <obs-module.h>

namespace multireplay {

namespace {

struct ReplaySourceContext {
	obs_source_t *source;
	char channel;
};

const char *getNameA(void *)
{
	return obs_module_text("ReplaySource.A");
}

const char *getNameB(void *)
{
	return obs_module_text("ReplaySource.B");
}

void *createForChannel(obs_source_t *source, char channel)
{
	auto *ctx = new ReplaySourceContext{source, channel};
	if (ReplayPlayer *player = ReplayEngine::instance().channel(channel))
		player->attachSource(source);
	return ctx;
}

void *createA(obs_data_t *, obs_source_t *source)
{
	return createForChannel(source, 'A');
}

void *createB(obs_data_t *, obs_source_t *source)
{
	return createForChannel(source, 'B');
}

void destroy(void *data)
{
	auto *ctx = static_cast<ReplaySourceContext *>(data);
	if (ReplayPlayer *player =
		    ReplayEngine::instance().channel(ctx->channel))
		player->detachSource(ctx->source);
	delete ctx;
}

obs_source_info makeInfo(const char *id, const char *(*getName)(void *),
			 void *(*create)(obs_data_t *, obs_source_t *))
{
	obs_source_info info = {};
	info.id = id;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			    OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = getName;
	info.create = create;
	info.destroy = destroy;
	return info;
}

obs_source_info infoA;
obs_source_info infoB;

} // namespace

void registerReplaySources()
{
	infoA = makeInfo("multireplay_replay_a", getNameA, createA);
	infoB = makeInfo("multireplay_replay_b", getNameB, createB);
	obs_register_source(&infoA);
	obs_register_source(&infoB);
}

} // namespace multireplay
