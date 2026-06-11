/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "web-server.hpp"
#include "event-store.hpp"
#include "export.hpp"
#include "playback-coordinator.hpp"
#include "preview.hpp"
#include "replay-core.hpp"
#include "replay-player.hpp"
#include "plugin-support.h"

#include <obs-module.h>

// cpp-httplib (MIT) — HTTP only, no TLS needed on the local network for M1.
#define CPPHTTPLIB_NO_EXCEPTIONS
// Headroom for the snapshot pollers + REST polling of multiple browsers
// (long-lived MJPEG streams would otherwise starve the default pool).
#define CPPHTTPLIB_THREAD_POOL_COUNT 16
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
			     obs_data_set_string(data, "outputSceneName",
						 cfg.outputSceneName.c_str());
			     obs_data_set_string(data, "musicSourceName",
						 cfg.musicSourceName.c_str());
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
		if (obs_data_has_user_value(data, "outputSceneName"))
			cfg.outputSceneName =
				obs_data_get_string(data, "outputSceneName");
		if (obs_data_has_user_value(data, "musicSourceName"))
			cfg.musicSourceName =
				obs_data_get_string(data, "musicSourceName");
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

	// =====================================================================
	// M3: events, marking, event playback, multiview previews
	// =====================================================================
	auto &store = EventStore::instance();
	auto &coordinator = PlaybackCoordinator::instance();

	// Current mark time: Live mode = recording "now"; Recorded mode =
	// channel A playhead (the reference controller Live/Recorded semantics).
	auto markTimeNs = [&core, &engine, &store]() -> int64_t {
		if (store.liveMode()) {
			int64_t now = core.masterNowNs();
			if (now >= 0)
				return now;
		}
		return engine.channelA().position();
	};

	server_->Get("/api/events", [&store](const httplib::Request &req,
					     httplib::Response &res) {
		int list = store.selectedList();
		if (req.has_param("list"))
			list = std::stoi(req.get_param_value("list"));
		jsonResponse(res, store.listJson(list));
	});

	server_->Post("/api/events/list", [&store](const httplib::Request &req,
						   httplib::Response &res) {
		obs_data_t *body =
			obs_data_create_from_json(req.body.c_str());
		if (!body) {
			errorResponse(res, "invalid JSON");
			return;
		}
		store.selectList((int)obs_data_get_int(body, "list"));
		obs_data_release(body);
		okResponse(res);
	});

	server_->Post("/api/events/mode", [&store](const httplib::Request &req,
						   httplib::Response &res) {
		obs_data_t *body =
			obs_data_create_from_json(req.body.c_str());
		if (!body) {
			errorResponse(res, "invalid JSON");
			return;
		}
		store.setLiveMode(obs_data_get_bool(body, "live"));
		obs_data_release(body);
		okResponse(res);
	});

	server_->Post("/api/events/markIn",
		      [&store, markTimeNs](const httplib::Request &,
					   httplib::Response &res) {
			      store.markIn(markTimeNs());
			      okResponse(res);
		      });
	server_->Post("/api/events/markOut",
		      [&store, markTimeNs](const httplib::Request &,
					   httplib::Response &res) {
			      if (store.markOut(markTimeNs()))
				      okResponse(res);
			      else
				      errorResponse(res, "no open event");
		      });
	server_->Post("/api/events/markInOut",
		      [&store, markTimeNs](const httplib::Request &req,
					   httplib::Response &res) {
			      obs_data_t *body = obs_data_create_from_json(
				      req.body.c_str());
			      int seconds =
				      body ? (int)obs_data_get_int(body,
								   "seconds")
					   : 5;
			      if (body)
				      obs_data_release(body);
			      store.markInOut(markTimeNs(),
					      seconds > 0 ? seconds : 5);
			      okResponse(res);
		      });
	server_->Post("/api/events/markCancel",
		      [&store](const httplib::Request &,
			       httplib::Response &res) {
			      store.markCancel();
			      okResponse(res);
		      });

	// Generic event edit. Body: {id, op, ...}
	// ops: toggleAngle{angle} | note{angle,text} | speed{value} |
	//      movePoint{in,deltaNs} | moveToList{list} | remove | duplicate
	server_->Post("/api/events/edit", [&store](const httplib::Request &req,
						   httplib::Response &res) {
		obs_data_t *body =
			obs_data_create_from_json(req.body.c_str());
		if (!body) {
			errorResponse(res, "invalid JSON");
			return;
		}
		int id = (int)obs_data_get_int(body, "id");
		std::string op = obs_data_get_string(body, "op");
		bool ok = false;
		if (op == "toggleAngle")
			ok = store.toggleAngle(
				id, (int)obs_data_get_int(body, "angle"));
		else if (op == "note")
			ok = store.setAngleNote(
				id, (int)obs_data_get_int(body, "angle"),
				obs_data_get_string(body, "text"));
		else if (op == "speed")
			ok = store.setSpeed(
				id, obs_data_get_double(body, "value"));
		else if (op == "movePoint")
			ok = store.movePoint(
				id, obs_data_get_bool(body, "in"),
				obs_data_get_int(body, "deltaNs"));
		else if (op == "moveToList")
			ok = store.moveToList(
				id, (int)obs_data_get_int(body, "list"));
		else if (op == "remove")
			ok = store.remove(id);
		else if (op == "duplicate")
			ok = store.duplicate(id) != 0;
		obs_data_release(body);
		if (ok)
			okResponse(res);
		else
			errorResponse(res, "edit failed (unknown id or op)");
	});

	// --- Event playback (the reference controller PlayLastEvent / PlaySelectedEvent /
	//     ...ToOutput / StopEvents) ---
	auto parsePlayBody = [](const std::string &body,
				std::vector<int> &idsOut, bool &toOutputOut) {
		obs_data_t *data = obs_data_create_from_json(
			body.empty() ? "{}" : body.c_str());
		if (!data)
			return;
		toOutputOut = obs_data_get_bool(data, "toOutput");
		obs_data_array_t *ids = obs_data_get_array(data, "ids");
		if (ids) {
			size_t n = obs_data_array_count(ids);
			for (size_t i = 0; i < n; i++) {
				obs_data_t *item =
					obs_data_array_item(ids, i);
				idsOut.push_back((int)obs_data_get_int(
					item, "id"));
				obs_data_release(item);
			}
			obs_data_array_release(ids);
		}
		obs_data_release(data);
	};

	server_->Post("/api/play/selected",
		      [&coordinator, ensureSession, parsePlayBody](
			      const httplib::Request &req,
			      httplib::Response &res) {
			      std::string err;
			      if (!ensureSession(err)) {
				      errorResponse(res, err);
				      return;
			      }
			      std::vector<int> ids;
			      bool toOutput = false;
			      parsePlayBody(req.body, ids, toOutput);
			      if (coordinator.playEvents(ids, toOutput, err))
				      okResponse(res);
			      else
				      errorResponse(res, err);
		      });
	server_->Post("/api/play/last",
		      [&coordinator, ensureSession, parsePlayBody](
			      const httplib::Request &req,
			      httplib::Response &res) {
			      std::string err;
			      if (!ensureSession(err)) {
				      errorResponse(res, err);
				      return;
			      }
			      std::vector<int> ids;
			      bool toOutput = false;
			      parsePlayBody(req.body, ids, toOutput);
			      if (coordinator.playLastEvent(toOutput, err))
				      okResponse(res);
			      else
				      errorResponse(res, err);
		      });
	server_->Post("/api/play/stop",
		      [&coordinator](const httplib::Request &,
				     httplib::Response &res) {
			      coordinator.stopEvents();
			      okResponse(res);
		      });

	// --- M4: loop / music / Delete All / export clips ---
	server_->Post("/api/play/loop", [&coordinator](
						const httplib::Request &req,
						httplib::Response &res) {
		obs_data_t *body =
			obs_data_create_from_json(req.body.c_str());
		if (!body) {
			errorResponse(res, "invalid JSON");
			return;
		}
		coordinator.setLoop(obs_data_get_bool(body, "enabled"));
		obs_data_release(body);
		okResponse(res);
	});
	server_->Post("/api/play/music", [&coordinator](
						 const httplib::Request &req,
						 httplib::Response &res) {
		obs_data_t *body =
			obs_data_create_from_json(req.body.c_str());
		if (!body) {
			errorResponse(res, "invalid JSON");
			return;
		}
		coordinator.setMusicEnabled(
			obs_data_get_bool(body, "enabled"));
		obs_data_release(body);
		okResponse(res);
	});
	server_->Get("/api/play/flags",
		     [&coordinator](const httplib::Request &,
				    httplib::Response &res) {
			     obs_data_t *d = obs_data_create();
			     obs_data_set_bool(d, "loop",
					       coordinator.loop());
			     obs_data_set_bool(d, "music",
					       coordinator.musicEnabled());
			     jsonResponse(res, obs_data_get_json(d));
			     obs_data_release(d);
		     });

	server_->Post("/api/session/deleteAll",
		      [&core, &engine](const httplib::Request &,
				       httplib::Response &res) {
			      std::string err;
			      if (!core.deleteAllSession(err)) {
				      errorResponse(res, err);
				      return;
			      }
			      EventStore::instance().clearAll();
			      engine.clearSession();
			      okResponse(res);
		      });

	server_->Post("/api/export/event", [](const httplib::Request &req,
					      httplib::Response &res) {
		obs_data_t *body =
			obs_data_create_from_json(req.body.c_str());
		if (!body) {
			errorResponse(res, "invalid JSON");
			return;
		}
		int id = (int)obs_data_get_int(body, "id");
		int angle = (int)obs_data_get_int(body, "angle"); // 0 = auto
		std::string folder = obs_data_get_string(body, "folder");
		obs_data_release(body);
		std::string err;
		if (ExportManager::instance().exportEvent(id, angle, folder,
							  err))
			okResponse(res);
		else
			errorResponse(res, err);
	});
	server_->Post("/api/export/last", [](const httplib::Request &req,
					     httplib::Response &res) {
		obs_data_t *body = obs_data_create_from_json(
			req.body.empty() ? "{}" : req.body.c_str());
		std::string folder =
			body ? obs_data_get_string(body, "folder") : "";
		if (body)
			obs_data_release(body);
		std::string err;
		if (ExportManager::instance().exportLastEvent(folder, err))
			okResponse(res);
		else
			errorResponse(res, err);
	});
	server_->Get("/api/export/status",
		     [](const httplib::Request &, httplib::Response &res) {
			     jsonResponse(res, ExportManager::instance()
						       .statusJson());
		     });

	// --- Multiview previews: MJPEG stream + JPEG snapshot ---
	auto slotFromName = [](const std::string &name) -> int {
		if (name == "a")
			return kPreviewSlotA;
		if (name == "b")
			return kPreviewSlotB;
		if (name.size() == 4 && name.rfind("cam", 0) == 0)
			return name[3] - '1'; // cam1..cam4 -> 0..3
		return -1;
	};

	server_->Get("/api/preview/debug",
		     [](const httplib::Request &, httplib::Response &res) {
			     jsonResponse(res, PreviewManager::instance()
						       .debugJson());
		     });

	server_->Get(R"(/preview/(cam[1-4]|a|b)\.jpg)",
		     [slotFromName](const httplib::Request &req,
				    httplib::Response &res) {
			     int slot = slotFromName(req.matches[1].str());
			     uint64_t seq = 0;
			     auto jpeg = PreviewManager::instance().latest(
				     slot, seq);
			     if (!jpeg || jpeg->empty()) {
				     res.status = 404;
				     return;
			     }
			     res.set_content((const char *)jpeg->data(),
					     jpeg->size(), "image/jpeg");
		     });

	server_->Get(
		R"(/preview/(cam[1-4]|a|b))",
		[slotFromName](const httplib::Request &req,
			       httplib::Response &res) {
			int slot = slotFromName(req.matches[1].str());
			if (slot < 0) {
				res.status = 404;
				return;
			}
			res.set_content_provider(
				"multipart/x-mixed-replace; boundary=mrframe",
				[slot, lastSeq = (uint64_t)0](
					size_t,
					httplib::DataSink &sink) mutable {
					uint64_t seq = 0;
					auto jpeg =
						PreviewManager::instance()
							.waitNext(slot,
								  lastSeq,
								  seq, 1000);
					if (!jpeg) {
						// End the stream on shutdown,
						// otherwise the open
						// connection blocks
						// WebServer::stop() and OBS
						// never exits.
						if (!PreviewManager::instance()
							     .running())
							return false;
						return sink.is_writable();
					}
					lastSeq = seq;
					char head[128];
					int n = snprintf(
						head, sizeof(head),
						"--mrframe\r\nContent-Type: "
						"image/jpeg\r\n"
						"Content-Length: %zu\r\n\r\n",
						jpeg->size());
					if (!sink.write(head, (size_t)n))
						return false;
					if (!sink.write(
						    (const char *)jpeg->data(),
						    jpeg->size()))
						return false;
					return sink.write("\r\n", 2);
				});
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
