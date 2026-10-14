/* Copyright (c) 2026 mgba-mcp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/debugger/mcp-server.h>

#include <mgba/core/cheats.h>
#include <mgba/core/core.h>
#include <mgba/core/mem-search.h>
#include <mgba/core/serialize.h>
#include <mgba/core/thread.h>
#include <mgba/core/version.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#ifdef M_CORE_GBA
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/decoder.h>
#include <mgba/internal/gba/cheats.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/cheats.h>
#endif

#include "third-party/cjson/cJSON.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>

mLOG_DECLARE_CATEGORY(MCP);
mLOG_DEFINE_CATEGORY(MCP, "MCP server", "core.debugger.mcp");

#define MCP_SERVER_NAME "mgba-mcp"
#define MCP_MAX_REQUEST (4 * 1024 * 1024)
#define MCP_MAX_READ 4096
#define MCP_MAX_STEPS 1000000
#define MCP_MAX_FRAMES 3600
#define MCP_MAX_DISASM 256
#define MCP_MAX_SEARCH_SHOW 200

#define MCP_META_PROTOCOL_VERSION "io.modelcontextprotocol/protocolVersion"
#define MCP_META_SERVER_INFO "io.modelcontextprotocol/serverInfo"

/* Protocol revisions, newest first. Modern revisions are stateless (no initialize). */
static const char* const _modernVersions[] = { "2026-07-28", NULL };
static const char* const _legacyVersions[] = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", NULL };

enum {
	JSONRPC_PARSE_ERROR = -32700,
	JSONRPC_INVALID_REQUEST = -32600,
	JSONRPC_METHOD_NOT_FOUND = -32601,
	JSONRPC_INVALID_PARAMS = -32602,
	JSONRPC_INTERNAL_ERROR = -32603,
};

/* Where a request is being handled from. */
enum MCPContext {
	/* End of an emulated frame, game running: must not run the CPU or swap state here. */
	MCP_CONTEXT_RUNNING,
	/* The debugger's paused loop: anything goes. */
	MCP_CONTEXT_PAUSED,
	/* The frontend paused the emulation thread: state is quiescent, but don't run the CPU. */
	MCP_CONTEXT_IDLE,
};

/* What a tool needs from the context it runs in. */
enum MCPRequirement {
	MCP_REQ_ANY,
	/* Needs a quiescent core (reset, loading state). Mid-frame, pause, run it, then resume. */
	MCP_REQ_QUIESCENT,
	/* Runs the CPU (step, run frames). Mid-frame, pause first and stay paused. */
	MCP_REQ_EXECUTE,
};

enum MCPProcessResult {
	MCP_NEED_MORE,
	MCP_PROCESSED,
	MCP_DEFERRED,
	MCP_CLOSED,
};

struct MCPToolResult {
	cJSON* content;
	bool isError;
};

struct MCPTool {
	const char* name;
	const char* description;
	const char* inputSchema;
	enum MCPRequirement requirement;
	void (*handler)(struct MCPServer*, const cJSON* args, enum MCPContext, struct MCPToolResult*);
};

struct MCPHttpRequest {
	char method[16];
	char path[256];
	size_t headerLength;
	size_t contentLength;
	char origin[256];
	char protocolVersion[32];
};

static const struct MCPTool _tools[];

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static bool _inList(const char* const* list, const char* value) {
	size_t i;
	for (i = 0; list[i]; ++i) {
		if (strcmp(list[i], value) == 0) {
			return true;
		}
	}
	return false;
}

static char* _base64Encode(const uint8_t* data, size_t length) {
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t outLength = 4 * ((length + 2) / 3);
	char* out = malloc(outLength + 1);
	if (!out) {
		return NULL;
	}
	size_t i, o = 0;
	for (i = 0; i + 2 < length; i += 3) {
		uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
		out[o++] = table[(v >> 18) & 0x3F];
		out[o++] = table[(v >> 12) & 0x3F];
		out[o++] = table[(v >> 6) & 0x3F];
		out[o++] = table[v & 0x3F];
	}
	if (i < length) {
		uint32_t v = data[i] << 16;
		if (i + 1 < length) {
			v |= data[i + 1] << 8;
		}
		out[o++] = table[(v >> 18) & 0x3F];
		out[o++] = table[(v >> 12) & 0x3F];
		out[o++] = i + 1 < length ? table[(v >> 6) & 0x3F] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
	return out;
}

static struct mCore* _core(struct MCPServer* server) {
	return server->d.p->core;
}

static bool _isGBA(struct mCore* core) {
#ifdef M_CORE_GBA
	return core->platform(core) == mPLATFORM_GBA;
#else
	UNUSED(core);
	return false;
#endif
}

/* ---------------------------------------------------------------------------
 * Tool results and arguments
 * ------------------------------------------------------------------------- */

static void _resultText(struct MCPToolResult* result, const char* text) {
	cJSON* item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "type", "text");
	cJSON_AddStringToObject(item, "text", text);
	cJSON_AddItemToArray(result->content, item);
}

static void _resultPrintf(struct MCPToolResult* result, const char* fmt, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	_resultText(result, buffer);
}

static void _resultError(struct MCPToolResult* result, const char* fmt, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	_resultText(result, buffer);
	result->isError = true;
}

/* Takes ownership of `json`. */
static void _resultJson(struct MCPToolResult* result, cJSON* json) {
	char* text = cJSON_Print(json);
	if (text) {
		_resultText(result, text);
		cJSON_free(text);
	}
	cJSON_Delete(json);
}

static bool _parseU32(const char* string, uint32_t* out) {
	while (isspace((unsigned char) *string)) {
		++string;
	}
	if (!*string) {
		return false;
	}
	char* end;
	unsigned long long value;
	if (string[0] == '$') {
		value = strtoull(&string[1], &end, 16);
	} else {
		value = strtoull(string, &end, 0);
	}
	while (isspace((unsigned char) *end)) {
		++end;
	}
	if (*end || value > 0xFFFFFFFFULL) {
		return false;
	}
	*out = (uint32_t) value;
	return true;
}

/* Accepts JSON numbers as well as strings such as "0x02000000", "$2000000" or "1234". */
static bool _argU32(const cJSON* args, const char* name, uint32_t* out, bool required, struct MCPToolResult* result) {
	const cJSON* item = cJSON_GetObjectItemCaseSensitive(args, name);
	if (!item || cJSON_IsNull(item)) {
		if (required) {
			_resultError(result, "Missing required argument '%s'", name);
		}
		return false;
	}
	if (cJSON_IsNumber(item)) {
		if (item->valuedouble < 0 || item->valuedouble > 4294967295.0) {
			_resultError(result, "Argument '%s' is out of range", name);
			return false;
		}
		*out = (uint32_t) item->valuedouble;
		return true;
	}
	if (cJSON_IsString(item) && _parseU32(item->valuestring, out)) {
		return true;
	}
	_resultError(result, "Argument '%s' must be a number or a numeric string such as \"0x02000000\"", name);
	return false;
}

static int _argInt(const cJSON* args, const char* name, int defaultValue) {
	const cJSON* item = cJSON_GetObjectItemCaseSensitive(args, name);
	if (cJSON_IsNumber(item)) {
		return item->valueint;
	}
	uint32_t value;
	if (cJSON_IsString(item) && _parseU32(item->valuestring, &value)) {
		return (int) value;
	}
	return defaultValue;
}

static bool _argBool(const cJSON* args, const char* name, bool defaultValue) {
	const cJSON* item = cJSON_GetObjectItemCaseSensitive(args, name);
	if (cJSON_IsBool(item)) {
		return cJSON_IsTrue(item);
	}
	return defaultValue;
}

static const char* _argString(const cJSON* args, const char* name) {
	const cJSON* item = cJSON_GetObjectItemCaseSensitive(args, name);
	if (cJSON_IsString(item)) {
		return item->valuestring;
	}
	return NULL;
}

/* ---------------------------------------------------------------------------
 * Emulation helpers (always called on the core thread)
 * ------------------------------------------------------------------------- */

static const char* const _keyNames[] = {
	"A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", NULL
};

static bool _parseButtons(const cJSON* args, const char* name, uint32_t* keys, struct MCPToolResult* result) {
	*keys = 0;
	const cJSON* list = cJSON_GetObjectItemCaseSensitive(args, name);
	if (!list || cJSON_IsNull(list)) {
		return true;
	}
	if (cJSON_IsString(list)) {
		/* Be lenient and accept a single button as a string. */
		const char* single = list->valuestring;
		size_t i;
		for (i = 0; _keyNames[i]; ++i) {
			if (strcasecmp(_keyNames[i], single) == 0) {
				*keys |= 1 << i;
				return true;
			}
		}
		_resultError(result, "Unknown button '%s'", single);
		return false;
	}
	if (!cJSON_IsArray(list)) {
		_resultError(result, "Argument '%s' must be an array of button names", name);
		return false;
	}
	const cJSON* item;
	cJSON_ArrayForEach(item, list) {
		if (!cJSON_IsString(item)) {
			_resultError(result, "Button names must be strings");
			return false;
		}
		size_t i;
		bool found = false;
		for (i = 0; _keyNames[i]; ++i) {
			if (strcasecmp(_keyNames[i], item->valuestring) == 0) {
				*keys |= 1 << i;
				found = true;
				break;
			}
		}
		if (!found) {
			_resultError(result, "Unknown button '%s' (valid: A, B, Select, Start, Right, Left, Up, Down, R, L)", item->valuestring);
			return false;
		}
	}
	return true;
}

static cJSON* _keysToJson(uint32_t keys) {
	cJSON* array = cJSON_CreateArray();
	size_t i;
	for (i = 0; _keyNames[i]; ++i) {
		if (keys & (1 << i)) {
			cJSON_AddItemToArray(array, cJSON_CreateString(_keyNames[i]));
		}
	}
	return array;
}

static void _formatHex(char* buffer, size_t size, uint32_t value) {
	snprintf(buffer, size, "0x%08X", value);
}

static void _addHex(cJSON* object, const char* name, uint32_t value) {
	char buffer[16];
	_formatHex(buffer, sizeof(buffer), value);
	cJSON_AddStringToObject(object, name, buffer);
}

/* Address of the instruction about to execute. */
static bool _currentPC(struct mCore* core, uint32_t* pc, bool* thumb) {
#ifdef M_CORE_GBA
	if (_isGBA(core)) {
		struct ARMCore* cpu = core->cpu;
		bool isThumb = cpu->executionMode == MODE_THUMB;
		*pc = cpu->gprs[ARM_PC] - (isThumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM);
		if (thumb) {
			*thumb = isThumb;
		}
		return true;
	}
#endif
	int32_t value;
	if (core->readRegister(core, "pc", &value)) {
		*pc = (uint32_t) value;
		if (thumb) {
			*thumb = false;
		}
		return true;
	}
	return false;
}

#ifdef M_CORE_GBA
/* Disassembles one instruction, returns its size. */
static uint32_t _disassembleOne(struct mCore* core, uint32_t address, bool thumb, char* out, size_t outSize) {
	char disassembly[96];
	struct ARMInstructionInfo info;
	if (!thumb) {
		address &= ~(WORD_SIZE_ARM - 1);
		uint32_t instruction = core->rawRead32(core, address, -1);
		ARMDecodeARM(instruction, &info);
		ARMDisassemble(&info, core->cpu, core->symbolTable, address + WORD_SIZE_ARM * 2, disassembly, sizeof(disassembly));
		snprintf(out, outSize, "%08X:  %08X   %s", address, instruction, disassembly);
		return WORD_SIZE_ARM;
	}
	address &= ~(WORD_SIZE_THUMB - 1);
	struct ARMInstructionInfo info2;
	struct ARMInstructionInfo combined;
	uint16_t instruction = core->rawRead16(core, address, -1);
	uint16_t instruction2 = core->rawRead16(core, address + WORD_SIZE_THUMB, -1);
	ARMDecodeThumb(instruction, &info);
	ARMDecodeThumb(instruction2, &info2);
	if (ARMDecodeThumbCombine(&info, &info2, &combined)) {
		ARMDisassemble(&combined, core->cpu, core->symbolTable, address + WORD_SIZE_THUMB * 2, disassembly, sizeof(disassembly));
		snprintf(out, outSize, "%08X:  %04X %04X  %s", address, instruction, instruction2, disassembly);
		return WORD_SIZE_THUMB * 2;
	}
	ARMDisassemble(&info, core->cpu, core->symbolTable, address + WORD_SIZE_THUMB * 2, disassembly, sizeof(disassembly));
	snprintf(out, outSize, "%08X:  %04X       %s", address, instruction, disassembly);
	return WORD_SIZE_THUMB;
}
#endif

