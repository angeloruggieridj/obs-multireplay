/*
obs-multireplay — broadcast-style instant replay for OBS Studio
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <memory>
#include <thread>

namespace httplib {
class Server;
}

namespace multireplay {

class WebServer {
public:
	static WebServer &instance();

	void start(int port);
	void stop();
	bool running() const { return running_; }
	int port() const { return port_; }

private:
	WebServer() = default;
	void setupRoutes();
	// Preview endpoints (.jpg snapshot + MJPEG push stream) registered on
	// BOTH servers. The dedicated stream port (port+1) exists because
	// browsers cap connections at 6 per host:port — persistent MJPEG
	// streams on the main port would starve the REST API.
	void registerPreviewRoutes(httplib::Server &srv);

	std::unique_ptr<httplib::Server> server_;
	std::unique_ptr<httplib::Server> streamServer_;
	std::thread thread_;
	std::thread streamThread_;
	bool running_ = false;
	int port_ = 0;
};

} // namespace multireplay
