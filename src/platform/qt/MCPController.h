/* Copyright (c) 2026 mgba-mcp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "DebuggerController.h"

#ifdef ENABLE_MCP_SERVER

#include <mgba/internal/debugger/mcp-server.h>

namespace QGBA {

class CoreController;

/*
 * Runs the built-in MCP server for the game in a window. Unlike the GDB stub it
 * attaches without pausing, so the game keeps running and AI agents can pause it
 * through MCP when they need to. The debugger module is torn down whenever a game
 * stops, so Window calls start() again for every game.
 */
class MCPController : public DebuggerController {
Q_OBJECT

public:
	MCPController(QObject* parent = nullptr);
	~MCPController();

	ushort port() const { return m_port; }
	bool isListening() const;

public slots:
	/* Starts (or restarts, if the game or port changed) the server for `controller`. */
	bool start(std::shared_ptr<CoreController> controller, ushort port);
	void stop();

signals:
	void listening(ushort port);
	void listenFailed(ushort port);

private:
	virtual void shutdownInternal() override;

	MCPServer m_server;
	ushort m_port = MCP_SERVER_DEFAULT_PORT;
};

}

#endif