static const char* _reasonName(enum mDebuggerEntryReason reason) {
	switch (reason) {
	case DEBUGGER_ENTER_MANUAL:
		return "manual";
	case DEBUGGER_ENTER_ATTACHED:
		return "attached";
	case DEBUGGER_ENTER_BREAKPOINT:
		return "breakpoint";
	case DEBUGGER_ENTER_WATCHPOINT:
		return "watchpoint";
	case DEBUGGER_ENTER_ILLEGAL_OP:
		return "illegal_opcode";
	case DEBUGGER_ENTER_STACK:
		return "stack_trace";
	}
	return "unknown";
}

static const char* _watchTypeName(enum mWatchpointType type) {
	switch (type) {
	case WATCHPOINT_WRITE:
		return "write";
	case WATCHPOINT_READ:
		return "read";
	case WATCHPOINT_RW:
		return "rw";
	case WATCHPOINT_CHANGE:
		return "change";
	case WATCHPOINT_WRITE_CHANGE:
		return "write_change";
	}
	return "unknown";
}

static cJSON* _stopToJson(const struct MCPStopInfo* stop) {
	if (!stop->valid) {
		return cJSON_CreateNull();
	}
	cJSON* json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "reason", _reasonName(stop->reason));
	cJSON_AddNumberToObject(json, "frame", stop->frame);
	if (stop->reason == DEBUGGER_ENTER_BREAKPOINT || stop->reason == DEBUGGER_ENTER_WATCHPOINT || stop->reason == DEBUGGER_ENTER_ILLEGAL_OP) {
		_addHex(json, "address", stop->address);
	}
	if (stop->pointId >= 0 && (stop->reason == DEBUGGER_ENTER_BREAKPOINT || stop->reason == DEBUGGER_ENTER_WATCHPOINT)) {
		cJSON_AddNumberToObject(json, "id", (double) stop->pointId);
	}
	if (stop->reason == DEBUGGER_ENTER_WATCHPOINT) {
		cJSON_AddStringToObject(json, "access", _watchTypeName(stop->accessType));
		cJSON_AddNumberToObject(json, "width", stop->width);
		_addHex(json, "oldValue", stop->oldValue);
		_addHex(json, "newValue", stop->newValue);
	}
	return json;
}

static void _pause(struct MCPServer* server) {
	struct mDebuggerEntryInfo info = {0};
	info.target = &server->d;
	info.pointId = -1;
	mDebuggerEnter(server->d.p, DEBUGGER_ENTER_MANUAL, &info);
}

static void _resume(struct MCPServer* server) {
	server->d.isPaused = false;
	mDebuggerUpdatePaused(server->d.p);
}

/* Runs `frames` frames, honoring breakpoints and watchpoints. Returns frames completed. */
static int _runFrames(struct MCPServer* server, int frames) {
	struct mCore* core = _core(server);
	struct mDebuggerPlatform* platform = server->d.p->platform;
	server->stopHit = false;
	int done;
	for (done = 0; done < frames && !server->stopHit; ++done) {
		uint32_t frame = core->frameCounter(core);
		if (platform->hasBreakpoints(platform)) {
			while (core->frameCounter(core) == frame && !server->stopHit) {
				core->step(core);
				platform->checkBreakpoints(platform);
			}
			if (server->stopHit && core->frameCounter(core) == frame) {
				break;
			}
		} else {
			core->runFrame(core);
		}
	}
	return done;
}

static bool _screenshot(struct MCPServer* server, struct MCPToolResult* result) {
	struct mCore* core = _core(server);
	struct VFile* vf = VFileMemChunk(NULL, 0);
	if (!vf) {
		return false;
	}
	bool ok = mCoreTakeScreenshotVF(core, vf);
	if (ok) {
		size_t size = vf->size(vf);
		void* data = vf->map(vf, size, MAP_READ);
		char* encoded = data ? _base64Encode(data, size) : NULL;
		if (data) {
			vf->unmap(vf, data, size);
		}
		if (encoded) {
			cJSON* item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "type", "image");
			cJSON_AddStringToObject(item, "mimeType", "image/png");
			cJSON_AddStringToObject(item, "data", encoded);
			cJSON_AddItemToArray(result->content, item);
			free(encoded);
		} else {
			ok = false;
		}
	}
	vf->close(vf);
	return ok;
}

/* ---------------------------------------------------------------------------
 * Tools
 * ------------------------------------------------------------------------- */

static void _toolGetStatus(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	struct mCore* core = _core(server);
	cJSON* json = cJSON_CreateObject();
	const char* state;
	if (context == MCP_CONTEXT_IDLE) {
		state = "paused_by_frontend";
	} else if (server->d.isPaused || server->d.p->state == DEBUGGER_PAUSED) {
		state = "paused";
	} else {
		state = "running";
	}
	cJSON_AddStringToObject(json, "state", state);
	cJSON_AddStringToObject(json, "platform", core->platform(core) == mPLATFORM_GB ? "GB" : "GBA");
	cJSON_AddNumberToObject(json, "frame", core->frameCounter(core));

	struct mGameInfo info = {0};
	core->getGameInfo(core, &info);
	cJSON* game = cJSON_AddObjectToObject(json, "game");
	cJSON_AddStringToObject(game, "title", info.title);
	cJSON_AddStringToObject(game, "code", info.code);
	cJSON_AddStringToObject(game, "maker", info.maker);
	cJSON_AddNumberToObject(game, "version", info.version);

	uint32_t pc;
	bool thumb;
	if (_currentPC(core, &pc, &thumb)) {
		_addHex(json, "pc", pc);
		if (_isGBA(core)) {
			cJSON_AddStringToObject(json, "cpuMode", thumb ? "thumb" : "arm");
		}
	}
	cJSON_AddItemToObject(json, "heldButtons", _keysToJson(core->getKeys(core)));
	cJSON_AddItemToObject(json, "lastStop", _stopToJson(&server->lastStop));
	_resultJson(result, json);
}

static void _toolPause(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	if (server->d.isPaused) {
		_resultText(result, "Already paused.");
		return;
	}
	_pause(server);
	if (context == MCP_CONTEXT_IDLE) {
		_resultText(result, "The frontend has the emulator paused; it will stay paused under MCP control when the frontend resumes.");
		return;
	}
	_resultPrintf(result, "Paused at frame %u.", _core(server)->frameCounter(_core(server)));
}

static void _toolResume(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	if (!server->d.isPaused) {
		if (context == MCP_CONTEXT_IDLE) {
			_resultText(result, "The emulator was paused from the frontend; resume it there.");
		} else {
			_resultText(result, "Already running.");
		}
		return;
	}
	_resume(server);
	if (server->d.p->state == DEBUGGER_PAUSED) {
		_resultText(result, "Released MCP's pause, but another debugger is still holding the emulator paused.");
		return;
	}
	_resultText(result, "Resumed.");
}

static void _toolStep(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	struct mDebuggerPlatform* platform = server->d.p->platform;
	int count = _argInt(args, "count", 1);
	if (count < 1 || count > MCP_MAX_STEPS) {
		_resultError(result, "'count' must be between 1 and %d", MCP_MAX_STEPS);
		return;
	}
	server->stopHit = false;
	int i;
	for (i = 0; i < count && !server->stopHit; ++i) {
		core->step(core);
		if (platform->hasBreakpoints(platform)) {
			platform->checkBreakpoints(platform);
		}
	}
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "executed", i);
	cJSON_AddNumberToObject(json, "frame", core->frameCounter(core));
	uint32_t pc;
	bool thumb;
	if (_currentPC(core, &pc, &thumb)) {
		_addHex(json, "pc", pc);
#ifdef M_CORE_GBA
		if (_isGBA(core)) {
			char line[160];
			_disassembleOne(core, pc, thumb, line, sizeof(line));
			cJSON_AddStringToObject(json, "next", line);
		}
#endif
	}
	if (server->stopHit) {
		cJSON_AddItemToObject(json, "stoppedBy", _stopToJson(&server->lastStop));
	}
	_resultJson(result, json);
}

static void _finishRun(struct MCPServer* server, int requested, int done, bool screenshot, struct MCPToolResult* result) {
	struct mCore* core = _core(server);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "framesRequested", requested);
	cJSON_AddNumberToObject(json, "framesRun", done);
	cJSON_AddNumberToObject(json, "frame", core->frameCounter(core));
	if (server->stopHit) {
		cJSON_AddItemToObject(json, "stoppedBy", _stopToJson(&server->lastStop));
		uint32_t pc;
		if (_currentPC(core, &pc, NULL)) {
			_addHex(json, "pc", pc);
		}
	}
	_resultJson(result, json);
	if (screenshot && !_screenshot(server, result)) {
		_resultText(result, "(Screenshot unavailable: this build has no PNG support or no video buffer.)");
	}
}

static void _toolRunFrames(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	int frames = _argInt(args, "frames", 1);
	if (frames < 1 || frames > MCP_MAX_FRAMES) {
		_resultError(result, "'frames' must be between 1 and %d", MCP_MAX_FRAMES);
		return;
	}
	uint32_t keys;
	if (!_parseButtons(args, "buttons", &keys, result)) {
		return;
	}
	core->addKeys(core, keys);
	int done = _runFrames(server, frames);
	core->clearKeys(core, keys);
	_finishRun(server, frames, done, _argBool(args, "screenshot", false), result);
}

static void _toolPressButtons(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	struct mCore* core = _core(server);
	uint32_t keys;
	if (!_parseButtons(args, "buttons", &keys, result)) {
		return;
	}
	if (!keys) {
		_resultError(result, "'buttons' must name at least one button");
		return;
	}
	int frames = _argInt(args, "frames", 6);
	if (frames < 1 || frames > MCP_MAX_FRAMES) {
		_resultError(result, "'frames' must be between 1 and %d", MCP_MAX_FRAMES);
		return;
	}
	if (context != MCP_CONTEXT_PAUSED) {
		/* Game is running (or the frontend paused it): hold the buttons and let the frame hook release them. */
		core->addKeys(core, keys);
		server->timedKeys |= keys;
		server->timedKeysFrames = frames;
		_resultPrintf(result, "Holding buttons for %d frames while the game runs.", frames);
		return;
	}
	core->addKeys(core, keys);
	int done = _runFrames(server, frames);
	core->clearKeys(core, keys);
	int settle = _argInt(args, "release_frames", 1);
	if (settle > 0 && !server->stopHit) {
		if (settle > MCP_MAX_FRAMES) {
			settle = MCP_MAX_FRAMES;
		}
		done += _runFrames(server, settle);
		frames += settle;
	}
	_finishRun(server, frames, done, _argBool(args, "screenshot", false), result);
}

static void _toolSetButtons(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	uint32_t keys;
	if (!_parseButtons(args, "buttons", &keys, result)) {
		return;
	}
	server->timedKeys = 0;
	server->timedKeysFrames = 0;
	core->setKeys(core, keys);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddItemToObject(json, "heldButtons", _keysToJson(keys));
	_resultJson(result, json);
}

static void _toolReset(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	_core(server)->reset(_core(server));
	_resultText(result, "Reset.");
}

static void _toolScreenshot(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	if (!_screenshot(server, result)) {
		_resultError(result, "Screenshot failed: this build has no PNG support or the frontend has no video buffer.");
	}
}

