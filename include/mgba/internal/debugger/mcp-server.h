/* Copyright (c) 2026 mgba-mcp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/interface.h>
#include <mgba/core/mem-search.h>
#include <mgba/debugger/debugger.h>
#include <mgba-util/socket.h>

/*
 * A Model Context Protocol (MCP) server that exposes the emulator to AI
 * agents such as Claude Code over Streamable HTTP (POST /mcp).
 *
 * It is a regular debugger module: every request is handled on the core
 * thread, so tools can touch emulator state without extra locking. Requests
 * are picked up from three places:
 *  - the end of every emulated frame while the game runs,
 *  - the debugger's paused loop (after `pause`, a breakpoint or a watchpoint),
 *  - the debugger's update hook while the frontend has paused the thread.
 */

#define MCP_SERVER_DEFAULT_PORT 2347
#define MCP_SERVER_MAX_CONNECTIONS 8

struct MCPConnection {
	Socket socket;
	char* buffer;
	size_t size;
	size_t capacity;
};

struct MCPStopInfo {
	bool valid;
	enum mDebuggerEntryReason reason;
	uint32_t address;
	ssize_t pointId;
	int width;
	uint32_t oldValue;
	uint32_t newValue;
	enum mWatchpointType accessType;
	uint32_t frame;
};

#define MCP_SNAPSHOT_REGIONS 4
#define MCP_MAX_WATCHES 64
#define MCP_MAX_SEQUENCE 256

struct MCPMemoryWatch {
	uint32_t address;
	int width;
	uint32_t lastValue;
	char label[48];
};

struct MCPInputStep {
	uint32_t keys;
	int frames;
};

struct MCPServer {
	struct mDebuggerModule d;

	Socket listenSocket;
	struct MCPConnection connections[MCP_SERVER_MAX_CONNECTIONS];

	struct mCoreCallbacks callbacks;
	bool callbacksAttached;

	/* Re-entrancy guard: set while a tool is executing (e.g. while it runs frames). */
	bool busy;
	/* Set when a request arriving mid-frame needed the paused loop; resume after it. */
	bool resumeAfterDeferred;
	/* Set by `entered` while a tool is running frames, so the tool can stop early. */
	bool stopHit;

	struct MCPStopInfo lastStop;

	/* Buttons held by `press_buttons` while the game is running. */
	uint32_t timedKeys;
	int timedKeysFrames;

	/* Value search ("cheat search") state, kept between memory_search_* calls. */
	struct mCoreMemorySearchResults searchResults;
	bool searchActive;
	int searchWidth;
	bool searchRamOnly;
	unsigned searchSteps;

	/* State captured before the last load_state, for undo_load_state. */
	void* undoState;
	size_t undoStateSize;

	/* RAM copy taken by snapshot_memory, compared by diff_memory. */
	uint8_t* snapshot;
	uint32_t snapshotStart[MCP_SNAPSHOT_REGIONS];
	uint32_t snapshotSize[MCP_SNAPSHOT_REGIONS];
	unsigned snapshotRegions;
	uint32_t snapshotFrame;

	/* Addresses tracked by memory_watch. */
	struct MCPMemoryWatch watches[MCP_MAX_WATCHES];
	unsigned nWatches;

	/* Timed button sequence played by input_sequence while the game runs. */
	struct MCPInputStep sequence[MCP_MAX_SEQUENCE];
	unsigned sequenceLength;
	unsigned sequenceIndex;
	int sequenceFramesLeft;
};

void MCPServerCreate(struct MCPServer*);
bool MCPServerListen(struct MCPServer*, int port, const struct Address* bindAddress);
void MCPServerShutdown(struct MCPServer*);
bool MCPServerIsListening(const struct MCPServer*);

CXX_GUARD_END

#endif
