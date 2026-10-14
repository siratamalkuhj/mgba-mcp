/* Copyright (c) 2026 mgba-mcp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "MCPController.h"
#include "moc_MCPController.cpp"

#include "CoreController.h"

#include <mgba/core/log.h>

using namespace QGBA;

MCPController::MCPController(QObject* parent)
	: DebuggerController(&m_server.d, parent)
{
	MCPServerCreate(&m_server);
}

MCPController::~MCPController() {
	stop();
}

bool MCPController::isListening() const {
	return MCPServerIsListening(&m_server);
}

bool MCPController::start(std::shared_ptr<CoreController> controller, ushort port) {
	if (!controller) {
		return false;
	}
	if (controller == m_gameController && port == m_port && isAttached() && isListening()) {
		return true;
	}
	stop();
	if (controller != m_gameController) {
		// Avoid stacking duplicate "stopping" connections for the same game.
		setController(controller);
	}
	m_port = port;

	if (!MCPServerListen(&m_server, m_port, nullptr)) {
		mLOG(STATUS, ERROR, "MCP server: could not listen on port %u (already in use?)", m_port);
		emit listenFailed(m_port);
		return false;
	}
	// Attach without interrupting: the game keeps running until an agent pauses it.
	m_gameController->attachDebuggerModule(m_debugger, false);
	mLOG(STATUS, INFO, "MCP server: http://127.0.0.1:%u/mcp", m_port);
	emit listening(m_port);
	return true;
}

void MCPController::stop() {
	if (isAttached()) {
		detach();
	} else {
		MCPServerShutdown(&m_server);
	}
}

void MCPController::shutdownInternal() {
	MCPServerShutdown(&m_server);
}