static void _toolReadMemory(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	uint32_t address;
	if (!_argU32(args, "address", &address, true, result)) {
		return;
	}
	int length = _argInt(args, "length", 16);
	if (length < 1 || length > MCP_MAX_READ) {
		_resultError(result, "'length' must be between 1 and %d", MCP_MAX_READ);
		return;
	}
	int segment = _argInt(args, "segment", -1);
	const char* format = _argString(args, "format");
	if (!format || strcmp(format, "dump") == 0) {
		size_t lines = (length + 15) / 16;
		size_t size = lines * 80 + 1;
		char* text = malloc(size);
		size_t pos = 0;
		int offset;
		for (offset = 0; offset < length; offset += 16) {
			char ascii[17];
			int i;
			pos += snprintf(&text[pos], size - pos, "%08X: ", address + offset);
			for (i = 0; i < 16; ++i) {
				if (offset + i < length) {
					uint8_t byte = core->rawRead8(core, address + offset + i, segment);
					pos += snprintf(&text[pos], size - pos, "%02X ", byte);
					ascii[i] = (byte >= 0x20 && byte < 0x7F) ? (char) byte : '.';
				} else {
					pos += snprintf(&text[pos], size - pos, "   ");
					ascii[i] = ' ';
				}
			}
			ascii[16] = '\0';
			pos += snprintf(&text[pos], size - pos, " %s\n", ascii);
		}
		_resultText(result, text);
		free(text);
		return;
	}
	int width;
	if (strcmp(format, "u8") == 0) {
		width = 1;
	} else if (strcmp(format, "u16") == 0) {
		width = 2;
	} else if (strcmp(format, "u32") == 0) {
		width = 4;
	} else {
		_resultError(result, "'format' must be one of dump, u8, u16, u32");
		return;
	}
	cJSON* json = cJSON_CreateObject();
	_addHex(json, "address", address);
	cJSON_AddStringToObject(json, "format", format);
	cJSON* values = cJSON_AddArrayToObject(json, "values");
	int offset;
	for (offset = 0; offset + width <= length; offset += width) {
		uint32_t value;
		switch (width) {
		case 1:
			value = core->rawRead8(core, address + offset, segment);
			break;
		case 2:
			value = core->rawRead16(core, address + offset, segment);
			break;
		default:
			value = core->rawRead32(core, address + offset, segment);
			break;
		}
		cJSON_AddItemToArray(values, cJSON_CreateNumber(value));
	}
	_resultJson(result, json);
}

static void _toolWriteMemory(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	uint32_t address;
	if (!_argU32(args, "address", &address, true, result)) {
		return;
	}
	int segment = _argInt(args, "segment", -1);
	const char* data = _argString(args, "data");
	if (data) {
		size_t written = 0;
		const char* p = data;
		while (*p) {
			while (*p && (isspace((unsigned char) *p) || *p == ',')) {
				++p;
			}
			if (!*p) {
				break;
			}
			if (!isxdigit((unsigned char) p[0]) || !isxdigit((unsigned char) p[1])) {
				_resultError(result, "'data' must be hex bytes such as \"DE AD BE EF\" (%zu bytes written before the error)", written);
				return;
			}
			char byte[3] = { p[0], p[1], '\0' };
			core->rawWrite8(core, address + written, segment, (uint8_t) strtoul(byte, NULL, 16));
			++written;
			p += 2;
		}
		_resultPrintf(result, "Wrote %zu bytes at 0x%08X.", written, address);
		return;
	}
	uint32_t value;
	if (!_argU32(args, "value", &value, false, result)) {
		if (!result->isError) {
			_resultError(result, "Pass either 'data' (hex bytes) or 'value' with 'width'");
		}
		return;
	}
	int width = _argInt(args, "width", 1);
	switch (width) {
	case 1:
		core->rawWrite8(core, address, segment, value);
		break;
	case 2:
		core->rawWrite16(core, address, segment, value);
		break;
	case 4:
		core->rawWrite32(core, address, segment, value);
		break;
	default:
		_resultError(result, "'width' must be 1, 2 or 4");
		return;
	}
	_resultPrintf(result, "Wrote 0x%X (%d bytes) at 0x%08X.", value, width, address);
}

static void _toolListMemoryBlocks(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mCore* core = _core(server);
	const struct mCoreMemoryBlock* blocks;
	size_t count = core->listMemoryBlocks(core, &blocks);
	cJSON* json = cJSON_CreateArray();
	size_t i;
	for (i = 0; i < count; ++i) {
		cJSON* block = cJSON_CreateObject();
		cJSON_AddStringToObject(block, "name", blocks[i].internalName);
		cJSON_AddStringToObject(block, "description", blocks[i].longName);
		_addHex(block, "start", blocks[i].start);
		_addHex(block, "end", blocks[i].end);
		cJSON_AddNumberToObject(block, "size", blocks[i].size);
		cJSON_AddBoolToObject(block, "writable", (blocks[i].flags & mCORE_MEMORY_WRITE) != 0);
		if (blocks[i].maxSegment) {
			cJSON_AddNumberToObject(block, "maxSegment", blocks[i].maxSegment);
		}
		cJSON_AddItemToArray(json, block);
	}
	_resultJson(result, json);
}

static void _toolGetRegisters(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mCore* core = _core(server);
	const struct mCoreRegisterInfo* registers;
	size_t count = core->listRegisters(core, &registers);
	cJSON* json = cJSON_CreateObject();
	size_t i;
	for (i = 0; i < count; ++i) {
		int32_t value;
		if (core->readRegister(core, registers[i].name, &value)) {
			_addHex(json, registers[i].name, (uint32_t) value);
		}
	}
#ifdef M_CORE_GBA
	if (_isGBA(core)) {
		struct ARMCore* cpu = core->cpu;
		uint32_t pc;
		bool thumb;
		_currentPC(core, &pc, &thumb);
		_addHex(json, "currentInstruction", pc);
		cJSON_AddStringToObject(json, "cpuMode", thumb ? "thumb" : "arm");
		char flags[5] = {
			cpu->cpsr.n ? 'N' : '-',
			cpu->cpsr.z ? 'Z' : '-',
			cpu->cpsr.c ? 'C' : '-',
			cpu->cpsr.v ? 'V' : '-',
			'\0'
		};
		cJSON_AddStringToObject(json, "flags", flags);
	}
#endif
	_resultJson(result, json);
}

static void _toolSetRegister(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	const char* name = _argString(args, "name");
	uint32_t value;
	if (!name) {
		_resultError(result, "Missing required argument 'name'");
		return;
	}
	if (!_argU32(args, "value", &value, true, result)) {
		return;
	}
	if (!core->writeRegister(core, name, (int32_t) value)) {
		_resultError(result, "Unknown or read-only register '%s'", name);
		return;
	}
	_resultPrintf(result, "%s = 0x%08X", name, value);
}

static void _toolDisassemble(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
#ifdef M_CORE_GBA
	if (_isGBA(core)) {
		uint32_t pc;
		bool thumb;
		_currentPC(core, &pc, &thumb);
		uint32_t address = pc;
		if (cJSON_GetObjectItemCaseSensitive(args, "address") && !_argU32(args, "address", &address, false, result)) {
			return;
		}
		const char* mode = _argString(args, "mode");
		if (mode && strcmp(mode, "arm") == 0) {
			thumb = false;
		} else if (mode && strcmp(mode, "thumb") == 0) {
			thumb = true;
		} else if (mode && strcmp(mode, "auto") != 0) {
			_resultError(result, "'mode' must be auto, arm or thumb");
			return;
		}
		int count = _argInt(args, "count", 10);
		if (count < 1 || count > MCP_MAX_DISASM) {
			_resultError(result, "'count' must be between 1 and %d", MCP_MAX_DISASM);
			return;
		}
		size_t size = count * 128 + 64;
		char* text = malloc(size);
		size_t pos = snprintf(text, size, "; %s mode\n", thumb ? "Thumb" : "ARM");
		int i;
		for (i = 0; i < count; ++i) {
			char line[160];
			uint32_t at = address;
			address += _disassembleOne(core, address, thumb, line, sizeof(line));
			pos += snprintf(&text[pos], size - pos, "%s %s\n", at == pc ? "=>" : "  ", line);
		}
		_resultText(result, text);
		free(text);
		return;
	}
#endif
	UNUSED(args);
	_resultError(result, "Disassembly is only implemented for GBA games so far.");
}

static void _toolAddBreakpoint(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	uint32_t address;
	if (!_argU32(args, "address", &address, true, result)) {
		return;
	}
	struct mBreakpoint breakpoint = {
		.address = address,
		.segment = _argInt(args, "segment", -1),
		.type = BREAKPOINT_HARDWARE,
	};
	struct mDebuggerPlatform* platform = server->d.p->platform;
	ssize_t id = platform->setBreakpoint(platform, &server->d, &breakpoint);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "id", (double) id);
	_addHex(json, "address", address & ~1);
	_resultJson(result, json);
}

static void _toolAddWatchpoint(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	uint32_t address;
	if (!_argU32(args, "address", &address, true, result)) {
		return;
	}
	int length = _argInt(args, "length", 1);
	if (length < 1 || length > 0x10000) {
		_resultError(result, "'length' must be between 1 and 65536");
		return;
	}
	const char* typeName = _argString(args, "type");
	enum mWatchpointType type = WATCHPOINT_WRITE;
	if (typeName) {
		if (strcmp(typeName, "write") == 0) {
			type = WATCHPOINT_WRITE;
		} else if (strcmp(typeName, "read") == 0) {
			type = WATCHPOINT_READ;
		} else if (strcmp(typeName, "rw") == 0) {
			type = WATCHPOINT_RW;
		} else if (strcmp(typeName, "change") == 0) {
			type = WATCHPOINT_WRITE_CHANGE;
		} else {
			_resultError(result, "'type' must be write, read, rw or change");
			return;
		}
	}
	struct mWatchpoint watchpoint = {
		.segment = _argInt(args, "segment", -1),
		.minAddress = address,
		.maxAddress = address + length,
		.type = type,
	};
	struct mDebuggerPlatform* platform = server->d.p->platform;
	ssize_t id = platform->setWatchpoint(platform, &server->d, &watchpoint);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "id", (double) id);
	_addHex(json, "address", address);
	cJSON_AddNumberToObject(json, "length", length);
	cJSON_AddStringToObject(json, "type", _watchTypeName(type));
	_resultJson(result, json);
}

static void _toolRemoveBreakpoint(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	uint32_t id;
	if (!_argU32(args, "id", &id, true, result)) {
		return;
	}
	struct mDebuggerPlatform* platform = server->d.p->platform;
	if (!platform->clearBreakpoint(platform, (ssize_t) id)) {
		_resultError(result, "No breakpoint or watchpoint with id %u", id);
		return;
	}
	_resultPrintf(result, "Removed %u.", id);
}

static void _toolListBreakpoints(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mDebuggerPlatform* platform = server->d.p->platform;
	cJSON* json = cJSON_CreateObject();
	cJSON* bps = cJSON_AddArrayToObject(json, "breakpoints");
	cJSON* wps = cJSON_AddArrayToObject(json, "watchpoints");

	struct mBreakpointList breakpoints;
	mBreakpointListInit(&breakpoints, 0);
	platform->listBreakpoints(platform, &server->d, &breakpoints);
	size_t i;
	for (i = 0; i < mBreakpointListSize(&breakpoints); ++i) {
		const struct mBreakpoint* bp = mBreakpointListGetConstPointer(&breakpoints, i);
		if (bp->isTemporary) {
			continue;
		}
		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "id", (double) bp->id);
		_addHex(item, "address", bp->address);
		cJSON_AddBoolToObject(item, "enabled", !bp->disabled);
		cJSON_AddItemToArray(bps, item);
	}
	mBreakpointListDeinit(&breakpoints);

	struct mWatchpointList watchpoints;
	mWatchpointListInit(&watchpoints, 0);
	platform->listWatchpoints(platform, &server->d, &watchpoints);
	for (i = 0; i < mWatchpointListSize(&watchpoints); ++i) {
		const struct mWatchpoint* wp = mWatchpointListGetConstPointer(&watchpoints, i);
		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "id", (double) wp->id);
		_addHex(item, "address", wp->minAddress);
		cJSON_AddNumberToObject(item, "length", wp->maxAddress - wp->minAddress);
		cJSON_AddStringToObject(item, "type", _watchTypeName(wp->type));
		cJSON_AddBoolToObject(item, "enabled", !wp->disabled);
		cJSON_AddItemToArray(wps, item);
	}
	mWatchpointListDeinit(&watchpoints);
	_resultJson(result, json);
}

static bool _stateArgs(const cJSON* args, int* slot, const char** path, struct MCPToolResult* result) {
	*path = _argString(args, "path");
	*slot = _argInt(args, "slot", 0);
	if (*path && *slot) {
		_resultError(result, "Pass either 'slot' or 'path', not both");
		return false;
	}
	if (!*path && (*slot < 1 || *slot > 9)) {
		_resultError(result, "Pass 'slot' (1-9) or 'path'");
		return false;
	}
	return true;
}

