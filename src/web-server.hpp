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

	std::unique_ptr<httplib::Server> server_;
	std::thread thread_;
	bool running_ = false;
	int port_ = 0;
};

} // namespace multireplay
