/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "web-server.hpp"
#include "replay-core.hpp"
#include "replay-player.hpp"
#include "plugin-support.h"

#include <obs-module.h>

// cpp-httplib (MIT) — HTTP only, no TLS needed on the local network for M1.
#define CPPHTTPLIB_NO_EXCEPTIONS
#include "third-party/httplib.h"

namespace multireplay {

namespace {

void jsonResponse(httplib::Response &res, const std::string &json, int status = 200)
{
	res.status = status;
	res.set_content(json, "application/json");
}

void errorResponse(httplib::Response &res, const std::string &message, int status = 400)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "ok", false);
	obs_data_set_string(data, "error", message.c_str());
	jsonResponse(res, obs_data_get_json(data), status);
	obs_data_release(data);
}

void okResponse(httplib::Response &res)
{
	jsonResponse(res, "{\"ok\":true}");
}

} // namespace

WebServer &WebServer::instance()
{
	static WebServer server;
	return server;
}

void WebServer::setupRoutes()
{
	auto &core = ReplayCore::instance();

	// --- Status / introspection ---
	server_->Get("/api/status",
		     [&core](const httplib::Request &, httplib::Response &res) {
			     jsonResponse(res, core.statusJson());
		     });
	server_->Get("/api/sources",
		     [&core](const httplib::Request &, httplib::Response &res) {
			     jsonResponse(res, core.sourcesJson());
		     });
	server_->Get("/api/encoders",
		     [&core](const httplib::Request &, httplib::Response &res) {
			     jsonResponse(res, core.encodersJson());
		     });

	// --- Recording control ---
	server_->Post("/api/recording/start",
		      [&core](const httplib::Request &, httplib::Response &res) {
			      std::string err;
			      if (core.startRecording(err))
				      okResponse(res);
			      else
				      errorResponse(res, err);
		      });
	server_->Post("/api/recording/stop",
		      [&core](const httplib::Request &, httplib::Response &res) {
			      core.stopRecording();
			      okResponse(res);
		      });

	// --- Config ---
	server_->Get("/api/config",
		     [&core](const httplib::Request &, httplib::Response &res) {
			     Config cfg = core.getConfig();
			     obs_data_t *data = obs_data_create();
			     obs_data_set_string(data, "sessionFolder",
						 cfg.sessionFolder.c_str());
			     obs_data_set_int(data, "port", cfg.port);
			     obs_data_set_int(data, "splitMinutes",
					      cfg.splitMinutes);
			     obs_data_set_int(data, "videoBitrateKbps",
					      cfg.videoBitrateKbps);
			     obs_data_set_int(data, "audioBitrateKbps",
					      cfg.audioBitrateKbps);
			     obs_data_set_string(data, "videoEncoderId",
						 cfg.videoEncoderId.c_str());
			     obs_data_set_string(data, "recFormat",
						 cfg.recFormat.c_str());
			     obs_data_array_t *cams = obs_data_array_create();
			     for (const auto &cam : cfg.cameras) {
				     obs_data_t *item = obs_data_create();
				     obs_data_set_string(
					     item, "sourceName",
					     cam.sourceName.c_str());
				     obs_data_array_push_back(cams, item);
				     obs_data_release(item);
			     }
			     obs_data_set_array(data, "cameras", cams);
			     obs_data_array_release(cams);
			     jsonResponse(res, obs_data_get_json(data));
			     obs_data_release(data);
		     });

	server_->Post("/api/config", [&core](const httplib::Request &req,
					     httplib::Response &res) {
		obs_data_t *data =
			obs_data_create_from_json(req.body.c_str());
		if (!data) {
			errorResponse(res, "invalid JSON");
			return;
		}
		Config cfg = core.getConfig();
		if (obs_data_has_user_value(data, "sessionFolder"))
			cfg.sessionFolder =
				obs_data_get_string(data, "sessionFolder");
		if (obs_data_has_user_value(data, "splitMinutes"))
			cfg.splitMinutes =
				(int)obs_data_get_int(data, "splitMinutes");
		if (obs_data_has_user_value(data, "videoBitrateKbps"))
			cfg.videoBitrateKbps = (int)obs_data_get_int(
				data, "videoBitrateKbps");
		if (obs_data_has_user_value(data, "audioBitrateKbps"))
			cfg.audioBitrateKbps = (int)obs_data_get_int(
				data, "audioBitrateKbps");
		if (obs_data_has_user_value(data, "videoEncoderId"))
			cfg.videoEncoderId =
				obs_data_get_string(data, "videoEncoderId");
		if (obs_data_has_user_value(data, "recFormat"))
			cfg.recFormat = obs_data_get_string(data, "recFormat");
		obs_data_array_t *cams = obs_data_get_array(data, "cameras");
		if (cams) {
			size_t count = obs_data_array_count(cams);
			for (size_t i = 0; i < (size_t)kMaxCameras; i++) {
				if (i < count) {
					obs_data_t *item =
						obs_data_array_item(cams, i);
					cfg.cameras[i].sourceName =
						obs_data_get_string(
							item, "sourceName");
					obs_data_release(item);
				} else {
					cfg.cameras[i].sourceName.clear();
				}
			}
			obs_data_array_release(cams);
		}
		obs_data_release(data);
		core.setConfig(cfg);
		okResponse(res);
	});

	// --- M2: session loading for playback ---
	auto &engine = ReplayEngine::instance();

	auto ensureSession = [&core, &engine](std::string &err) -> bool {
		if (engine.sessionLoaded())
			return true;
		return engine.loadSession(core.getConfig().sessionFolder, err);
	};

	server_->Post("/api/session/load", [&core, &engine](
						   const httplib::Request &,
						   httplib::Response &res) {
		std::string err;
		if (engine.loadSession(core.getConfig().sessionFolder, err))
			okResponse(res);
		else
			errorResponse(res, err);
	});
	server_->Post("/api/session/refresh",
		      [&engine](const httplib::Request &,
				httplib::Response &res) {
			      engine.refreshSession();
			      okResponse(res);
		      });

	// --- M2: transport (the reference controller: ReplayPlayPause / ChangeDirection /
	// ChangeSpeed / JumpFrames / JumpToNow / ACamera / BCamera) ---
	server_->Get("/api/transport",
		     [&engine](const httplib::Request &,
			       httplib::Response &res) {
			     jsonResponse(res, engine.transportJson());
		     });

	// Body: { "channel": "A"|"B" (default A; mirrored when linked),
	//         ...command-specific fields }
	auto transportCmd =
		[&engine, ensureSession](
			const httplib::Request &req, httplib::Response &res,
			void (*apply)(ReplayPlayer &, obs_data_t *)) {
			std::string err;
			if (!ensureSession(err)) {
				errorResponse(res, err);
				return;
			}
			obs_data_t *body = obs_data_create_from_json(
				req.body.empty() ? "{}" : req.body.c_str());
			if (!body) {
				errorResponse(res, "invalid JSON");
				return;
			}
			const char *chStr =
				obs_data_get_string(body, "channel");
			char ch = (chStr && (*chStr == 'B' || *chStr == 'b'))
					  ? 'B'
					  : 'A';
			engine.applyTransport(ch, [&](ReplayPlayer &p) {
				apply(p, body);
			});
			obs_data_release(body);
			okResponse(res);
		};

	server_->Post("/api/transport/playPause",
		      [transportCmd](const httplib::Request &req,
				     httplib::Response &res) {
			      transportCmd(req, res,
					   [](ReplayPlayer &p, obs_data_t *) {
						   p.setPlaying(!p.playing());
					   });
		      });
	server_->Post("/api/transport/play",
		      [transportCmd](const httplib::Request &req,
				     httplib::Response &res) {
			      transportCmd(req, res,
					   [](ReplayPlayer &p, obs_data_t *) {
						   p.setPlaying(true);
					   });
		      });
	server_->Post("/api/transport/pause",
		      [transportCmd](const httplib::Request &req,
				     httplib::Response &res) {
			      transportCmd(req, res,
					   [](ReplayPlayer &p, obs_data_t *) {
						   p.setPlaying(false);
					   });
		      });
	server_->Post("/api/transport/changeDirection",
		      [transportCmd](const httplib::Request &req,
				     httplib::Response &res) {
			      transportCmd(req, res,
					   [](ReplayPlayer &p, obs_data_t *) {
						   p.changeDirection();
					   });
		      });
	server_->Post(
		"/api/transport/speed",
		[transportCmd](const httplib::Request &req,
			       httplib::Response &res) {
			transportCmd(req, res,
				     [](ReplayPlayer &p, obs_data_t *body) {
					     p.setSpeed(obs_data_get_double(
						     body, "value"));
				     });
		});
	server_->Post(
		"/api/transport/jumpFrames",
		[transportCmd](const httplib::Request &req,
			       httplib::Response &res) {
			transportCmd(req, res,
				     [](ReplayPlayer &p, obs_data_t *body) {
					     p.stepFrames((int)obs_data_get_int(
						     body, "value"));
				     });
		});
	server_->Post(
		"/api/transport/seek",
		[transportCmd](const httplib::Request &req,
			       httplib::Response &res) {
			transportCmd(req, res,
				     [](ReplayPlayer &p, obs_data_t *body) {
					     p.seekMaster(obs_data_get_int(
						     body, "positionNs"));
				     });
		});
	server_->Post("/api/transport/jumpToNow",
		      [transportCmd](const httplib::Request &req,
				     httplib::Response &res) {
			      transportCmd(req, res,
					   [](ReplayPlayer &p, obs_data_t *) {
						   p.jumpToEnd();
					   });
		      });

	// --- M2: channel angle + linking (the reference controller A|B) ---
	server_->Post("/api/channel/angle", [&engine, ensureSession](
						    const httplib::Request &req,
						    httplib::Response &res) {
		std::string err;
		if (!ensureSession(err)) {
			errorResponse(res, err);
			return;
		}
		obs_data_t *body =
			obs_data_create_from_json(req.body.c_str());
		if (!body) {
			errorResponse(res, "invalid JSON");
			return;
		}
		const char *chStr = obs_data_get_string(body, "channel");
		int angle = (int)obs_data_get_int(body, "angle"); // 1-based
		ReplayPlayer *p = engine.channel(chStr ? *chStr : 'A');
		if (p && angle >= 1 && angle <= kIndexMaxCameras) {
			p->setAngle(angle - 1);
			okResponse(res);
		} else {
			errorResponse(res, "invalid channel or angle");
		}
		obs_data_release(body);
	});

	server_->Post("/api/channel/link", [&engine](
						   const httplib::Request &req,
						   httplib::Response &res) {
		obs_data_t *body =
			obs_data_create_from_json(req.body.c_str());
		if (!body) {
			errorResponse(res, "invalid JSON");
			return;
		}
		engine.setLinked(obs_data_get_bool(body, "linked"));
		obs_data_release(body);
		okResponse(res);
	});

	// --- Static UI ---
	char *uiPath = obs_module_file("ui");
	if (uiPath) {
		server_->set_mount_point("/", uiPath);
		bfree(uiPath);
	} else {
		obs_log(LOG_WARNING, "UI directory not found in module data");
	}
}

void WebServer::start(int port)
{
	if (running_)
		return;

	server_ = std::make_unique<httplib::Server>();
	setupRoutes();
	port_ = port;

	thread_ = std::thread([this, port]() {
		obs_log(LOG_INFO, "Web UI listening on http://0.0.0.0:%d", port);
		if (!server_->listen("0.0.0.0", port)) {
			obs_log(LOG_ERROR,
				"Web server failed to bind port %d "
				"(already in use?)",
				port);
		}
	});
	running_ = true;
}

void WebServer::stop()
{
	if (!running_)
		return;
	server_->stop();
	if (thread_.joinable())
		thread_.join();
	server_.reset();
	running_ = false;
}

} // namespace multireplay