static void _toolSaveState(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	int slot;
	const char* path;
	if (!_stateArgs(args, &slot, &path, result)) {
		return;
	}
	const int flags = SAVESTATE_SCREENSHOT | SAVESTATE_RTC | SAVESTATE_METADATA;
	bool ok;
	if (path) {
		struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
		ok = vf && mCoreSaveStateNamed(core, vf, flags);
		if (vf) {
			vf->close(vf);
		}
	} else {
		ok = mCoreSaveState(core, slot, flags);
	}
	if (!ok) {
		_resultError(result, "Failed to save state");
		return;
	}
	if (path) {
		_resultPrintf(result, "Saved state to %s at frame %u.", path, core->frameCounter(core));
	} else {
		_resultPrintf(result, "Saved state to slot %d at frame %u.", slot, core->frameCounter(core));
	}
}

static void _toolLoadState(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	int slot;
	const char* path;
	if (!_stateArgs(args, &slot, &path, result)) {
		return;
	}
	const int flags = SAVESTATE_RTC;
	bool ok;
	if (path) {
		struct VFile* vf = VFileOpen(path, O_RDONLY);
		ok = vf && mCoreLoadStateNamed(core, vf, flags);
		if (vf) {
			vf->close(vf);
		}
	} else {
		ok = mCoreLoadState(core, slot, flags);
	}
	if (!ok) {
		_resultError(result, "Failed to load state");
		return;
	}
	_resultPrintf(result, "Loaded state; now at frame %u.", core->frameCounter(core));
}


/* ---------------------------------------------------------------------------
 * Cheats
 * ------------------------------------------------------------------------- */

static struct mCheatDevice* _cheatDevice(struct mCore* core, struct MCPToolResult* result) {
	struct mCheatDevice* device = core->cheatDevice ? core->cheatDevice(core) : NULL;
	if (!device) {
		_resultError(result, "This core has no cheat support.");
	}
	return device;
}

static void _cheatAutosave(struct mCheatDevice* device) {
#if defined(ENABLE_VFS) && defined(ENABLE_DIRECTORIES)
	/* Persists cheats next to the save file when the frontend enabled it (as the GUI does). */
	mCheatAutosave(device);
#else
	UNUSED(device);
#endif
}

static bool _cheatType(struct mCore* core, const char* name, int* type, struct MCPToolResult* result) {
	*type = 0; /* autodetect, on every platform */
	if (!name || strcmp(name, "auto") == 0) {
		return true;
	}
#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		if (strcmp(name, "gameshark") == 0) {
			*type = GBA_CHEAT_GAMESHARK;
		} else if (strcmp(name, "action_replay") == 0) {
			*type = GBA_CHEAT_PRO_ACTION_REPLAY;
		} else if (strcmp(name, "codebreaker") == 0) {
			*type = GBA_CHEAT_CODEBREAKER;
		} else if (strcmp(name, "vba") == 0) {
			*type = GBA_CHEAT_VBA;
		} else {
			_resultError(result, "'type' for GBA must be auto, gameshark, action_replay, codebreaker or vba");
			return false;
		}
		return true;
	}
#endif
#ifdef M_CORE_GB
	if (core->platform(core) == mPLATFORM_GB) {
		if (strcmp(name, "gameshark") == 0) {
			*type = GB_CHEAT_GAMESHARK;
		} else if (strcmp(name, "game_genie") == 0) {
			*type = GB_CHEAT_GAME_GENIE;
		} else if (strcmp(name, "vba") == 0) {
			*type = GB_CHEAT_VBA;
		} else {
			_resultError(result, "'type' for Game Boy must be auto, gameshark, game_genie or vba");
			return false;
		}
		return true;
	}
#endif
	_resultError(result, "Unknown cheat type '%s'", name);
	return false;
}

/* Adds whitespace-separated code words to `set` the way the GUI does: words are joined
 * until they form a valid line, so "XXXXXXXX YYYYYYYY" and one code per line both work. */
static size_t _addCheatCode(struct mCheatSet* set, const char* code, int type, int* failures) {
	size_t added = 0;
	char buffer[256] = "";
	char word[128];
	const char* p = code;
	*failures = 0;
	while (*p) {
		while (*p && isspace((unsigned char) *p)) {
			++p;
		}
		if (!*p) {
			break;
		}
		size_t length = 0;
		while (p[length] && !isspace((unsigned char) p[length])) {
			++length;
		}
		if (length >= sizeof(word)) {
			length = sizeof(word) - 1;
		}
		memcpy(word, p, length);
		word[length] = '\0';
		p += length;
		while (*p && !isspace((unsigned char) *p)) {
			++p;
		}

		if (buffer[0]) {
			char joined[sizeof(buffer) + sizeof(word) + 1];
			snprintf(joined, sizeof(joined), "%s %s", buffer, word);
			if (mCheatAddLine(set, joined, type)) {
				buffer[0] = '\0';
				++added;
			} else if (mCheatAddLine(set, word, type)) {
				buffer[0] = '\0';
				++added;
				++*failures;
			} else {
				strncpy(buffer, word, sizeof(buffer) - 1);
				buffer[sizeof(buffer) - 1] = '\0';
				++*failures;
			}
		} else if (mCheatAddLine(set, word, type)) {
			++added;
		} else {
			strncpy(buffer, word, sizeof(buffer) - 1);
			buffer[sizeof(buffer) - 1] = '\0';
		}
	}
	if (buffer[0]) {
		++*failures;
	}
	return added;
}

/* Builds code lines that keep `value` written at `address` every frame. */
static bool _freezeCode(struct mCore* core, uint32_t address, uint32_t value, int width, char* out, size_t outSize, int* type, struct MCPToolResult* result) {
#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		*type = GBA_CHEAT_VBA;
		switch (width) {
		case 1:
			snprintf(out, outSize, "%08X:%02X", address, value & 0xFF);
			return true;
		case 2:
			snprintf(out, outSize, "%08X:%04X", address, value & 0xFFFF);
			return true;
		case 4:
			snprintf(out, outSize, "%08X:%08X", address, value);
			return true;
		}
		_resultError(result, "'width' must be 1, 2 or 4");
		return false;
	}
#endif
#ifdef M_CORE_GB
	if (core->platform(core) == mPLATFORM_GB) {
		/* GameShark "01VVLLHH": write byte VV to address HHLL. One code per byte, little-endian. */
		if (address > 0xFFFF || (width != 1 && width != 2 && width != 4)) {
			_resultError(result, "Game Boy addresses are 16-bit and 'width' must be 1, 2 or 4");
			return false;
		}
		*type = GB_CHEAT_GAMESHARK;
		size_t pos = 0;
		int i;
		for (i = 0; i < width; ++i) {
			uint32_t a = (address + i) & 0xFFFF;
			pos += snprintf(&out[pos], outSize - pos, "%s01%02X%02X%02X", i ? "\n" : "", (value >> (8 * i)) & 0xFF, a & 0xFF, a >> 8);
		}
		return true;
	}
#endif
	UNUSED(address);
	UNUSED(value);
	UNUSED(width);
	UNUSED(out);
	UNUSED(outSize);
	UNUSED(type);
	_resultError(result, "Freezing values is not supported on this platform");
	return false;
}

static cJSON* _cheatListJson(struct mCheatDevice* device) {
	cJSON* list = cJSON_CreateArray();
	size_t i;
	for (i = 0; i < mCheatSetsSize(&device->cheats); ++i) {
		struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, i);
		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "index", (double) i);
		cJSON_AddStringToObject(item, "name", set->name ? set->name : "");
		cJSON_AddBoolToObject(item, "enabled", set->enabled);
		cJSON* lines = cJSON_AddArrayToObject(item, "codes");
		size_t l;
		for (l = 0; l < StringListSize(&set->lines); ++l) {
			cJSON_AddItemToArray(lines, cJSON_CreateString(*StringListGetPointer(&set->lines, l)));
		}
		cJSON_AddItemToArray(list, item);
	}
	return list;
}

static struct mCheatSet* _cheatAt(struct mCheatDevice* device, const cJSON* args, struct MCPToolResult* result) {
	uint32_t index;
	if (!_argU32(args, "index", &index, true, result)) {
		return NULL;
	}
	if (index >= mCheatSetsSize(&device->cheats)) {
		_resultError(result, "No cheat at index %u (there are %zu; see list_cheats)", index, mCheatSetsSize(&device->cheats));
		return NULL;
	}
	return *mCheatSetsGetPointer(&device->cheats, index);
}

static void _toolAddCheat(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	struct mCheatDevice* device = _cheatDevice(core, result);
	if (!device) {
		return;
	}
	const char* code = _argString(args, "code");
	const char* name = _argString(args, "name");
	bool hasAddress = cJSON_GetObjectItemCaseSensitive(args, "address") != NULL;
	if ((code != NULL) == hasAddress) {
		_resultError(result, "Pass either 'code' (a cheat code) or 'address' + 'value' (to freeze a value), not both");
		return;
	}

	char generated[128];
	char defaultName[64];
	int type;
	if (code) {
		if (!_cheatType(core, _argString(args, "type"), &type, result)) {
			return;
		}
		snprintf(defaultName, sizeof(defaultName), "MCP cheat");
	} else {
		uint32_t address, value;
		if (!_argU32(args, "address", &address, true, result) || !_argU32(args, "value", &value, true, result)) {
			return;
		}
		int width = _argInt(args, "width", 1);
		if (!_freezeCode(core, address, value, width, generated, sizeof(generated), &type, result)) {
			return;
		}
		code = generated;
		snprintf(defaultName, sizeof(defaultName), "MCP: 0x%X = %u", address, value);
	}

	struct mCheatSet* set = device->createSet(device, name ? name : defaultName);
	int failures = 0;
	size_t added = _addCheatCode(set, code, type, &failures);
	if (!added) {
		mCheatSetDeinit(set);
		_resultError(result, "No valid cheat code found. Check the format, or pass 'type' (gameshark, action_replay, codebreaker, vba).");
		return;
	}
	set->enabled = _argBool(args, "enabled", true);
	mCheatAddSet(device, set);
	if (set->refresh) {
		set->refresh(set, device);
	}
	_cheatAutosave(device);

	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "index", (double) (mCheatSetsSize(&device->cheats) - 1));
	cJSON_AddStringToObject(json, "name", set->name ? set->name : "");
	cJSON_AddNumberToObject(json, "linesAdded", (double) added);
	if (failures) {
		cJSON_AddNumberToObject(json, "linesRejected", failures);
		cJSON_AddStringToObject(json, "warning", "Some words could not be parsed and were skipped; the cheat may not work as intended.");
	}
	cJSON_AddBoolToObject(json, "enabled", set->enabled);
	cJSON_AddStringToObject(json, "note", "Cheats apply every frame while enabled. Some codes need the game to reach a certain point, or a reset, to take effect.");
	_resultJson(result, json);
}

static void _toolListCheats(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mCheatDevice* device = _cheatDevice(_core(server), result);
	if (!device) {
		return;
	}
	cJSON* json = cJSON_CreateObject();
	cJSON_AddItemToObject(json, "cheats", _cheatListJson(device));
	_resultJson(result, json);
}

static void _toolSetCheatEnabled(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCheatDevice* device = _cheatDevice(_core(server), result);
	if (!device) {
		return;
	}
	struct mCheatSet* set = _cheatAt(device, args, result);
	if (!set) {
		return;
	}
	const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(args, "enabled");
	if (!cJSON_IsBool(enabled)) {
		_resultError(result, "Missing required boolean argument 'enabled'");
		return;
	}
	set->enabled = cJSON_IsTrue(enabled);
	/* Applies the change now (including undoing ROM patches when disabling). */
	mCheatRefresh(device, set);
	_cheatAutosave(device);
	_resultPrintf(result, "Cheat '%s' %s.", set->name ? set->name : "", set->enabled ? "enabled" : "disabled");
}

static void _toolRemoveCheat(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCheatDevice* device = _cheatDevice(_core(server), result);
	if (!device) {
		return;
	}
	struct mCheatSet* set = _cheatAt(device, args, result);
	if (!set) {
		return;
	}
	char name[128];
	snprintf(name, sizeof(name), "%s", set->name ? set->name : "");
	if (set->enabled) {
		/* Undo ROM patches before the set goes away. */
		set->enabled = false;
		mCheatRefresh(device, set);
	}
	mCheatRemoveSet(device, set);
	mCheatSetDeinit(set);
	_cheatAutosave(device);
	_resultPrintf(result, "Removed cheat '%s'. Remaining cheats were renumbered; see list_cheats.", name);
}

/* ---------------------------------------------------------------------------
 * Value search ("cheat search")
 * ------------------------------------------------------------------------- */

static bool _isRamBlock(const struct mCoreMemoryBlock* block) {
	return strcmp(block->internalName, "wram") == 0 || strcmp(block->internalName, "iwram") == 0 || strcmp(block->internalName, "hram") == 0;
}

static void _filterRam(struct mCore* core, struct mCoreMemorySearchResults* results) {
	const struct mCoreMemoryBlock* blocks;
	size_t nBlocks = core->listMemoryBlocks(core, &blocks);
	size_t in, out = 0;
	for (in = 0; in < mCoreMemorySearchResultsSize(results); ++in) {
		const struct mCoreMemorySearchResult* res = mCoreMemorySearchResultsGetConstPointer(results, in);
		size_t b;
		for (b = 0; b < nBlocks; ++b) {
			if (_isRamBlock(&blocks[b]) && res->address >= blocks[b].start && res->address < blocks[b].end) {
				*mCoreMemorySearchResultsGetPointer(results, out++) = *res;
				break;
			}
		}
	}
	mCoreMemorySearchResultsResize(results, (ssize_t) out - (ssize_t) in);
}

static int _compareResults(const void* a, const void* b) {
	const struct mCoreMemorySearchResult* ra = a;
	const struct mCoreMemorySearchResult* rb = b;
	return ra->address < rb->address ? -1 : ra->address > rb->address;
}

static uint32_t _readWidth(struct mCore* core, uint32_t address, int segment, int width) {
	switch (width) {
	case 1:
		return core->rawRead8(core, address, segment);
	case 2:
		return core->rawRead16(core, address, segment);
	default:
		return core->rawRead32(core, address, segment);
	}
}

static void _searchSummary(struct MCPServer* server, int maxShow, struct MCPToolResult* result) {
	struct mCore* core = _core(server);
	struct mCoreMemorySearchResults* results = &server->searchResults;
	size_t count = mCoreMemorySearchResultsSize(results);
	if (maxShow < 0) {
		maxShow = 0;
	} else if (maxShow > MCP_MAX_SEARCH_SHOW) {
		maxShow = MCP_MAX_SEARCH_SHOW;
	}
	if (count > 1) {
		qsort(mCoreMemorySearchResultsGetPointer(results, 0), count, sizeof(struct mCoreMemorySearchResult), _compareResults);
	}

	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "candidates", (double) count);
	cJSON_AddNumberToObject(json, "width", server->searchWidth);
	cJSON_AddNumberToObject(json, "step", server->searchSteps);
	cJSON* list = cJSON_AddArrayToObject(json, "results");
	size_t i;
	for (i = 0; i < count && (int) i < maxShow; ++i) {
		const struct mCoreMemorySearchResult* res = mCoreMemorySearchResultsGetConstPointer(results, i);
		cJSON* item = cJSON_CreateObject();
		_addHex(item, "address", res->address);
		cJSON_AddNumberToObject(item, "value", _readWidth(core, res->address, res->segment, server->searchWidth));
		cJSON_AddItemToArray(list, item);
	}
	if (count > (size_t) maxShow) {
		cJSON_AddNumberToObject(json, "notShown", (double) (count - maxShow));
	}
	if (count == 0) {
		cJSON_AddStringToObject(json, "hint", "No candidates left. Start over with memory_search_start (try another width, or 'unknown' if the shown number is not stored as-is).");
	} else if (count > 50) {
		cJSON_AddStringToObject(json, "hint", "Change the value in the game (or let time pass), then narrow down with memory_search_refine.");
	} else {
		cJSON_AddStringToObject(json, "hint", "Few candidates left: try write_memory on one to confirm, then add_cheat with address + value to freeze it.");
	}
	_resultJson(result, json);
}

static void _toolSearchStart(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	const char* condition = _argString(args, "condition");
	if (!condition) {
		condition = cJSON_GetObjectItemCaseSensitive(args, "value") ? "equal" : "unknown";
	}
	int width = _argInt(args, "width", 1);
	if (width != 1 && width != 2 && width != 4) {
		_resultError(result, "'width' must be 1, 2 or 4 (bytes)");
		return;
	}
	const char* region = _argString(args, "region");
	bool ramOnly = !region || strcmp(region, "ram") == 0;
	if (!ramOnly && strcmp(region, "all") != 0) {
		_resultError(result, "'region' must be ram or all");
		return;
	}

	struct mCoreMemorySearchParams params;
	memset(&params, 0, sizeof(params));
	params.memoryFlags = mCORE_MEMORY_WRITE;
	params.type = mCORE_MEMORY_SEARCH_INT;
	params.align = -1;
	params.width = width;
	if (strcmp(condition, "unknown") == 0) {
		params.op = mCORE_MEMORY_SEARCH_ANY;
	} else {
		uint32_t value;
		if (!_argU32(args, "value", &value, true, result)) {
			return;
		}
		if ((width == 1 && value > 0xFF) || (width == 2 && value > 0xFFFF)) {
			_resultError(result, "Value %u does not fit in %d byte(s); use a larger 'width'", value, width);
			return;
		}
		params.valueInt = (int32_t) value;
		if (strcmp(condition, "equal") == 0) {
			params.op = mCORE_MEMORY_SEARCH_EQUAL;
		} else if (strcmp(condition, "greater") == 0) {
			params.op = mCORE_MEMORY_SEARCH_GREATER;
		} else if (strcmp(condition, "less") == 0) {
			params.op = mCORE_MEMORY_SEARCH_LESS;
		} else {
			_resultError(result, "'condition' for a new search must be equal, greater, less or unknown");
			return;
		}
	}

	mCoreMemorySearchResultsClear(&server->searchResults);
	mCoreMemorySearch(core, &params, &server->searchResults, 0);
	if (ramOnly) {
		_filterRam(core, &server->searchResults);
	}
	server->searchActive = true;
	server->searchWidth = width;
	server->searchRamOnly = ramOnly;
	server->searchSteps = 1;
	_searchSummary(server, _argInt(args, "max_results", 20), result);
}

static void _toolSearchRefine(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	if (!server->searchActive) {
		_resultError(result, "No search in progress; call memory_search_start first.");
		return;
	}
	const char* condition = _argString(args, "condition");
	if (!condition) {
		_resultError(result, "Missing required argument 'condition'");
		return;
	}
	struct mCoreMemorySearchParams params;
	memset(&params, 0, sizeof(params));
	params.memoryFlags = mCORE_MEMORY_WRITE;
	params.type = mCORE_MEMORY_SEARCH_INT;
	params.align = -1;
	params.width = server->searchWidth;

	bool needsValue = strcmp(condition, "equal") == 0 || strcmp(condition, "greater") == 0 ||
	                  strcmp(condition, "less") == 0 || strcmp(condition, "changed_by") == 0;
	if (needsValue) {
		uint32_t value = 0;
		const cJSON* item = cJSON_GetObjectItemCaseSensitive(args, "value");
		if (cJSON_IsNumber(item) && item->valuedouble < 0) {
			/* changed_by accepts negative deltas. */
			params.valueInt = (int32_t) item->valuedouble;
		} else if (_argU32(args, "value", &value, true, result)) {
			params.valueInt = (int32_t) value;
		} else {
			return;
		}
	}
	if (strcmp(condition, "equal") == 0) {
		params.op = mCORE_MEMORY_SEARCH_EQUAL;
	} else if (strcmp(condition, "greater") == 0) {
		params.op = mCORE_MEMORY_SEARCH_GREATER;
	} else if (strcmp(condition, "less") == 0) {
		params.op = mCORE_MEMORY_SEARCH_LESS;
	} else if (strcmp(condition, "changed") == 0) {
		params.op = mCORE_MEMORY_SEARCH_DELTA_ANY;
	} else if (strcmp(condition, "unchanged") == 0) {
		params.op = mCORE_MEMORY_SEARCH_DELTA;
		params.valueInt = 0;
	} else if (strcmp(condition, "increased") == 0) {
		params.op = mCORE_MEMORY_SEARCH_DELTA_POSITIVE;
	} else if (strcmp(condition, "decreased") == 0) {
		params.op = mCORE_MEMORY_SEARCH_DELTA_NEGATIVE;
	} else if (strcmp(condition, "changed_by") == 0) {
		params.op = mCORE_MEMORY_SEARCH_DELTA;
	} else {
		_resultError(result, "'condition' must be equal, greater, less, changed, unchanged, increased, decreased or changed_by");
		return;
	}
	size_t before = mCoreMemorySearchResultsSize(&server->searchResults);
	mCoreMemorySearchRepeat(core, &params, &server->searchResults);
	++server->searchSteps;
	mLOG(MCP, DEBUG, "Search refined from %zu to %zu", before, mCoreMemorySearchResultsSize(&server->searchResults));
	_searchSummary(server, _argInt(args, "max_results", 20), result);
}

static void _toolSearchResults(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	if (!server->searchActive) {
		_resultError(result, "No search in progress; call memory_search_start first.");
		return;
	}
	int maxShow = _argInt(args, "max_results", 50);
	if (maxShow < 1 || maxShow > MCP_MAX_SEARCH_SHOW) {
		_resultError(result, "'max_results' must be between 1 and %d", MCP_MAX_SEARCH_SHOW);
		return;
	}
	_searchSummary(server, maxShow, result);
}

#define BUTTONS_SCHEMA "{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"A\",\"B\",\"Select\",\"Start\",\"Right\",\"Left\",\"Up\",\"Down\",\"R\",\"L\"]}}"
#define ADDRESS_SCHEMA(DESC) "{\"type\":[\"integer\",\"string\"],\"description\":\"" DESC " Number or string such as \\\"0x02000000\\\".\"}"
#define EMPTY_SCHEMA "{\"type\":\"object\",\"properties\":{}}"

static const struct MCPTool _tools[] = {
	{ "get_status",
	  "Get emulator status: running or paused, frame number, game title/code, current PC and CPU mode, held buttons, and why execution last stopped (breakpoint, watchpoint, manual pause).",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolGetStatus },
	{ "pause",
	  "Pause emulation (takes effect at the end of the current frame). While paused, use step / run_frames / press_buttons to advance deterministically.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolPause },
	{ "resume",
	  "Resume free-running emulation after a pause, breakpoint or watchpoint.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolResume },
	{ "step",
	  "Execute CPU instructions one at a time (pauses first if running). Returns the new PC and the next instruction. Stops early on a breakpoint or watchpoint.",
	  "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000000,\"default\":1}}}",
	  MCP_REQ_EXECUTE, _toolStep },
	{ "run_frames",
	  "Advance emulation by N frames (60 frames = 1 second), optionally holding buttons the whole time, then stay paused. Stops early on a breakpoint or watchpoint. Set screenshot=true to get the final frame.",
	  "{\"type\":\"object\",\"properties\":{\"frames\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3600,\"default\":1},\"buttons\":" BUTTONS_SCHEMA ",\"screenshot\":{\"type\":\"boolean\",\"default\":false}}}",
	  MCP_REQ_EXECUTE, _toolRunFrames },
	{ "press_buttons",
	  "Press buttons for N frames, then release them. When paused, emulation advances for those frames (plus release_frames) and stays paused; when running, the buttons are held for the next N frames.",
	  "{\"type\":\"object\",\"properties\":{\"buttons\":" BUTTONS_SCHEMA ",\"frames\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3600,\"default\":6},\"release_frames\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3600,\"default\":1,\"description\":\"Frames to run after releasing (paused mode only).\"},\"screenshot\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"buttons\"]}",
	  MCP_REQ_ANY, _toolPressButtons },
	{ "set_buttons",
	  "Set exactly which buttons are held (replaces the current set; pass an empty list to release everything). They stay held until changed.",
	  "{\"type\":\"object\",\"properties\":{\"buttons\":" BUTTONS_SCHEMA "},\"required\":[\"buttons\"]}",
	  MCP_REQ_ANY, _toolSetButtons },
	{ "screenshot",
	  "Capture the current screen as a PNG image.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolScreenshot },
	{ "reset",
	  "Reset the emulated console.",
	  EMPTY_SCHEMA, MCP_REQ_QUIESCENT, _toolReset },
	{ "read_memory",
	  "Read memory without side effects. format=dump returns a hex dump; u8/u16/u32 return little-endian values. GBA map: 0x02000000 EWRAM, 0x03000000 IWRAM, 0x04000000 I/O, 0x05000000 palette, 0x06000000 VRAM, 0x07000000 OAM, 0x08000000 ROM.",
	  "{\"type\":\"object\",\"properties\":{\"address\":" ADDRESS_SCHEMA("Start address.") ",\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":16,\"description\":\"Bytes to read.\"},\"format\":{\"type\":\"string\",\"enum\":[\"dump\",\"u8\",\"u16\",\"u32\"],\"default\":\"dump\"},\"segment\":{\"type\":\"integer\",\"default\":-1,\"description\":\"Bank for banked memory; -1 = current.\"}},\"required\":[\"address\"]}",
	  MCP_REQ_ANY, _toolReadMemory },
	{ "write_memory",
	  "Write memory. Pass 'data' as hex bytes (\"DE AD BE EF\"), or 'value' with 'width' (1, 2 or 4 bytes, little-endian).",
	  "{\"type\":\"object\",\"properties\":{\"address\":" ADDRESS_SCHEMA("Start address.") ",\"data\":{\"type\":\"string\"},\"value\":{\"type\":[\"integer\",\"string\"]},\"width\":{\"type\":\"integer\",\"enum\":[1,2,4],\"default\":1},\"segment\":{\"type\":\"integer\",\"default\":-1}},\"required\":[\"address\"]}",
	  MCP_REQ_ANY, _toolWriteMemory },
	{ "list_memory_blocks",
	  "List the console's memory regions with their address ranges.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolListMemoryBlocks },
	{ "get_registers",
	  "Read all CPU registers (and, on GBA, CPU mode and NZCV flags).",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolGetRegisters },
	{ "set_register",
	  "Write a CPU register by name (e.g. r0, sp, lr, pc, cpsr).",
	  "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"value\":{\"type\":[\"integer\",\"string\"]}},\"required\":[\"name\",\"value\"]}",
	  MCP_REQ_ANY, _toolSetRegister },
	{ "disassemble",
	  "Disassemble instructions (GBA). Defaults to the current PC in the current CPU mode; '=>' marks the next instruction to execute.",
	  "{\"type\":\"object\",\"properties\":{\"address\":" ADDRESS_SCHEMA("Start address (default: current PC).") ",\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256,\"default\":10},\"mode\":{\"type\":\"string\",\"enum\":[\"auto\",\"arm\",\"thumb\"],\"default\":\"auto\"}}}",
	  MCP_REQ_ANY, _toolDisassemble },
	{ "add_breakpoint",
	  "Pause execution when the CPU is about to execute the instruction at an address. Returns the breakpoint id.",
	  "{\"type\":\"object\",\"properties\":{\"address\":" ADDRESS_SCHEMA("Instruction address.") ",\"segment\":{\"type\":\"integer\",\"default\":-1}},\"required\":[\"address\"]}",
	  MCP_REQ_ANY, _toolAddBreakpoint },
	{ "add_watchpoint",
	  "Pause execution when memory in [address, address+length) is accessed. type: write (default), read, rw, or change (writes that change the value). Returns the watchpoint id.",
	  "{\"type\":\"object\",\"properties\":{\"address\":" ADDRESS_SCHEMA("Start address.") ",\"length\":{\"type\":\"integer\",\"minimum\":1,\"default\":1},\"type\":{\"type\":\"string\",\"enum\":[\"write\",\"read\",\"rw\",\"change\"],\"default\":\"write\"},\"segment\":{\"type\":\"integer\",\"default\":-1}},\"required\":[\"address\"]}",
	  MCP_REQ_ANY, _toolAddWatchpoint },
	{ "remove_breakpoint",
	  "Remove a breakpoint or watchpoint by id.",
	  "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"]}",
	  MCP_REQ_ANY, _toolRemoveBreakpoint },
	{ "list_breakpoints",
	  "List breakpoints and watchpoints set through MCP.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolListBreakpoints },
	{ "add_cheat",
	  "Add a cheat that mGBA applies every frame. Either pass 'code' (GameShark, Action Replay, CodeBreaker or VBA codes; several lines allowed; format is auto-detected) or 'address' + 'value' (+ 'width') to keep a value frozen, e.g. infinite money once its address is known. Cheats are saved with the game like ones entered in the GUI.",
	  "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\",\"description\":\"Cheat code(s), e.g. \\\"82025D3C 03E7\\\". Whitespace or newlines between codes.\"},\"type\":{\"type\":\"string\",\"enum\":[\"auto\",\"gameshark\",\"action_replay\",\"codebreaker\",\"vba\",\"game_genie\"],\"default\":\"auto\"},\"address\":" ADDRESS_SCHEMA("Address to freeze (instead of 'code').") ",\"value\":{\"type\":[\"integer\",\"string\"],\"description\":\"Value to keep at 'address'.\"},\"width\":{\"type\":\"integer\",\"enum\":[1,2,4],\"default\":1},\"name\":{\"type\":\"string\",\"description\":\"Label shown in list_cheats and the GUI cheats window.\"},\"enabled\":{\"type\":\"boolean\",\"default\":true}}}",
	  MCP_REQ_ANY, _toolAddCheat },
	{ "list_cheats",
	  "List cheats (index, name, enabled, codes), including ones added in the GUI.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolListCheats },
	{ "set_cheat_enabled",
	  "Turn a cheat on or off by its index from list_cheats.",
	  "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0},\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"index\",\"enabled\"]}",
	  MCP_REQ_ANY, _toolSetCheatEnabled },
	{ "remove_cheat",
	  "Delete a cheat by its index from list_cheats. Later cheats are renumbered.",
	  "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0}},\"required\":[\"index\"]}",
	  MCP_REQ_ANY, _toolRemoveCheat },
	{ "memory_search_start",
	  "Start a value search to find where the game stores a number (money, HP, lives...). Search for the value shown on screen (condition=equal), or use condition=unknown to snapshot all memory when the number isn't shown directly. Then change the value in the game and call memory_search_refine until few candidates remain. Searches work RAM by default.",
	  "{\"type\":\"object\",\"properties\":{\"condition\":{\"type\":\"string\",\"enum\":[\"equal\",\"greater\",\"less\",\"unknown\"],\"description\":\"Default: equal if 'value' is given, otherwise unknown.\"},\"value\":{\"type\":[\"integer\",\"string\"]},\"width\":{\"type\":\"integer\",\"enum\":[1,2,4],\"default\":1,\"description\":\"Bytes per value. Use 2 or 4 for numbers that can exceed 255.\"},\"region\":{\"type\":\"string\",\"enum\":[\"ram\",\"all\"],\"default\":\"ram\",\"description\":\"ram = work RAM only; all = every writable region (VRAM, I/O, save RAM...).\"},\"max_results\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":200,\"default\":20}}}",
	  MCP_REQ_ANY, _toolSearchStart },
	{ "memory_search_refine",
	  "Narrow the current value search, comparing each candidate's current value against the given value or against its value at the previous step. Typical loop: change the value in the game (spend money, take damage), then refine with equal + the new value, or decreased / increased / changed / unchanged / changed_by (delta, may be negative).",
	  "{\"type\":\"object\",\"properties\":{\"condition\":{\"type\":\"string\",\"enum\":[\"equal\",\"greater\",\"less\",\"changed\",\"unchanged\",\"increased\",\"decreased\",\"changed_by\"]},\"value\":{\"type\":[\"integer\",\"string\"],\"description\":\"Needed for equal, greater, less and changed_by.\"},\"max_results\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":200,\"default\":20}},\"required\":[\"condition\"]}",
	  MCP_REQ_ANY, _toolSearchRefine },
	{ "memory_search_results",
	  "Show the current value search candidates with their current values.",
	  "{\"type\":\"object\",\"properties\":{\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200,\"default\":50}}}",
	  MCP_REQ_ANY, _toolSearchResults },
	{ "save_state",
	  "Save a savestate to a numbered slot (1-9) or to a file path.",
	  "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":9},\"path\":{\"type\":\"string\"}}}",
	  MCP_REQ_ANY, _toolSaveState },
	{ "load_state",
	  "Load a savestate from a numbered slot (1-9) or a file path.",
	  "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":9},\"path\":{\"type\":\"string\"}}}",
	  MCP_REQ_QUIESCENT, _toolLoadState },
	{ 0 }
};

static const struct MCPTool* _findTool(const char* name) {
	size_t i;
	for (i = 0; _tools[i].name; ++i) {
		if (strcmp(_tools[i].name, name) == 0) {
			return &_tools[i];
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------------------
 * JSON-RPC / MCP
 * ------------------------------------------------------------------------- */

static cJSON* _rpcError(const cJSON* id, int code, const char* message) {
	cJSON* response = cJSON_CreateObject();
	cJSON_AddStringToObject(response, "jsonrpc", "2.0");
	cJSON_AddItemToObject(response, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
	cJSON* error = cJSON_AddObjectToObject(response, "error");
	cJSON_AddNumberToObject(error, "code", code);
	cJSON_AddStringToObject(error, "message", message);
	return response;
}

/* 2026-07-28: cacheable results (lists, discovery) MUST carry ttlMs and cacheScope. */
#define MCP_STATIC_TTL_MS (5 * 60 * 1000)

static cJSON* _rpcResultCached(const cJSON* id, cJSON* result, bool modern, bool cacheable) {
	if (modern) {
		/* Stateless revisions tag every result and identify the server in _meta. */
		cJSON_AddStringToObject(result, "resultType", "complete");
		if (cacheable) {
			/* These only change with the binary. */
			cJSON_AddNumberToObject(result, "ttlMs", MCP_STATIC_TTL_MS);
			cJSON_AddStringToObject(result, "cacheScope", "public");
		}
		cJSON* meta = cJSON_AddObjectToObject(result, "_meta");
		cJSON* info = cJSON_AddObjectToObject(meta, MCP_META_SERVER_INFO);
		cJSON_AddStringToObject(info, "name", MCP_SERVER_NAME);
		cJSON_AddStringToObject(info, "version", projectVersion);
	}
	cJSON* response = cJSON_CreateObject();
	cJSON_AddStringToObject(response, "jsonrpc", "2.0");
	cJSON_AddItemToObject(response, "id", cJSON_Duplicate(id, true));
	cJSON_AddItemToObject(response, "result", result);
	return response;
}

static cJSON* _rpcResult(const cJSON* id, cJSON* result, bool modern) {
	return _rpcResultCached(id, result, modern, false);
}

static cJSON* _versionList(void) {
	cJSON* list = cJSON_CreateArray();
	size_t i;
	for (i = 0; _modernVersions[i]; ++i) {
		cJSON_AddItemToArray(list, cJSON_CreateString(_modernVersions[i]));
	}
	for (i = 0; _legacyVersions[i]; ++i) {
		cJSON_AddItemToArray(list, cJSON_CreateString(_legacyVersions[i]));
	}
	return list;
}

static cJSON* _capabilities(void) {
	cJSON* caps = cJSON_CreateObject();
	cJSON* tools = cJSON_AddObjectToObject(caps, "tools");
	cJSON_AddBoolToObject(tools, "listChanged", false);
	return caps;
}

static const char* _instructions =
	"mGBA emulator control. Typical loop for playing: pause, then press_buttons / run_frames with screenshot=true "
	"to advance deterministically and look at the result. For reverse engineering: read_memory, add_watchpoint on a "
	"RAM value, resume, then get_status / get_registers / disassemble when it triggers. For cheats: find a value's "
	"address with memory_search_start / memory_search_refine, confirm it with write_memory, then add_cheat with "
	"address + value to freeze it.";

static enum MCPRequirement _requestRequirement(const cJSON* request) {
	const cJSON* method = cJSON_GetObjectItemCaseSensitive(request, "method");
	if (!cJSON_IsString(method) || strcmp(method->valuestring, "tools/call") != 0) {
		return MCP_REQ_ANY;
	}
	const cJSON* params = cJSON_GetObjectItemCaseSensitive(request, "params");
	const cJSON* name = cJSON_GetObjectItemCaseSensitive(params, "name");
	if (!cJSON_IsString(name)) {
		return MCP_REQ_ANY;
	}
	const struct MCPTool* tool = _findTool(name->valuestring);
	return tool ? tool->requirement : MCP_REQ_ANY;
}

/* Returns the response object, or NULL for notifications. */
static cJSON* _handleRpc(struct MCPServer* server, const cJSON* request, const struct MCPHttpRequest* http, enum MCPContext context, int* httpStatus) {
	if (!cJSON_IsObject(request)) {
		return _rpcError(NULL, JSONRPC_INVALID_REQUEST, "Invalid request");
	}
	const cJSON* id = cJSON_GetObjectItemCaseSensitive(request, "id");
	const cJSON* method = cJSON_GetObjectItemCaseSensitive(request, "method");
	const cJSON* params = cJSON_GetObjectItemCaseSensitive(request, "params");
	if (!cJSON_IsString(method)) {
		if (!id) {
			/* A response from the client (we never send requests); ignore it. */
			return NULL;
		}
		return _rpcError(id, JSONRPC_INVALID_REQUEST, "Missing method");
	}
	const char* name = method->valuestring;
	if (!id) {
		/* Notifications: notifications/initialized, notifications/cancelled, ... */
		return NULL;
	}

	const cJSON* meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
	bool modern = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(meta, MCP_META_PROTOCOL_VERSION)) ||
	              strcmp(name, "server/discover") == 0 ||
	              (http->protocolVersion[0] && !_inList(_legacyVersions, http->protocolVersion));

	if (strcmp(name, "initialize") == 0) {
		const cJSON* requested = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
		const char* version = _legacyVersions[0];
		if (cJSON_IsString(requested) && _inList(_legacyVersions, requested->valuestring)) {
			version = requested->valuestring;
		}
		cJSON* result = cJSON_CreateObject();
		cJSON_AddStringToObject(result, "protocolVersion", version);
		cJSON_AddItemToObject(result, "capabilities", _capabilities());
		cJSON* info = cJSON_AddObjectToObject(result, "serverInfo");
		cJSON_AddStringToObject(info, "name", MCP_SERVER_NAME);
		cJSON_AddStringToObject(info, "version", projectVersion);
		cJSON_AddStringToObject(result, "instructions", _instructions);
		return _rpcResult(id, result, false);
	}
	if (strcmp(name, "server/discover") == 0) {
		cJSON* result = cJSON_CreateObject();
		cJSON_AddItemToObject(result, "supportedVersions", _versionList());
		cJSON_AddItemToObject(result, "capabilities", _capabilities());
		cJSON_AddStringToObject(result, "instructions", _instructions);
		return _rpcResultCached(id, result, true, true);
	}
	if (strcmp(name, "ping") == 0 && !modern) {
		return _rpcResult(id, cJSON_CreateObject(), false);
	}
	if (strcmp(name, "tools/list") == 0) {
		cJSON* result = cJSON_CreateObject();
		cJSON* list = cJSON_AddArrayToObject(result, "tools");
		size_t i;
		for (i = 0; _tools[i].name; ++i) {
			cJSON* tool = cJSON_CreateObject();
			cJSON_AddStringToObject(tool, "name", _tools[i].name);
			cJSON_AddStringToObject(tool, "description", _tools[i].description);
			cJSON* schema = cJSON_Parse(_tools[i].inputSchema);
			if (!schema) {
				mLOG(MCP, ERROR, "Invalid schema for tool %s", _tools[i].name);
				schema = cJSON_Parse(EMPTY_SCHEMA);
			}
			cJSON_AddItemToObject(tool, "inputSchema", schema);
			cJSON_AddItemToArray(list, tool);
		}
		return _rpcResultCached(id, result, modern, true);
	}
	if (strcmp(name, "tools/call") == 0) {
		const cJSON* toolName = cJSON_GetObjectItemCaseSensitive(params, "name");
		if (!cJSON_IsString(toolName)) {
			return _rpcError(id, JSONRPC_INVALID_PARAMS, "Missing tool name");
		}
		const struct MCPTool* tool = _findTool(toolName->valuestring);
		if (!tool) {
			return _rpcError(id, JSONRPC_INVALID_PARAMS, "Unknown tool");
		}
		const cJSON* args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
		cJSON* empty = NULL;
		if (!cJSON_IsObject(args)) {
			empty = cJSON_CreateObject();
			args = empty;
		}
		struct MCPToolResult toolResult = { cJSON_CreateArray(), false };
		if (tool->requirement == MCP_REQ_EXECUTE && context == MCP_CONTEXT_IDLE) {
			_resultError(&toolResult, "The emulator is paused from the frontend. Resume it there first, then use the 'pause' tool to step under MCP control.");
		} else {
			mLOG(MCP, DEBUG, "tools/call %s", tool->name);
			tool->handler(server, args, context, &toolResult);
		}
		cJSON_Delete(empty);
		cJSON* result = cJSON_CreateObject();
		cJSON_AddItemToObject(result, "content", toolResult.content);
		cJSON_AddBoolToObject(result, "isError", toolResult.isError);
		return _rpcResult(id, result, modern);
	}
	if (strcmp(name, "resources/list") == 0 || strcmp(name, "prompts/list") == 0 || strcmp(name, "resources/templates/list") == 0) {
		cJSON* result = cJSON_CreateObject();
		const char* key = strcmp(name, "prompts/list") == 0 ? "prompts" : (strcmp(name, "resources/list") == 0 ? "resources" : "resourceTemplates");
		cJSON_AddArrayToObject(result, key);
		return _rpcResultCached(id, result, modern, true);
	}
	if (modern) {
		*httpStatus = 404;
	}
	return _rpcError(id, JSONRPC_METHOD_NOT_FOUND, "Method not found");
}

/* ---------------------------------------------------------------------------
 * HTTP transport
 * ------------------------------------------------------------------------- */

static void _closeConnection(struct MCPConnection* conn) {
	if (!SOCKET_FAILED(conn->socket)) {
		SocketClose(conn->socket);
	}
	conn->socket = INVALID_SOCKET;
	free(conn->buffer);
	conn->buffer = NULL;
	conn->size = 0;
	conn->capacity = 0;
}

static bool _sendAll(Socket socket, const char* data, size_t size) {
	/* Responses are small and the peer is local; just block until they are out. */
	SocketSetBlocking(socket, true);
	bool ok = true;
	while (size) {
		ssize_t sent = SocketSend(socket, data, size);
		if (sent <= 0) {
			ok = false;
			break;
		}
		data += sent;
		size -= sent;
	}
	SocketSetBlocking(socket, false);
	return ok;
}

static const char* _statusText(int status) {
	switch (status) {
	case 200:
		return "OK";
	case 202:
		return "Accepted";
	case 204:
		return "No Content";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 413:
		return "Payload Too Large";
	default:
		return "Internal Server Error";
	}
}

static bool _sendHttp(struct MCPConnection* conn, int status, const char* contentType, const char* body, const char* extraHeaders) {
	size_t bodyLength = body ? strlen(body) : 0;
	char header[512];
	int headerLength = snprintf(header, sizeof(header),
	    "HTTP/1.1 %d %s\r\n"
	    "Content-Length: %zu\r\n"
	    "%s%s%s"
	    "Cache-Control: no-store\r\n"
	    "Connection: keep-alive\r\n"
	    "%s"
	    "\r\n",
	    status, _statusText(status), bodyLength,
	    contentType ? "Content-Type: " : "", contentType ? contentType : "", contentType ? "\r\n" : "",
	    extraHeaders ? extraHeaders : "");
	if (!_sendAll(conn->socket, header, headerLength)) {
		return false;
	}
	return !bodyLength || _sendAll(conn->socket, body, bodyLength);
}

static bool _sendJson(struct MCPConnection* conn, int status, const cJSON* json) {
	char* body = cJSON_PrintUnformatted(json);
	bool ok = _sendHttp(conn, status, "application/json", body, NULL);
	cJSON_free(body);
	return ok;
}

static void _copyHeaderValue(char* out, size_t outSize, const char* value, size_t length) {
	while (length && (*value == ' ' || *value == '\t')) {
		++value;
		--length;
	}
	while (length && (value[length - 1] == ' ' || value[length - 1] == '\t')) {
		--length;
	}
	if (length >= outSize) {
		length = outSize - 1;
	}
	memcpy(out, value, length);
	out[length] = '\0';
}

/* Returns false if the request head is complete but malformed. */
static bool _parseHttpHead(struct MCPConnection* conn, struct MCPHttpRequest* request, bool* complete) {
	*complete = false;
	memset(request, 0, sizeof(*request));
	const char* end = NULL;
	size_t i;
	for (i = 0; i + 3 < conn->size; ++i) {
		if (memcmp(&conn->buffer[i], "\r\n\r\n", 4) == 0) {
			end = &conn->buffer[i];
			break;
		}
	}
	if (!end) {
		return conn->size < 16384;
	}
	*complete = true;
	request->headerLength = end - conn->buffer + 4;

	const char* line = conn->buffer;
	const char* lineEnd = strstr(line, "\r\n");
	if (sscanf(line, "%15s %255s", request->method, request->path) != 2) {
		return false;
	}
	char* query = strchr(request->path, '?');
	if (query) {
		*query = '\0';
	}
	while (lineEnd && lineEnd < end) {
		line = lineEnd + 2;
		lineEnd = strstr(line, "\r\n");
		if (!lineEnd || line >= end) {
			break;
		}
		const char* colon = memchr(line, ':', lineEnd - line);
		if (!colon) {
			continue;
		}
		size_t nameLength = colon - line;
		const char* value = colon + 1;
		size_t valueLength = lineEnd - value;
		if (nameLength == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
			char number[32];
			_copyHeaderValue(number, sizeof(number), value, valueLength);
			request->contentLength = strtoul(number, NULL, 10);
		} else if (nameLength == 6 && strncasecmp(line, "Origin", 6) == 0) {
			_copyHeaderValue(request->origin, sizeof(request->origin), value, valueLength);
		} else if (nameLength == 20 && strncasecmp(line, "MCP-Protocol-Version", 20) == 0) {
			_copyHeaderValue(request->protocolVersion, sizeof(request->protocolVersion), value, valueLength);
		}
	}
	return true;
}

static bool _originAllowed(const char* origin) {
	if (!origin[0] || strcmp(origin, "null") == 0) {
		return true;
	}
	static const char* const allowed[] = {
		"http://localhost", "https://localhost", "http://127.0.0.1", "https://127.0.0.1", "http://[::1]", "https://[::1]", NULL
	};
	size_t i;
	for (i = 0; allowed[i]; ++i) {
		size_t length = strlen(allowed[i]);
		if (strncmp(origin, allowed[i], length) == 0 && (origin[length] == '\0' || origin[length] == ':')) {
			return true;
		}
	}
	return false;
}

static enum MCPProcessResult _processRequest(struct MCPServer* server, struct MCPConnection* conn, enum MCPContext context) {
	struct MCPHttpRequest http;
	bool complete;
	if (!_parseHttpHead(conn, &http, &complete)) {
		_sendHttp(conn, 400, "text/plain", "Bad request", NULL);
		_closeConnection(conn);
		return MCP_CLOSED;
	}
	if (!complete) {
		return MCP_NEED_MORE;
	}
	if (http.contentLength > MCP_MAX_REQUEST) {
		_sendHttp(conn, 413, "text/plain", "Request too large", NULL);
		_closeConnection(conn);
		return MCP_CLOSED;
	}
	size_t total = http.headerLength + http.contentLength;
	if (conn->size < total) {
		return MCP_NEED_MORE;
	}

	bool keep = true;
	if (!_originAllowed(http.origin)) {
		/* DNS rebinding protection: only local pages may talk to us. */
		keep = _sendHttp(conn, 403, "text/plain", "Forbidden origin", NULL);
	} else if (strcmp(http.path, "/mcp") != 0 && strcmp(http.path, "/") != 0) {
		keep = _sendHttp(conn, 404, "text/plain", "Not found; the MCP endpoint is /mcp", NULL);
	} else if (strcmp(http.method, "POST") != 0) {
		keep = _sendHttp(conn, 405, "text/plain", "Only POST is supported", "Allow: POST\r\n");
	} else {
		char* body = malloc(http.contentLength + 1);
		memcpy(body, &conn->buffer[http.headerLength], http.contentLength);
		body[http.contentLength] = '\0';
		cJSON* json = cJSON_Parse(body);
		free(body);
		if (!json) {
			cJSON* error = _rpcError(NULL, JSONRPC_PARSE_ERROR, "Parse error");
			keep = _sendJson(conn, 400, error);
			cJSON_Delete(error);
		} else {
			/* Anything that must run outside a frame waits for the paused loop. */
			if (context == MCP_CONTEXT_RUNNING) {
				enum MCPRequirement requirement = MCP_REQ_ANY;
				if (cJSON_IsArray(json)) {
					const cJSON* item;
					cJSON_ArrayForEach(item, json) {
						enum MCPRequirement r = _requestRequirement(item);
						if (r > requirement) {
							requirement = r;
						}
					}
				} else {
					requirement = _requestRequirement(json);
				}
				if (requirement != MCP_REQ_ANY) {
					cJSON_Delete(json);
					server->resumeAfterDeferred = requirement == MCP_REQ_QUIESCENT;
					_pause(server);
					return MCP_DEFERRED;
				}
			}
			int status = 200;
			cJSON* response;
			if (cJSON_IsArray(json)) {
				response = cJSON_CreateArray();
				const cJSON* item;
				cJSON_ArrayForEach(item, json) {
					cJSON* single = _handleRpc(server, item, &http, context, &status);
					if (single) {
						cJSON_AddItemToArray(response, single);
					}
				}
				if (!cJSON_GetArraySize(response)) {
					cJSON_Delete(response);
					response = NULL;
				}
			} else {
				response = _handleRpc(server, json, &http, context, &status);
			}
			cJSON_Delete(json);
			if (response) {
				keep = _sendJson(conn, status, response);
				cJSON_Delete(response);
			} else {
				keep = _sendHttp(conn, 202, NULL, NULL, NULL);
			}
		}
	}

	memmove(conn->buffer, &conn->buffer[total], conn->size - total);
	conn->size -= total;
	if (!keep) {
		_closeConnection(conn);
		return MCP_CLOSED;
	}
	return MCP_PROCESSED;
}

static void _acceptConnections(struct MCPServer* server) {
	while (true) {
		Socket socket = SocketAccept(server->listenSocket, NULL);
		if (SOCKET_FAILED(socket)) {
			return;
		}
		size_t i;
		struct MCPConnection* slot = NULL;
		for (i = 0; i < MCP_SERVER_MAX_CONNECTIONS; ++i) {
			if (SOCKET_FAILED(server->connections[i].socket)) {
				slot = &server->connections[i];
				break;
			}
		}
		if (!slot) {
			mLOG(MCP, WARN, "Too many connections; dropping one");
			SocketClose(socket);
			continue;
		}
		SocketSetBlocking(socket, false);
		SocketSetTCPPush(socket, 1);
		slot->socket = socket;
		slot->size = 0;
		mLOG(MCP, DEBUG, "Client connected");
	}
}

/* Drain readable data. Returns false if the connection was closed. */
static bool _readConnection(struct MCPConnection* conn) {
	while (true) {
		if (conn->capacity - conn->size < 4096) {
			size_t capacity = conn->capacity ? conn->capacity * 2 : 8192;
			if (capacity > MCP_MAX_REQUEST + 65536) {
				_closeConnection(conn);
				return false;
			}
			char* buffer = realloc(conn->buffer, capacity);
			if (!buffer) {
				_closeConnection(conn);
				return false;
			}
			conn->buffer = buffer;
			conn->capacity = capacity;
		}
		ssize_t received = SocketRecv(conn->socket, &conn->buffer[conn->size], conn->capacity - conn->size - 1);
		if (received > 0) {
			conn->size += received;
			continue;
		}
		if (received < 0 && SocketWouldBlock()) {
			return true;
		}
		_closeConnection(conn);
		return false;
	}
}

static bool _hasCompleteRequest(const struct MCPConnection* conn) {
	size_t i;
	for (i = 0; i + 3 < conn->size; ++i) {
		if (memcmp(&conn->buffer[i], "\r\n\r\n", 4) == 0) {
			return true;
		}
	}
	return false;
}

static void _poll(struct MCPServer* server, enum MCPContext context, int32_t timeoutMs) {
	if (server->busy || SOCKET_FAILED(server->listenSocket)) {
		return;
	}
	server->busy = true;

	size_t i;
	bool pending = false;
	for (i = 0; i < MCP_SERVER_MAX_CONNECTIONS; ++i) {
		if (!SOCKET_FAILED(server->connections[i].socket) && _hasCompleteRequest(&server->connections[i])) {
			pending = true;
		}
	}
	if (timeoutMs > 0 && !pending) {
		Socket reads[MCP_SERVER_MAX_CONNECTIONS + 1];
		size_t n = 0;
		reads[n++] = server->listenSocket;
		for (i = 0; i < MCP_SERVER_MAX_CONNECTIONS; ++i) {
			if (!SOCKET_FAILED(server->connections[i].socket)) {
				reads[n++] = server->connections[i].socket;
			}
		}
		SocketPoll(n, reads, NULL, NULL, timeoutMs);
	}

	_acceptConnections(server);
	for (i = 0; i < MCP_SERVER_MAX_CONNECTIONS; ++i) {
		struct MCPConnection* conn = &server->connections[i];
		if (SOCKET_FAILED(conn->socket)) {
			continue;
		}
		if (!_readConnection(conn)) {
			continue;
		}
		enum MCPProcessResult status = MCP_NEED_MORE;
		while (conn->size) {
			status = _processRequest(server, conn, context);
			if (status != MCP_PROCESSED) {
				break;
			}
		}
		if (status == MCP_DEFERRED) {
			/* Everything else waits for the paused loop too. */
			break;
		}
	}

	if (context == MCP_CONTEXT_PAUSED && server->resumeAfterDeferred) {
		/* A request that arrived mid-frame only paused to run safely; it has run now. */
		server->resumeAfterDeferred = false;
		if (server->d.isPaused) {
			_resume(server);
		}
	}

	server->busy = false;
}

/* ---------------------------------------------------------------------------
 * Debugger module hooks
 * ------------------------------------------------------------------------- */

static void _frameEnded(void* context) {
	struct MCPServer* server = context;
	if (server->busy || !server->d.p) {
		return;
	}
	if (server->timedKeysFrames > 0 && --server->timedKeysFrames == 0) {
		struct mCore* core = _core(server);
		core->clearKeys(core, server->timedKeys);
		server->timedKeys = 0;
	}
	_poll(server, MCP_CONTEXT_RUNNING, 0);
}

static void _attachCallbacks(struct MCPServer* server) {
	if (server->callbacksAttached || !server->d.p || !server->d.p->core) {
		return;
	}
	memset(&server->callbacks, 0, sizeof(server->callbacks));
	server->callbacks.context = server;
	server->callbacks.videoFrameEnded = _frameEnded;
	server->d.p->core->addCoreCallbacks(server->d.p->core, &server->callbacks);
	server->callbacksAttached = true;
}

static void _detachCallbacks(struct MCPServer* server) {
	if (!server->callbacksAttached || !server->d.p || !server->d.p->core) {
		return;
	}
	server->d.p->core->removeCoreCallbacks(server->d.p->core, &server->callbacks);
	server->callbacksAttached = false;
}

static void _mcpInit(struct mDebuggerModule* module) {
	_attachCallbacks((struct MCPServer*) module);
}

static void _mcpDeinit(struct mDebuggerModule* module) {
	struct MCPServer* server = (struct MCPServer*) module;
	_detachCallbacks(server);
	/* Search results belong to the game that is going away. */
	mCoreMemorySearchResultsClear(&server->searchResults);
	server->searchActive = false;
	MCPServerShutdown(server);
}

static void _mcpPaused(struct mDebuggerModule* module, int32_t timeoutMs) {
	struct MCPServer* server = (struct MCPServer*) module;
	_attachCallbacks(server);
	_poll(server, MCP_CONTEXT_PAUSED, timeoutMs);
}

static void _mcpUpdate(struct mDebuggerModule* module) {
	struct MCPServer* server = (struct MCPServer*) module;
	_attachCallbacks(server);
	/* The update hook also runs while another thread has interrupted the core thread to
	 * use the core itself (e.g. the Qt UI). Only serve requests while it is truly paused.
	 * We are on the core thread and it holds the state mutex here, so reading is safe. */
	struct mCoreThread* thread = mCoreThreadGet();
	if (thread && thread->impl && thread->impl->state != mTHREAD_PAUSED) {
		return;
	}
	_poll(server, MCP_CONTEXT_IDLE, 0);
}

static void _mcpCustom(struct mDebuggerModule* module) {
	struct MCPServer* server = (struct MCPServer*) module;
	_poll(server, MCP_CONTEXT_RUNNING, 0);
}

static void _mcpEntered(struct mDebuggerModule* module, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info) {
	struct MCPServer* server = (struct MCPServer*) module;
	if (reason == DEBUGGER_ENTER_ATTACHED) {
		return;
	}
	struct MCPStopInfo* stop = &server->lastStop;
	memset(stop, 0, sizeof(*stop));
	stop->valid = true;
	stop->reason = reason;
	stop->pointId = -1;
	stop->frame = server->d.p->core->frameCounter(server->d.p->core);
	if (info) {
		stop->address = info->address;
		stop->pointId = info->pointId;
		stop->width = info->width;
		if (reason == DEBUGGER_ENTER_WATCHPOINT) {
			stop->oldValue = info->type.wp.oldValue;
			stop->newValue = info->type.wp.newValue;
			stop->accessType = info->type.wp.accessType;
		}
	}
	if (reason != DEBUGGER_ENTER_MANUAL) {
		server->stopHit = true;
		mLOG(MCP, INFO, "Stopped: %s at 0x%08X", _reasonName(reason), stop->address);
	}
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void MCPServerCreate(struct MCPServer* server) {
	memset(server, 0, sizeof(*server));
	server->listenSocket = INVALID_SOCKET;
	size_t i;
	for (i = 0; i < MCP_SERVER_MAX_CONNECTIONS; ++i) {
		server->connections[i].socket = INVALID_SOCKET;
	}
	mCoreMemorySearchResultsInit(&server->searchResults, 0);
	server->d.type = DEBUGGER_CUSTOM;
	server->d.init = _mcpInit;
	server->d.deinit = _mcpDeinit;
	server->d.paused = _mcpPaused;
	server->d.update = _mcpUpdate;
	server->d.entered = _mcpEntered;
	server->d.custom = _mcpCustom;
	server->d.interrupt = NULL;
}

bool MCPServerListen(struct MCPServer* server, int port, const struct Address* bindAddress) {
	if (!SOCKET_FAILED(server->listenSocket)) {
		MCPServerShutdown(server);
	}
#ifndef _WIN32
	/* A client hanging up mid-response must not kill the emulator. */
	signal(SIGPIPE, SIG_IGN);
#endif
	/* Not every frontend starts Winsock (the headless one doesn't); it is refcounted, so this is safe. */
	SocketSubsystemInit();
	struct Address localhost = { .version = IPV4, .ipv4 = 0x7F000001 };
	if (!bindAddress) {
		bindAddress = &localhost;
	}
	server->listenSocket = SocketOpenTCP(port, bindAddress);
	if (SOCKET_FAILED(server->listenSocket)) {
		mLOG(MCP, ERROR, "Couldn't open MCP socket on port %d", port);
		return false;
	}
	if (!SocketSetBlocking(server->listenSocket, false) || SocketListen(server->listenSocket, 8)) {
		mLOG(MCP, ERROR, "Couldn't listen on port %d", port);
		SocketClose(server->listenSocket);
		server->listenSocket = INVALID_SOCKET;
		return false;
	}
	mLOG(MCP, INFO, "Listening on http://127.0.0.1:%d/mcp", port);
	return true;
}

void MCPServerShutdown(struct MCPServer* server) {
	size_t i;
	for (i = 0; i < MCP_SERVER_MAX_CONNECTIONS; ++i) {
		_closeConnection(&server->connections[i]);
	}
	if (!SOCKET_FAILED(server->listenSocket)) {
		SocketClose(server->listenSocket);
		server->listenSocket = INVALID_SOCKET;
	}
	if (server->d.p && server->d.isPaused) {
		server->d.isPaused = false;
		mDebuggerUpdatePaused(server->d.p);
	}
}

bool MCPServerIsListening(const struct MCPServer* server) {
	return !SOCKET_FAILED(server->listenSocket);
}
