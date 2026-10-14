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
#include <mgba/internal/debugger/parser.h>
#include <mgba/internal/debugger/stack-trace.h>
#include <mgba-util/formatting.h>
#include <mgba-util/image.h>
#include <mgba/core/version.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#ifdef M_CORE_GBA
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/decoder.h>
#include <mgba/internal/gba/bios.h>
#include <mgba/internal/gba/cheats.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/timer.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/cheats.h>
#endif

#include "third-party/cjson/cJSON.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>

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

static void _rememberUndoState(struct MCPServer* server);
static void _sequenceTick(struct MCPServer* server);
static struct ParseTree* _parseCondition(const char* string);
static uint8_t* _base64Decode(const char* in, size_t* outSize);
static void* _readFile(const char* path, size_t* size, size_t maxSize);
static bool _writeFile(const char* path, const void* data, size_t size);

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
	UNUSED(context);
	const char* path = _argString(args, "path");
	if (path && path[0]) {
		struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
		bool ok = vf && mCoreTakeScreenshotVF(_core(server), vf);
		if (vf) {
			vf->close(vf);
		}
		if (ok) {
			_resultPrintf(result, "Saved screenshot to %s", path);
		} else {
			_resultError(result, "Could not save a screenshot to %s", path);
			return;
		}
	}
	if (!_argBool(args, "inline", true)) {
		return;
	}
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
	int segment = _argInt(args, "segment", -1);
	const char* format = _argString(args, "format");
	const char* outPath = _argString(args, "path");
	if (outPath || (format && strcmp(format, "base64") == 0)) {
		/* Raw bytes: to a file, or inline as base64. Up to 4 MiB. */
		if (length < 1 || length > 0x400000) {
			_resultError(result, "'length' must be between 1 and 4194304 for base64 or file output");
			return;
		}
		uint8_t* bytes = malloc(length);
		int i;
		for (i = 0; i < length; ++i) {
			bytes[i] = core->rawRead8(core, address + i, segment);
		}
		if (outPath) {
			if (_writeFile(outPath, bytes, length)) {
				_resultPrintf(result, "Wrote %d bytes from 0x%08X to %s", length, address, outPath);
			} else {
				_resultError(result, "Could not write %s", outPath);
			}
		} else {
			char* encoded = _base64Encode(bytes, length);
			cJSON* json = cJSON_CreateObject();
			_addHex(json, "address", address);
			cJSON_AddNumberToObject(json, "length", length);
			cJSON_AddStringToObject(json, "base64", encoded);
			free(encoded);
			_resultJson(result, json);
		}
		free(bytes);
		return;
	}
	if (length < 1 || length > MCP_MAX_READ) {
		_resultError(result, "'length' must be between 1 and %d (or use format=base64 / path for more)", MCP_MAX_READ);
		return;
	}
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

/* I/O registers only react to bus writes; raw (debugger) writes don't reach the hardware logic. */
static bool _isIoAddress(struct mCore* core, uint32_t address) {
#ifdef M_CORE_GBA
	if (_isGBA(core)) {
		return (address >> 24) == 0x04;
	}
#endif
#ifdef M_CORE_GB
	if (core->platform(core) == mPLATFORM_GB) {
		return address >= 0xFF00 && address < 0xFF80;
	}
#endif
	return false;
}

static void _write8(struct mCore* core, uint32_t address, int segment, uint8_t value) {
	if (_isIoAddress(core, address)) {
		core->busWrite8(core, address, value);
	} else {
		core->rawWrite8(core, address, segment, value);
	}
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
	const char* b64 = _argString(args, "base64");
	const char* inPath = _argString(args, "path");
	if (b64 || inPath) {
		size_t size = 0;
		uint8_t* bytes = b64 ? _base64Decode(b64, &size) : _readFile(inPath, &size, 0x400000);
		if (!bytes) {
			_resultError(result, b64 ? "Invalid base64 data" : "Could not read the file (missing, or larger than 4 MiB)");
			return;
		}
		size_t i;
		for (i = 0; i < size; ++i) {
			_write8(core, address + (uint32_t) i, segment, bytes[i]);
		}
		free(bytes);
		_resultPrintf(result, "Wrote %zu bytes at 0x%08X.", size, address);
		return;
	}
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
			_write8(core, address + written, segment, (uint8_t) strtoul(byte, NULL, 16));
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
	bool io = _isIoAddress(core, address);
	switch (width) {
	case 1:
		_write8(core, address, segment, value);
		break;
	case 2:
		if (io) {
			core->busWrite16(core, address, value);
		} else {
			core->rawWrite16(core, address, segment, value);
		}
		break;
	case 4:
		if (io) {
			core->busWrite32(core, address, value);
		} else {
			core->rawWrite32(core, address, segment, value);
		}
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
	const char* condition = _argString(args, "condition");
	if (condition && condition[0]) {
		breakpoint.condition = _parseCondition(condition);
		if (!breakpoint.condition) {
			_resultError(result, "Could not parse condition '%s'. Use mGBA debugger expressions with spaces between parts, e.g. \"r0 == 5\", \"r1 > 0x100\" or \"*0x02000010 == 3\" (* reads one byte). Numbers are decimal unless prefixed with 0x.", condition);
			return;
		}
	}
	struct mDebuggerPlatform* platform = server->d.p->platform;
	ssize_t id = platform->setBreakpoint(platform, &server->d, &breakpoint);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "id", (double) id);
	_addHex(json, "address", address & ~1);
	if (condition && condition[0]) {
		cJSON_AddStringToObject(json, "condition", condition);
	}
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
	_rememberUndoState(server);
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

/* ---------------------------------------------------------------------------
 * Shared helpers for the extended tool set
 * ------------------------------------------------------------------------- */

#ifdef M_CORE_GBA
static struct GBA* _gba(struct mCore* core) {
	return core->platform(core) == mPLATFORM_GBA ? core->board : NULL;
}

static uint16_t _io(struct GBA* gba, uint32_t offset) {
	return gba->memory.io[(offset & 0x3FF) >> 1];
}
#endif

static const char* _tempDir(void) {
	const char* dir = getenv("TEMP");
	if (!dir || !dir[0]) {
		dir = getenv("TMP");
	}
	if (!dir || !dir[0]) {
		dir = getenv("TMPDIR");
	}
	if (!dir || !dir[0]) {
		dir = "/tmp";
	}
	return dir;
}

/* Fills `out` with the caller's path, or a fresh file in the temp directory. */
static const char* _outputPath(const cJSON* args, const char* prefix, const char* ext, char* out, size_t outSize) {
	const char* path = _argString(args, "path");
	if (path && path[0]) {
		return path;
	}
	static unsigned counter = 0;
	snprintf(out, outSize, "%s%cmgba-mcp-%s-%u-%u.%s", _tempDir(), PATH_SEP[0], prefix, (unsigned) time(NULL), ++counter, ext);
	return out;
}

static bool _writeFile(const char* path, const void* data, size_t size) {
	struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
	if (!vf) {
		return false;
	}
	bool ok = vf->write(vf, data, size) == (ssize_t) size;
	vf->close(vf);
	return ok;
}

/* Reads a whole file; caller frees. */
static void* _readFile(const char* path, size_t* size, size_t maxSize) {
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		return NULL;
	}
	ssize_t length = vf->size(vf);
	if (length < 0 || (size_t) length > maxSize) {
		vf->close(vf);
		return NULL;
	}
	void* data = malloc(length ? length : 1);
	if (vf->read(vf, data, length) != length) {
		free(data);
		vf->close(vf);
		return NULL;
	}
	vf->close(vf);
	*size = length;
	return data;
}

static uint8_t* _base64Decode(const char* in, size_t* outSize) {
	size_t length = strlen(in);
	uint8_t* out = malloc(length / 4 * 3 + 3);
	uint32_t acc = 0;
	int bits = 0;
	size_t o = 0;
	size_t i;
	for (i = 0; i < length; ++i) {
		char c = in[i];
		int v;
		if (c >= 'A' && c <= 'Z') {
			v = c - 'A';
		} else if (c >= 'a' && c <= 'z') {
			v = c - 'a' + 26;
		} else if (c >= '0' && c <= '9') {
			v = c - '0' + 52;
		} else if (c == '+') {
			v = 62;
		} else if (c == '/') {
			v = 63;
		} else if (c == '=' || isspace((unsigned char) c)) {
			continue;
		} else {
			free(out);
			return NULL;
		}
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (acc >> bits) & 0xFF;
		}
	}
	*outSize = o;
	return out;
}

/* Adds a PNG of `image` to the result, and also saves it when 'path' is passed. */
static bool _resultImage(struct MCPToolResult* result, const struct mImage* image, const cJSON* args) {
#ifdef USE_PNG
	struct VFile* vf = VFileMemChunk(NULL, 0);
	if (!vf) {
		return false;
	}
	bool ok = mImageSaveVF(image, vf, "png");
	if (ok) {
		size_t size = vf->size(vf);
		void* data = vf->map(vf, size, MAP_READ);
		const char* path = args ? _argString(args, "path") : NULL;
		if (path && path[0]) {
			if (_writeFile(path, data, size)) {
				_resultPrintf(result, "Saved %ux%u PNG to %s", image->width, image->height, path);
			} else {
				_resultError(result, "Could not write %s", path);
			}
		}
		char* encoded = _base64Encode(data, size);
		vf->unmap(vf, data, size);
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "type", "image");
		cJSON_AddStringToObject(item, "mimeType", "image/png");
		cJSON_AddStringToObject(item, "data", encoded);
		cJSON_AddItemToArray(result->content, item);
		free(encoded);
	}
	vf->close(vf);
	return ok;
#else
	UNUSED(image);
	UNUSED(args);
	_resultError(result, "This build has no PNG support.");
	return false;
#endif
}

/* GBA BGR555 to the ARGB8 that mImageSetPixel expects. */
static uint32_t _color555(uint16_t c, bool opaque) {
	uint32_t r = c & 0x1F;
	uint32_t g = (c >> 5) & 0x1F;
	uint32_t b = (c >> 10) & 0x1F;
	r = (r << 3) | (r >> 2);
	g = (g << 3) | (g >> 2);
	b = (b << 3) | (b >> 2);
	return (opaque ? 0xFF000000u : 0) | (r << 16) | (g << 8) | b;
}

static int _clampInt(int value, int low, int high) {
	return value < low ? low : (value > high ? high : value);
}

/* Steps the CPU until `done` says so, a breakpoint/watchpoint hits, or `limit` runs out. */
struct MCPRunUntil {
	uint32_t returnAddress;
	uint32_t stackPointer;
};

static bool _reachedReturn(struct mCore* core, const struct MCPRunUntil* until) {
	uint32_t pc;
	if (!_currentPC(core, &pc, NULL)) {
		return true;
	}
	int32_t sp = 0;
	core->readRegister(core, "sp", &sp);
	return pc == until->returnAddress && (uint32_t) sp >= until->stackPointer;
}

static uint32_t _stepUntilReturn(struct MCPServer* server, const struct MCPRunUntil* until, uint32_t limit) {
	struct mCore* core = _core(server);
	struct mDebuggerPlatform* platform = server->d.p->platform;
	server->stopHit = false;
	uint32_t executed = 0;
	while (executed < limit && !server->stopHit) {
		core->step(core);
		++executed;
		if (platform->hasBreakpoints(platform)) {
			platform->checkBreakpoints(platform);
		}
		if (until && _reachedReturn(core, until)) {
			break;
		}
	}
	return executed;
}

static void _addPosition(struct MCPServer* server, cJSON* json) {
	struct mCore* core = _core(server);
	uint32_t pc;
	bool thumb;
	cJSON_AddNumberToObject(json, "frame", core->frameCounter(core));
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
}

/* ---------------------------------------------------------------------------
 * Debugging: step over / out, backtrace, trace, breakpoint management, CPU state
 * ------------------------------------------------------------------------- */

static void _toolStepOver(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	uint32_t limit = (uint32_t) _clampInt(_argInt(args, "max_instructions", 1000000), 1, 50000000);
	cJSON* json = cJSON_CreateObject();
#ifdef M_CORE_GBA
	if (_isGBA(core)) {
		uint32_t pc;
		bool thumb;
		_currentPC(core, &pc, &thumb);
		struct ARMInstructionInfo info;
		uint32_t size;
		if (thumb) {
			struct ARMInstructionInfo info2, combined;
			ARMDecodeThumb(core->rawRead16(core, pc, -1), &info);
			ARMDecodeThumb(core->rawRead16(core, pc + 2, -1), &info2);
			size = 2;
			if (ARMDecodeThumbCombine(&info, &info2, &combined)) {
				info = combined;
				size = 4;
			}
		} else {
			ARMDecodeARM(core->rawRead32(core, pc, -1), &info);
			size = 4;
		}
		if (info.branchType & ARM_BRANCH_LINKED) {
			int32_t sp = 0;
			core->readRegister(core, "sp", &sp);
			struct MCPRunUntil until = { pc + size, (uint32_t) sp };
			uint32_t executed = _stepUntilReturn(server, &until, limit);
			cJSON_AddStringToObject(json, "kind", "stepped_over_call");
			cJSON_AddNumberToObject(json, "executed", executed);
			if (!server->stopHit && !_reachedReturn(core, &until)) {
				cJSON_AddStringToObject(json, "warning", "The call did not return within max_instructions; execution is paused inside it.");
			}
			_addPosition(server, json);
			_resultJson(result, json);
			return;
		}
	}
#endif
	uint32_t executed = _stepUntilReturn(server, NULL, 1);
	cJSON_AddStringToObject(json, "kind", "single_step");
	cJSON_AddNumberToObject(json, "executed", executed);
	_addPosition(server, json);
	_resultJson(result, json);
}

static void _toolStepOut(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	struct mDebugger* debugger = server->d.p;
	struct mDebuggerPlatform* platform = debugger->platform;
	uint32_t limit = (uint32_t) _clampInt(_argInt(args, "max_instructions", 1000000), 1, 50000000);
	cJSON* json = cJSON_CreateObject();

	/* Preferred: the debugger's call stack, which knows the real return point. */
	if (platform->getStackTraceMode && platform->getStackTraceMode(platform) != STACK_TRACE_DISABLED) {
		struct mStackFrame* frame = mStackTraceGetFrame(&debugger->stackTrace, 0);
		if (frame) {
			frame->breakWhenFinished = true;
			/* mGBA only notices a return when SP rises past the frame, which never happens
			 * for leaf functions that don't touch the stack; also stop right after the call
			 * site (BL is 4 bytes in both modes, Thumb BLX reg is 2). */
			uint32_t callAddress = frame->callAddress;
			uint32_t frameBase = frame->frameBaseAddress;
			struct mDebuggerPlatform* plat = platform;
			server->stopHit = false;
			bool returned = false;
			uint32_t executed = 0;
			while (executed < limit && !server->stopHit) {
				core->step(core);
				++executed;
				plat->checkBreakpoints(plat);
				uint32_t pc;
				int32_t sp = 0;
				_currentPC(core, &pc, NULL);
				core->readRegister(core, "sp", &sp);
				if ((pc == callAddress + 4 || pc == callAddress + 2) && (uint32_t) sp >= frameBase) {
					returned = true;
					break;
				}
			}
			cJSON_AddStringToObject(json, "method", "stack_trace");
			cJSON_AddNumberToObject(json, "executed", executed);
			if (!server->stopHit && !returned) {
				frame = mStackTraceGetFrame(&debugger->stackTrace, 0);
				if (frame) {
					frame->breakWhenFinished = false;
				}
				cJSON_AddStringToObject(json, "warning", "The function did not return within max_instructions.");
			}
			_addPosition(server, json);
			_resultJson(result, json);
			return;
		}
	}

	/* Fallback: run until PC reaches LR. Correct while LR still holds the return
	 * address (leaf functions, or before the function calls anything else). */
	int32_t lr = 0, sp = 0;
	core->readRegister(core, "lr", &lr);
	core->readRegister(core, "sp", &sp);
	struct MCPRunUntil until = { (uint32_t) lr & ~1u, (uint32_t) sp };
	uint32_t executed = _stepUntilReturn(server, &until, limit);
	cJSON_AddStringToObject(json, "method", "link_register");
	cJSON_AddNumberToObject(json, "executed", executed);
	if (!server->stopHit && !_reachedReturn(core, &until)) {
		cJSON_AddStringToObject(json, "warning", "Did not reach the address in LR. For reliable step_out, enable stack tracing (backtrace with enable=true) before entering the function.");
	}
	if (platform->getStackTraceMode && platform->getStackTraceMode(platform) == STACK_TRACE_DISABLED) {
		cJSON_AddStringToObject(json, "hint", "Stack tracing is off, so this used LR. Turn it on with backtrace enable=true to make step_out exact for functions entered afterwards.");
	}
	_addPosition(server, json);
	_resultJson(result, json);
}

static void _toolBacktrace(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mDebugger* debugger = server->d.p;
	struct mDebuggerPlatform* platform = debugger->platform;
	if (!platform->getStackTraceMode) {
		_resultError(result, "Stack tracing is not supported on this platform.");
		return;
	}
	const cJSON* enable = cJSON_GetObjectItemCaseSensitive(args, "enable");
	if (cJSON_IsBool(enable)) {
		platform->setStackTraceMode(platform, cJSON_IsTrue(enable) ? STACK_TRACE_ENABLED : STACK_TRACE_DISABLED);
	}
	cJSON* json = cJSON_CreateObject();
	bool enabled = platform->getStackTraceMode(platform) != STACK_TRACE_DISABLED;
	cJSON_AddBoolToObject(json, "stackTracing", enabled);
	cJSON* frames = cJSON_AddArrayToObject(json, "frames");
	if (enabled) {
		size_t depth = mStackTraceGetDepth(&debugger->stackTrace);
		size_t i;
		for (i = 0; i < depth && i < 64; ++i) {
			char line[512];
			size_t length = sizeof(line);
			mStackTraceFormatFrame(&debugger->stackTrace, debugger->core->symbolTable, i, line, &length);
			line[length < sizeof(line) ? length : sizeof(line) - 1] = '\0';
			cJSON_AddItemToArray(frames, cJSON_CreateString(line));
		}
		if (!depth) {
			cJSON_AddStringToObject(json, "note", "No frames yet: the stack is only tracked for calls made after tracing was enabled.");
		}
	} else {
		cJSON_AddStringToObject(json, "note", "Stack tracing is off. Call backtrace with enable=true; it tracks calls from then on (and makes step_out exact). It slows emulation down somewhat.");
	}
	_resultJson(result, json);
}

static void _toolTrace(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	struct mDebuggerPlatform* platform = server->d.p->platform;
	if (!platform->trace) {
		_resultError(result, "Instruction tracing is not supported on this platform.");
		return;
	}
	const char* path = _argString(args, "path");
	int maxCount = path ? 5000000 : 2000;
	int count = _argInt(args, "count", 100);
	if (count < 1 || count > maxCount) {
		_resultError(result, "'count' must be between 1 and %d%s", maxCount, path ? "" : " (pass 'path' to trace more into a file)");
		return;
	}
	struct VFile* vf = NULL;
	if (path) {
		vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
		if (!vf) {
			_resultError(result, "Could not open %s", path);
			return;
		}
	}
	size_t capacity = path ? 0 : (size_t) count * 160 + 1;
	char* text = path ? NULL : malloc(capacity);
	size_t pos = 0;
	server->stopHit = false;
	int i;
	for (i = 0; i < count && !server->stopHit; ++i) {
		char line[1024];
		size_t length = sizeof(line) - 2;
		platform->trace(platform, line, &length);
		if (length > sizeof(line) - 2) {
			length = sizeof(line) - 2;
		}
		line[length++] = '\n';
		if (vf) {
			vf->write(vf, line, length);
		} else {
			if (pos + length + 1 > capacity) {
				capacity = (capacity + length) * 2;
				text = realloc(text, capacity);
			}
			memcpy(&text[pos], line, length);
			pos += length;
		}
		core->step(core);
		if (platform->hasBreakpoints(platform)) {
			platform->checkBreakpoints(platform);
		}
	}
	if (vf) {
		vf->close(vf);
		_resultPrintf(result, "Traced %d instructions to %s%s", i, path, server->stopHit ? " (stopped early by a breakpoint/watchpoint)" : "");
	} else {
		text[pos] = '\0';
		_resultText(result, text);
		free(text);
		if (server->stopHit) {
			_resultText(result, "(Stopped early by a breakpoint/watchpoint.)");
		}
	}
}

static void _toolSetBreakpointEnabled(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	uint32_t id;
	if (!_argU32(args, "id", &id, true, result)) {
		return;
	}
	const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(args, "enabled");
	if (!cJSON_IsBool(enabled)) {
		_resultError(result, "Missing required boolean argument 'enabled'");
		return;
	}
	struct mDebuggerPlatform* platform = server->d.p->platform;
	if (!platform->toggleBreakpoint || !platform->toggleBreakpoint(platform, (ssize_t) id, cJSON_IsTrue(enabled))) {
		_resultError(result, "No breakpoint or watchpoint with id %u", id);
		return;
	}
	_resultPrintf(result, "%s %u.", cJSON_IsTrue(enabled) ? "Enabled" : "Disabled", id);
}

static void _toolClearBreakpoints(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mDebuggerPlatform* platform = server->d.p->platform;
	size_t removed = 0;
	struct mBreakpointList breakpoints;
	mBreakpointListInit(&breakpoints, 0);
	platform->listBreakpoints(platform, &server->d, &breakpoints);
	size_t i;
	for (i = 0; i < mBreakpointListSize(&breakpoints); ++i) {
		if (platform->clearBreakpoint(platform, mBreakpointListGetPointer(&breakpoints, i)->id)) {
			++removed;
		}
	}
	mBreakpointListDeinit(&breakpoints);
	struct mWatchpointList watchpoints;
	mWatchpointListInit(&watchpoints, 0);
	platform->listWatchpoints(platform, &server->d, &watchpoints);
	for (i = 0; i < mWatchpointListSize(&watchpoints); ++i) {
		if (platform->clearBreakpoint(platform, mWatchpointListGetPointer(&watchpoints, i)->id)) {
			++removed;
		}
	}
	mWatchpointListDeinit(&watchpoints);
	_resultPrintf(result, "Removed %zu breakpoints/watchpoints.", removed);
}

static struct ParseTree* _parseCondition(const char* string) {
	/* The lexer stops at whitespace (the CLI feeds it one word at a time), so lex each
	 * word separately and require every word to be consumed completely. */
	struct LexVector lv;
	LexVectorInit(&lv, 0);
	bool error = false;
	bool any = false;
	const char* p = string;
	while (*p && !error) {
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
		size_t adjusted = lexExpression(&lv, p, length, NULL);
		if (adjusted != length) {
			error = true;
		}
		any = true;
		p += length;
	}
	struct ParseTree* tree = NULL;
	error = error || !any;
	if (!error) {
		tree = parseTreeCreate();
		error = !parseLexedExpression(tree, &lv);
	}
	lexFree(&lv);
	LexVectorClear(&lv);
	LexVectorDeinit(&lv);
	if (error && tree) {
		parseFree(tree);
		tree = NULL;
	}
	return tree;
}

static void _toolGetCpuState(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mCore* core = _core(server);
#ifdef M_CORE_GBA
	struct GBA* gba = _gba(core);
	if (gba) {
		struct ARMCore* cpu = core->cpu;
		static const char* const modeNames[] = { "usr", "fiq", "irq", "svc", "abt", "und", "sys" };
		static const int modeIds[] = { MODE_USER, MODE_FIQ, MODE_IRQ, MODE_SUPERVISOR, MODE_ABORT, MODE_UNDEFINED, MODE_SYSTEM };
		cJSON* json = cJSON_CreateObject();
		const char* mode = "invalid";
		size_t i;
		for (i = 0; i < 7; ++i) {
			if ((int) cpu->cpsr.priv == modeIds[i]) {
				mode = modeNames[i];
			}
		}
		_addHex(json, "cpsr", cpu->cpsr.packed);
		cJSON* flags = cJSON_AddObjectToObject(json, "cpsrDecoded");
		cJSON_AddBoolToObject(flags, "N", cpu->cpsr.n);
		cJSON_AddBoolToObject(flags, "Z", cpu->cpsr.z);
		cJSON_AddBoolToObject(flags, "C", cpu->cpsr.c);
		cJSON_AddBoolToObject(flags, "V", cpu->cpsr.v);
		cJSON_AddBoolToObject(flags, "irqDisabled", cpu->cpsr.i);
		cJSON_AddBoolToObject(flags, "fiqDisabled", cpu->cpsr.f);
		cJSON_AddBoolToObject(flags, "thumb", cpu->cpsr.t);
		cJSON_AddStringToObject(flags, "mode", mode);
		_addHex(json, "spsr", cpu->spsr.packed);
		cJSON_AddBoolToObject(json, "halted", cpu->halted != 0);
		uint16_t ie = _io(gba, GBA_REG_IE), iflag = _io(gba, GBA_REG_IF);
		cJSON_AddBoolToObject(json, "irqMasterEnable", _io(gba, GBA_REG_IME) & 1);
		_addHex(json, "irqPending", ie & iflag);
		cJSON* banked = cJSON_AddObjectToObject(json, "banked");
		static const char* const bankNames[] = { "usr/sys", "fiq", "svc", "abt", "irq", "und" };
		for (i = 0; i < 6; ++i) {
			cJSON* bank = cJSON_AddObjectToObject(banked, bankNames[i]);
			_addHex(bank, "r13", cpu->bankedRegisters[i][5]);
			_addHex(bank, "r14", cpu->bankedRegisters[i][6]);
			if (i) {
				_addHex(bank, "spsr", cpu->bankedSPSRs[i]);
			}
		}
		cJSON_AddStringToObject(json, "note", "Banked values are the saved copies; the current mode's live registers are in get_registers.");
		_resultJson(result, json);
		return;
	}
#endif
	_toolGetRegisters(server, args, context, result);
}

/* ---------------------------------------------------------------------------
 * Memory: dumps, byte search, snapshots/diffs, free space, watches
 * ------------------------------------------------------------------------- */

static const struct mCoreMemoryBlock* _findBlock(struct mCore* core, const char* name) {
	const struct mCoreMemoryBlock* blocks;
	size_t nBlocks = core->listMemoryBlocks(core, &blocks);
	size_t i;
	for (i = 0; i < nBlocks; ++i) {
		if (strcasecmp(blocks[i].internalName, name) == 0 || strcasecmp(blocks[i].shortName, name) == 0) {
			return &blocks[i];
		}
	}
	return NULL;
}

static void* _blockMemory(struct mCore* core, const struct mCoreMemoryBlock* block, size_t* size) {
	void* mem = core->getMemoryBlock(core, block->id, size);
	if (mem && *size > block->end - block->start) {
		*size = block->end - block->start;
	}
	return mem;
}

static void _toolDumpMemory(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	const char* region = _argString(args, "region");
	if (!region) {
		_resultError(result, "Missing required argument 'region' (see list_memory_blocks for names, e.g. wram, iwram, vram, oam, palette, rom)");
		return;
	}
	const struct mCoreMemoryBlock* block = _findBlock(core, region);
	if (!block) {
		_resultError(result, "Unknown region '%s'; see list_memory_blocks", region);
		return;
	}
	size_t size;
	void* mem = _blockMemory(core, block, &size);
	if (!mem || !size) {
		_resultError(result, "Region '%s' is empty or not readable", region);
		return;
	}
	char buffer[PATH_MAX];
	const char* path = _outputPath(args, block->internalName, "bin", buffer, sizeof(buffer));
	if (!_writeFile(path, mem, size)) {
		_resultError(result, "Could not write %s", path);
		return;
	}
	cJSON* json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "region", block->internalName);
	_addHex(json, "start", block->start);
	cJSON_AddNumberToObject(json, "size", (double) size);
	cJSON_AddStringToObject(json, "path", path);
	_resultJson(result, json);
}

static bool _parseHexBytes(const char* text, uint8_t* out, size_t maxSize, size_t* size) {
	*size = 0;
	while (*text) {
		while (*text && (isspace((unsigned char) *text) || *text == ',')) {
			++text;
		}
		if (!*text) {
			break;
		}
		if (text[0] == '?' && text[1] == '?') {
			return false;
		}
		if (!isxdigit((unsigned char) text[0]) || !isxdigit((unsigned char) text[1]) || *size >= maxSize) {
			return false;
		}
		char byte[3] = { text[0], text[1], '\0' };
		out[(*size)++] = (uint8_t) strtoul(byte, NULL, 16);
		text += 2;
	}
	return *size > 0;
}

/* Pattern with "??" wildcards, e.g. "12 ?? 34". */
static bool _parsePattern(const char* text, uint8_t* pattern, uint8_t* mask, size_t maxSize, size_t* size) {
	*size = 0;
	while (*text) {
		while (*text && (isspace((unsigned char) *text) || *text == ',')) {
			++text;
		}
		if (!*text) {
			break;
		}
		if (*size >= maxSize) {
			return false;
		}
		if (text[0] == '?' && text[1] == '?') {
			pattern[*size] = 0;
			mask[*size] = 0;
		} else if (isxdigit((unsigned char) text[0]) && isxdigit((unsigned char) text[1])) {
			char byte[3] = { text[0], text[1], '\0' };
			pattern[*size] = (uint8_t) strtoul(byte, NULL, 16);
			mask[*size] = 0xFF;
		} else {
			return false;
		}
		++*size;
		text += 2;
	}
	return *size > 0;
}

static void _toolSearchBytes(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	uint8_t pattern[256], mask[256];
	size_t length;
	const char* text = _argString(args, "pattern");
	if (!text || !_parsePattern(text, pattern, mask, sizeof(pattern), &length)) {
		_resultError(result, "'pattern' must be hex bytes with optional ?? wildcards, e.g. \"48 45 ?? 4C 4F\" (max 256 bytes)");
		return;
	}
	const char* maskText = _argString(args, "mask");
	if (maskText) {
		uint8_t explicitMask[256];
		size_t maskLength;
		if (!_parseHexBytes(maskText, explicitMask, sizeof(explicitMask), &maskLength) || maskLength != length) {
			_resultError(result, "'mask' must be hex bytes, the same length as 'pattern'");
			return;
		}
		size_t i;
		for (i = 0; i < length; ++i) {
			mask[i] &= explicitMask[i];
		}
	}
	const char* region = _argString(args, "region");
	if (!region) {
		region = "ram";
	}
	int maxResults = _clampInt(_argInt(args, "max_results", 100), 1, 1000);
	const struct mCoreMemoryBlock* blocks;
	size_t nBlocks = core->listMemoryBlocks(core, &blocks);
	cJSON* json = cJSON_CreateObject();
	cJSON* matches = cJSON_AddArrayToObject(json, "matches");
	int found = 0;
	bool truncated = false;
	size_t b;
	for (b = 0; b < nBlocks && !truncated; ++b) {
		const struct mCoreMemoryBlock* block = &blocks[b];
		bool wanted;
		if (strcmp(region, "ram") == 0) {
			wanted = _isRamBlock(block);
		} else if (strcmp(region, "all") == 0) {
			wanted = true;
		} else {
			wanted = strcasecmp(block->internalName, region) == 0 || strcasecmp(block->shortName, region) == 0;
		}
		if (!wanted) {
			continue;
		}
		size_t size;
		const uint8_t* mem = _blockMemory(core, block, &size);
		if (!mem || size < length) {
			continue;
		}
		size_t i;
		for (i = 0; i + length <= size; ++i) {
			size_t j;
			for (j = 0; j < length; ++j) {
				if ((mem[i + j] & mask[j]) != (pattern[j] & mask[j])) {
					break;
				}
			}
			if (j == length) {
				if (found >= maxResults) {
					truncated = true;
					break;
				}
				char hex[16];
				_formatHex(hex, sizeof(hex), block->start + (uint32_t) i);
				cJSON_AddItemToArray(matches, cJSON_CreateString(hex));
				++found;
			}
		}
	}
	cJSON_AddNumberToObject(json, "count", found);
	cJSON_AddBoolToObject(json, "truncated", truncated);
	_resultJson(result, json);
}

static void _toolSnapshotMemory(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mCore* core = _core(server);
	const struct mCoreMemoryBlock* blocks;
	size_t nBlocks = core->listMemoryBlocks(core, &blocks);
	free(server->snapshot);
	server->snapshot = NULL;
	server->snapshotRegions = 0;
	size_t total = 0;
	size_t b;
	for (b = 0; b < nBlocks && server->snapshotRegions < MCP_SNAPSHOT_REGIONS; ++b) {
		size_t size;
		if (!_isRamBlock(&blocks[b]) || !_blockMemory(core, &blocks[b], &size)) {
			continue;
		}
		server->snapshotStart[server->snapshotRegions] = blocks[b].start;
		server->snapshotSize[server->snapshotRegions] = (uint32_t) size;
		++server->snapshotRegions;
		total += size;
	}
	server->snapshot = malloc(total ? total : 1);
	size_t offset = 0;
	unsigned r = 0;
	for (b = 0; b < nBlocks && r < server->snapshotRegions; ++b) {
		size_t size;
		void* mem;
		if (!_isRamBlock(&blocks[b]) || !(mem = _blockMemory(core, &blocks[b], &size))) {
			continue;
		}
		memcpy(&server->snapshot[offset], mem, size);
		offset += size;
		++r;
	}
	server->snapshotFrame = core->frameCounter(core);
	_resultPrintf(result, "Snapshot of %zu bytes of work RAM taken at frame %u. Let the game run or act, then call diff_memory.", total, server->snapshotFrame);
}

static void _toolDiffMemory(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	if (!server->snapshot) {
		_resultError(result, "No snapshot yet; call snapshot_memory first.");
		return;
	}
	int width = _argInt(args, "width", 1);
	if (width != 1 && width != 2 && width != 4) {
		_resultError(result, "'width' must be 1, 2 or 4");
		return;
	}
	const char* filter = _argString(args, "filter");
	if (!filter) {
		filter = "changed";
	}
	int mode;
	if (strcmp(filter, "changed") == 0) {
		mode = 0;
	} else if (strcmp(filter, "increased") == 0) {
		mode = 1;
	} else if (strcmp(filter, "decreased") == 0) {
		mode = 2;
	} else {
		_resultError(result, "'filter' must be changed, increased or decreased");
		return;
	}
	int maxResults = _clampInt(_argInt(args, "max_results", 100), 1, 2000);
	cJSON* json = cJSON_CreateObject();
	cJSON* list = cJSON_AddArrayToObject(json, "changes");
	size_t total = 0;
	size_t offset = 0;
	unsigned r;
	for (r = 0; r < server->snapshotRegions; ++r) {
		uint32_t start = server->snapshotStart[r];
		uint32_t size = server->snapshotSize[r];
		uint32_t i;
		for (i = 0; i + width <= size; i += width) {
			uint32_t oldValue = 0, newValue = 0;
			int k;
			for (k = width - 1; k >= 0; --k) {
				oldValue = (oldValue << 8) | server->snapshot[offset + i + k];
			}
			newValue = _readWidth(core, start + i, -1, width);
			bool hit = mode == 0 ? newValue != oldValue : (mode == 1 ? newValue > oldValue : newValue < oldValue);
			if (!hit) {
				continue;
			}
			if ((int) total < maxResults) {
				cJSON* item = cJSON_CreateObject();
				_addHex(item, "address", start + i);
				cJSON_AddNumberToObject(item, "old", oldValue);
				cJSON_AddNumberToObject(item, "new", newValue);
				cJSON_AddItemToArray(list, item);
			}
			++total;
		}
		offset += size;
	}
	cJSON_AddNumberToObject(json, "total", (double) total);
	cJSON_AddNumberToObject(json, "snapshotFrame", server->snapshotFrame);
	cJSON_AddNumberToObject(json, "currentFrame", core->frameCounter(core));
	if (_argBool(args, "update_snapshot", false)) {
		struct MCPToolResult scratch = { cJSON_CreateArray(), false };
		_toolSnapshotMemory(server, NULL, context, &scratch);
		cJSON_Delete(scratch.content);
		cJSON_AddBoolToObject(json, "snapshotUpdated", true);
	}
	_resultJson(result, json);
}

static void _toolFindFreeRam(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	uint32_t fill = 0;
	if (cJSON_GetObjectItemCaseSensitive(args, "fill") && !_argU32(args, "fill", &fill, false, result)) {
		return;
	}
	if (fill > 0xFF) {
		_resultError(result, "'fill' must be a byte value (0-255)");
		return;
	}
	uint32_t minSize = (uint32_t) _clampInt(_argInt(args, "min_size", 64), 1, 0x40000);
	int maxResults = _clampInt(_argInt(args, "max_results", 20), 1, 200);
	struct { uint32_t start, size; } runs[200];
	int nRuns = 0;
	const struct mCoreMemoryBlock* blocks;
	size_t nBlocks = core->listMemoryBlocks(core, &blocks);
	size_t b;
	for (b = 0; b < nBlocks; ++b) {
		size_t size;
		const uint8_t* mem;
		if (!_isRamBlock(&blocks[b]) || !(mem = _blockMemory(core, &blocks[b], &size))) {
			continue;
		}
		size_t i = 0;
		while (i < size) {
			if (mem[i] != fill) {
				++i;
				continue;
			}
			size_t j = i;
			while (j < size && mem[j] == fill) {
				++j;
			}
			uint32_t length = (uint32_t) (j - i);
			if (length >= minSize) {
				/* Keep the largest runs: insert sorted, drop the smallest when full. */
				int pos = nRuns < maxResults ? nRuns++ : maxResults - 1;
				if (pos == maxResults - 1 && nRuns == maxResults && runs[pos].size >= length) {
					i = j;
					continue;
				}
				while (pos > 0 && runs[pos - 1].size < length) {
					runs[pos] = runs[pos - 1];
					--pos;
				}
				runs[pos].start = blocks[b].start + (uint32_t) i;
				runs[pos].size = length;
			}
			i = j;
		}
	}
	cJSON* json = cJSON_CreateObject();
	cJSON* list = cJSON_AddArrayToObject(json, "regions");
	int k;
	for (k = 0; k < nRuns; ++k) {
		cJSON* item = cJSON_CreateObject();
		_addHex(item, "start", runs[k].start);
		cJSON_AddNumberToObject(item, "size", runs[k].size);
		cJSON_AddItemToArray(list, item);
	}
	cJSON_AddStringToObject(json, "note", "Unused-looking space can still be used by the game later (e.g. buffers cleared on boot). Verify with a write watchpoint before relying on it.");
	_resultJson(result, json);
}

static void _toolMemoryWatch(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	const char* action = _argString(args, "action");
	if (!action) {
		action = "list";
	}
	if (strcmp(action, "add") == 0) {
		uint32_t address;
		if (!_argU32(args, "address", &address, true, result)) {
			return;
		}
		int width = _argInt(args, "width", 1);
		if (width != 1 && width != 2 && width != 4) {
			_resultError(result, "'width' must be 1, 2 or 4");
			return;
		}
		if (server->nWatches >= MCP_MAX_WATCHES) {
			_resultError(result, "Too many watches (max %d)", MCP_MAX_WATCHES);
			return;
		}
		struct MCPMemoryWatch* watch = &server->watches[server->nWatches++];
		watch->address = address;
		watch->width = width;
		watch->lastValue = _readWidth(core, address, -1, width);
		const char* label = _argString(args, "label");
		snprintf(watch->label, sizeof(watch->label), "%s", label ? label : "");
	} else if (strcmp(action, "remove") == 0) {
		uint32_t address;
		if (!_argU32(args, "address", &address, true, result)) {
			return;
		}
		unsigned i;
		bool found = false;
		for (i = 0; i < server->nWatches; ++i) {
			if (server->watches[i].address == address) {
				memmove(&server->watches[i], &server->watches[i + 1], (server->nWatches - i - 1) * sizeof(server->watches[0]));
				--server->nWatches;
				found = true;
				break;
			}
		}
		if (!found) {
			_resultError(result, "No watch at 0x%08X", address);
			return;
		}
	} else if (strcmp(action, "clear") == 0) {
		server->nWatches = 0;
	} else if (strcmp(action, "list") != 0) {
		_resultError(result, "'action' must be add, remove, list or clear");
		return;
	}
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "frame", core->frameCounter(core));
	cJSON* list = cJSON_AddArrayToObject(json, "watches");
	unsigned i;
	for (i = 0; i < server->nWatches; ++i) {
		struct MCPMemoryWatch* watch = &server->watches[i];
		uint32_t value = _readWidth(core, watch->address, -1, watch->width);
		cJSON* item = cJSON_CreateObject();
		_addHex(item, "address", watch->address);
		if (watch->label[0]) {
			cJSON_AddStringToObject(item, "label", watch->label);
		}
		cJSON_AddNumberToObject(item, "width", watch->width);
		cJSON_AddNumberToObject(item, "value", value);
		cJSON_AddNumberToObject(item, "previous", watch->lastValue);
		cJSON_AddBoolToObject(item, "changed", value != watch->lastValue);
		watch->lastValue = value;
		cJSON_AddItemToArray(list, item);
	}
	_resultJson(result, json);
}

/* ---------------------------------------------------------------------------
 * GBA video: registers, palettes, tiles, backgrounds, sprites
 * ------------------------------------------------------------------------- */

#ifdef M_CORE_GBA
static struct GBA* _requireGBA(struct mCore* core, struct MCPToolResult* result) {
	struct GBA* gba = _gba(core);
	if (!gba) {
		_resultError(result, "This tool is only available for GBA games.");
	}
	return gba;
}

static void _addBgControl(cJSON* bg, uint16_t cnt) {
	cJSON_AddNumberToObject(bg, "priority", cnt & 3);
	_addHex(bg, "charBase", 0x06000000 + ((cnt >> 2) & 3) * 0x4000);
	cJSON_AddBoolToObject(bg, "mosaic", (cnt >> 6) & 1);
	cJSON_AddNumberToObject(bg, "bpp", (cnt >> 7) & 1 ? 8 : 4);
	_addHex(bg, "screenBase", 0x06000000 + ((cnt >> 8) & 0x1F) * 0x800);
	cJSON_AddBoolToObject(bg, "affineWrap", (cnt >> 13) & 1);
	cJSON_AddNumberToObject(bg, "sizeSetting", (cnt >> 14) & 3);
}

static void _toolGetVideoState(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct GBA* gba = _requireGBA(_core(server), result);
	if (!gba) {
		return;
	}
	uint16_t dispcnt = _io(gba, GBA_REG_DISPCNT);
	int mode = dispcnt & 7;
	cJSON* json = cJSON_CreateObject();
	_addHex(json, "DISPCNT", dispcnt);
	cJSON_AddNumberToObject(json, "mode", mode);
	cJSON_AddNumberToObject(json, "frameSelect", (dispcnt >> 4) & 1);
	cJSON_AddBoolToObject(json, "hblankIntervalFree", (dispcnt >> 5) & 1);
	cJSON_AddStringToObject(json, "objMapping", (dispcnt >> 6) & 1 ? "1D" : "2D");
	cJSON_AddBoolToObject(json, "forcedBlank", (dispcnt >> 7) & 1);
	cJSON* layers = cJSON_AddObjectToObject(json, "enabled");
	cJSON_AddBoolToObject(layers, "bg0", (dispcnt >> 8) & 1);
	cJSON_AddBoolToObject(layers, "bg1", (dispcnt >> 9) & 1);
	cJSON_AddBoolToObject(layers, "bg2", (dispcnt >> 10) & 1);
	cJSON_AddBoolToObject(layers, "bg3", (dispcnt >> 11) & 1);
	cJSON_AddBoolToObject(layers, "obj", (dispcnt >> 12) & 1);
	cJSON_AddBoolToObject(layers, "win0", (dispcnt >> 13) & 1);
	cJSON_AddBoolToObject(layers, "win1", (dispcnt >> 14) & 1);
	cJSON_AddBoolToObject(layers, "objWin", (dispcnt >> 15) & 1);
	_addHex(json, "DISPSTAT", _io(gba, GBA_REG_DISPSTAT));
	cJSON_AddNumberToObject(json, "vcount", gba->video.vcount);

	cJSON* bgs = cJSON_AddArrayToObject(json, "backgrounds");
	int i;
	for (i = 0; i < 4; ++i) {
		uint16_t cnt = _io(gba, GBA_REG_BG0CNT + i * 2);
		cJSON* bg = cJSON_CreateObject();
		cJSON_AddNumberToObject(bg, "layer", i);
		const char* kind;
		if (mode == 0 || (mode == 1 && i < 2)) {
			kind = "text";
		} else if ((mode == 1 && i == 2) || (mode == 2 && i >= 2)) {
			kind = "affine";
		} else if (mode >= 3 && mode <= 5 && i == 2) {
			kind = "bitmap";
		} else {
			kind = "unused";
		}
		cJSON_AddStringToObject(bg, "kind", kind);
		_addHex(bg, "BGCNT", cnt);
		_addBgControl(bg, cnt);
		if (strcmp(kind, "text") == 0) {
			static const char* const sizes[] = { "256x256", "512x256", "256x512", "512x512" };
			cJSON_AddStringToObject(bg, "size", sizes[(cnt >> 14) & 3]);
			cJSON_AddNumberToObject(bg, "scrollX", _io(gba, GBA_REG_BG0HOFS + i * 4) & 0x1FF);
			cJSON_AddNumberToObject(bg, "scrollY", _io(gba, GBA_REG_BG0VOFS + i * 4) & 0x1FF);
		} else if (strcmp(kind, "affine") == 0) {
			static const char* const sizes[] = { "128x128", "256x256", "512x512", "1024x1024" };
			cJSON_AddStringToObject(bg, "size", sizes[(cnt >> 14) & 3]);
			uint32_t base = i == 2 ? GBA_REG_BG2PA : GBA_REG_BG3PA;
			cJSON* m = cJSON_AddObjectToObject(bg, "matrix");
			cJSON_AddNumberToObject(m, "pa", (int16_t) _io(gba, base) / 256.0);
			cJSON_AddNumberToObject(m, "pb", (int16_t) _io(gba, base + 2) / 256.0);
			cJSON_AddNumberToObject(m, "pc", (int16_t) _io(gba, base + 4) / 256.0);
			cJSON_AddNumberToObject(m, "pd", (int16_t) _io(gba, base + 6) / 256.0);
			int32_t x = _io(gba, base + 8) | (_io(gba, base + 10) << 16);
			int32_t y = _io(gba, base + 12) | (_io(gba, base + 14) << 16);
			x = (x << 4) >> 4;
			y = (y << 4) >> 4;
			cJSON_AddNumberToObject(m, "refX", x / 256.0);
			cJSON_AddNumberToObject(m, "refY", y / 256.0);
		}
		cJSON_AddItemToArray(bgs, bg);
	}
	cJSON* win = cJSON_AddObjectToObject(json, "windows");
	uint16_t w0h = _io(gba, GBA_REG_WIN0H), w0v = _io(gba, GBA_REG_WIN0V), w1h = _io(gba, GBA_REG_WIN1H), w1v = _io(gba, GBA_REG_WIN1V);
	cJSON* w0 = cJSON_AddObjectToObject(win, "win0");
	cJSON_AddNumberToObject(w0, "left", w0h >> 8);
	cJSON_AddNumberToObject(w0, "right", w0h & 0xFF);
	cJSON_AddNumberToObject(w0, "top", w0v >> 8);
	cJSON_AddNumberToObject(w0, "bottom", w0v & 0xFF);
	cJSON* w1 = cJSON_AddObjectToObject(win, "win1");
	cJSON_AddNumberToObject(w1, "left", w1h >> 8);
	cJSON_AddNumberToObject(w1, "right", w1h & 0xFF);
	cJSON_AddNumberToObject(w1, "top", w1v >> 8);
	cJSON_AddNumberToObject(w1, "bottom", w1v & 0xFF);
	_addHex(win, "WININ", _io(gba, GBA_REG_WININ));
	_addHex(win, "WINOUT", _io(gba, GBA_REG_WINOUT));
	_addHex(json, "MOSAIC", _io(gba, GBA_REG_MOSAIC));
	uint16_t bldcnt = _io(gba, GBA_REG_BLDCNT);
	cJSON* blend = cJSON_AddObjectToObject(json, "blending");
	static const char* const blendModes[] = { "none", "alpha", "brighten", "darken" };
	cJSON_AddStringToObject(blend, "mode", blendModes[(bldcnt >> 6) & 3]);
	_addHex(blend, "BLDCNT", bldcnt);
	uint16_t alpha = _io(gba, GBA_REG_BLDALPHA);
	cJSON_AddNumberToObject(blend, "eva", alpha & 0x1F);
	cJSON_AddNumberToObject(blend, "evb", (alpha >> 8) & 0x1F);
	cJSON_AddNumberToObject(blend, "evy", _io(gba, GBA_REG_BLDY) & 0x1F);
	_resultJson(result, json);
}

static void _toolDumpPalette(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct GBA* gba = _requireGBA(_core(server), result);
	if (!gba) {
		return;
	}
	/* 16x32 swatches: rows 0-15 = background palettes, rows 16-31 = sprite palettes. */
	const int cell = 8;
	struct mImage* image = mImageCreate(16 * cell, 32 * cell, mCOLOR_ABGR8);
	int i;
	for (i = 0; i < 512; ++i) {
		uint32_t color = _color555(gba->video.palette[i], true);
		int x, y;
		for (y = 0; y < cell; ++y) {
			for (x = 0; x < cell; ++x) {
				mImageSetPixel(image, (i % 16) * cell + x, (i / 16) * cell + y, color);
			}
		}
	}
	_resultText(result, "Palette RAM: 16 colors per row. Rows 0-15 are background palettes 0-15, rows 16-31 are sprite palettes 0-15. Color 0 of each row is transparent in 4bpp mode.");
	if (_argBool(args, "include_values", false)) {
		cJSON* json = cJSON_CreateObject();
		cJSON* bg = cJSON_AddArrayToObject(json, "background");
		cJSON* obj = cJSON_AddArrayToObject(json, "sprite");
		for (i = 0; i < 512; ++i) {
			char hex[8];
			snprintf(hex, sizeof(hex), "%04X", gba->video.palette[i]);
			cJSON_AddItemToArray(i < 256 ? bg : obj, cJSON_CreateString(hex));
		}
		_resultJson(result, json);
	}
	_resultImage(result, image, args);
	mImageDestroy(image);
}

/* Draws one 8x8 tile from `tileData` (VRAM offset) at (px, py). */
static void _drawTile(struct GBA* gba, struct mImage* image, uint32_t tileOffset, bool bpp8, int palette, bool objPalette,
                      int px, int py, bool hflip, bool vflip) {
	const uint8_t* vram = (const uint8_t*) gba->video.vram;
	const uint16_t* pal = &gba->video.palette[objPalette ? 256 : 0];
	int x, y;
	for (y = 0; y < 8; ++y) {
		for (x = 0; x < 8; ++x) {
			int sx = hflip ? 7 - x : x;
			int sy = vflip ? 7 - y : y;
			unsigned index;
			if (bpp8) {
				uint32_t off = tileOffset + sy * 8 + sx;
				index = off < GBA_SIZE_VRAM ? vram[off] : 0;
			} else {
				uint32_t off = tileOffset + sy * 4 + sx / 2;
				uint8_t byte = off < GBA_SIZE_VRAM ? vram[off] : 0;
				index = (sx & 1) ? byte >> 4 : byte & 0xF;
				if (index) {
					index += palette * 16;
				}
			}
			uint32_t color = index ? _color555(pal[index & 0xFF], true) : 0;
			mImageSetPixel(image, px + x, py + y, color);
		}
	}
}

static void _toolDumpTiles(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct GBA* gba = _requireGBA(_core(server), result);
	if (!gba) {
		return;
	}
	int block = _argInt(args, "char_block", 0);
	if (block < 0 || block > 5) {
		_resultError(result, "'char_block' must be 0-5 (0-3 = background tiles at 0x06000000 + n*0x4000, 4-5 = sprite tiles at 0x06010000)");
		return;
	}
	bool bpp8 = _argInt(args, "bpp", 4) == 8;
	int palette = _clampInt(_argInt(args, "palette", 0), 0, 15);
	const char* paletteType = _argString(args, "palette_type");
	bool objPalette = paletteType ? strcmp(paletteType, "sprite") == 0 : block >= 4;
	int tileBytes = bpp8 ? 64 : 32;
	int tiles = 0x4000 / tileBytes;
	int perRow = 32;
	int rows = tiles / perRow;
	struct mImage* image = mImageCreate(perRow * 8, rows * 8, mCOLOR_ABGR8);
	int t;
	for (t = 0; t < tiles; ++t) {
		_drawTile(gba, image, block * 0x4000 + t * tileBytes, bpp8, palette, objPalette, (t % perRow) * 8, (t / perRow) * 8, false, false);
	}
	_resultPrintf(result, "Char block %d (VRAM 0x%08X), %d tiles of %dbpp, 32 per row, %s palette %d. Transparent pixels are color 0.",
	              block, 0x06000000 + block * 0x4000, tiles, bpp8 ? 8 : 4, objPalette ? "sprite" : "background", palette);
	_resultImage(result, image, args);
	mImageDestroy(image);
}

static void _toolDumpBackground(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct GBA* gba = _requireGBA(_core(server), result);
	if (!gba) {
		return;
	}
	int layer = _argInt(args, "layer", 0);
	if (layer < 0 || layer > 3) {
		_resultError(result, "'layer' must be 0-3");
		return;
	}
	uint16_t dispcnt = _io(gba, GBA_REG_DISPCNT);
	int mode = dispcnt & 7;
	uint16_t cnt = _io(gba, GBA_REG_BG0CNT + layer * 2);
	uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
	uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
	bool bpp8 = (cnt >> 7) & 1;
	int sizeSetting = (cnt >> 14) & 3;
	const uint8_t* vram = (const uint8_t*) gba->video.vram;
	struct mImage* image = NULL;

	if (mode == 0 || (mode == 1 && layer < 2)) {
		static const int widths[] = { 256, 512, 256, 512 };
		static const int heights[] = { 256, 256, 512, 512 };
		int width = widths[sizeSetting], height = heights[sizeSetting];
		image = mImageCreate(width, height, mCOLOR_ABGR8);
		int tx, ty;
		for (ty = 0; ty < height / 8; ++ty) {
			for (tx = 0; tx < width / 8; ++tx) {
				/* 32x32-entry screen blocks: left/right, then top/bottom. */
				int sbIndex = (tx / 32) + (ty / 32) * (width / 256);
				uint32_t entryOff = screenBase + sbIndex * 0x800 + ((ty % 32) * 32 + (tx % 32)) * 2;
				if (entryOff + 1 >= GBA_SIZE_VRAM) {
					continue;
				}
				uint16_t entry = vram[entryOff] | (vram[entryOff + 1] << 8);
				unsigned tile = entry & 0x3FF;
				_drawTile(gba, image, charBase + tile * (bpp8 ? 64 : 32), bpp8, entry >> 12, false, tx * 8, ty * 8, (entry >> 10) & 1, (entry >> 11) & 1);
			}
		}
		_resultPrintf(result, "BG%d text layer, %dx%d, %dbpp, char base 0x%08X, screen base 0x%08X, scroll (%d, %d). The visible 240x160 window starts at the scroll position and wraps.",
		              layer, width, height, bpp8 ? 8 : 4, 0x06000000 + charBase, 0x06000000 + screenBase,
		              _io(gba, GBA_REG_BG0HOFS + layer * 4) & 0x1FF, _io(gba, GBA_REG_BG0VOFS + layer * 4) & 0x1FF);
	} else if ((mode == 1 && layer == 2) || (mode == 2 && layer >= 2)) {
		int size = 128 << sizeSetting;
		image = mImageCreate(size, size, mCOLOR_ABGR8);
		int tiles = size / 8;
		int tx, ty;
		for (ty = 0; ty < tiles; ++ty) {
			for (tx = 0; tx < tiles; ++tx) {
				uint32_t entryOff = screenBase + ty * tiles + tx;
				unsigned tile = entryOff < GBA_SIZE_VRAM ? vram[entryOff] : 0;
				_drawTile(gba, image, charBase + tile * 64, true, 0, false, tx * 8, ty * 8, false, false);
			}
		}
		_resultPrintf(result, "BG%d affine layer, %dx%d (shown untransformed; see get_video_state for the matrix).", layer, size, size);
	} else if (mode >= 3 && mode <= 5 && layer == 2) {
		int width = mode == 5 ? 160 : 240;
		int height = mode == 5 ? 128 : 160;
		uint32_t frame = (mode != 3 && ((dispcnt >> 4) & 1)) ? 0xA000 : 0;
		if (_argInt(args, "frame", -1) == 0 || _argInt(args, "frame", -1) == 1) {
			frame = mode != 3 && _argInt(args, "frame", 0) ? 0xA000 : 0;
		}
		image = mImageCreate(width, height, mCOLOR_ABGR8);
		int x, y;
		for (y = 0; y < height; ++y) {
			for (x = 0; x < width; ++x) {
				uint32_t color;
				if (mode == 4) {
					uint8_t index = vram[frame + y * width + x];
					color = _color555(gba->video.palette[index], true);
				} else {
					uint32_t off = frame + (y * width + x) * 2;
					color = _color555(vram[off] | (vram[off + 1] << 8), true);
				}
				mImageSetPixel(image, x, y, color);
			}
		}
		_resultPrintf(result, "BG2 bitmap (mode %d), %dx%d, frame buffer at 0x%08X.", mode, width, height, 0x06000000 + frame);
	} else {
		_resultError(result, "BG%d is not used in video mode %d.", layer, mode);
		return;
	}
	if (!((dispcnt >> (8 + layer)) & 1)) {
		_resultText(result, "Note: this layer is currently disabled in DISPCNT.");
	}
	_resultImage(result, image, args);
	mImageDestroy(image);
}

static const int _objSizes[3][4][2] = {
	{ { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
	{ { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
	{ { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

struct MCPSprite {
	int index;
	int x, y, width, height;
	unsigned tile;
	int palette, priority;
	bool bpp8, affine, doubleSize, hidden, hflip, vflip, mosaic;
	int affineIndex;
	int mode;
};

static bool _decodeSprite(struct GBA* gba, int index, struct MCPSprite* sprite) {
	uint16_t a0 = gba->video.oam.raw[index * 4];
	uint16_t a1 = gba->video.oam.raw[index * 4 + 1];
	uint16_t a2 = gba->video.oam.raw[index * 4 + 2];
	int shape = (a0 >> 14) & 3;
	if (shape == 3) {
		return false;
	}
	int size = (a1 >> 14) & 3;
	memset(sprite, 0, sizeof(*sprite));
	sprite->index = index;
	sprite->affine = (a0 >> 8) & 1;
	sprite->doubleSize = sprite->affine && ((a0 >> 9) & 1);
	sprite->hidden = !sprite->affine && ((a0 >> 9) & 1);
	sprite->mode = (a0 >> 10) & 3;
	sprite->mosaic = (a0 >> 12) & 1;
	sprite->bpp8 = (a0 >> 13) & 1;
	sprite->y = a0 & 0xFF;
	if (sprite->y >= 160) {
		sprite->y -= 256;
	}
	sprite->x = a1 & 0x1FF;
	if (sprite->x >= 256) {
		sprite->x -= 512;
	}
	if (sprite->affine) {
		sprite->affineIndex = (a1 >> 9) & 0x1F;
	} else {
		sprite->hflip = (a1 >> 12) & 1;
		sprite->vflip = (a1 >> 13) & 1;
	}
	sprite->width = _objSizes[shape][size][0];
	sprite->height = _objSizes[shape][size][1];
	sprite->tile = a2 & 0x3FF;
	sprite->priority = (a2 >> 10) & 3;
	sprite->palette = (a2 >> 12) & 0xF;
	return true;
}

static void _toolListSprites(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct GBA* gba = _requireGBA(_core(server), result);
	if (!gba) {
		return;
	}
	bool includeHidden = _argBool(args, "include_hidden", false);
	bool onlyVisible = _argBool(args, "on_screen_only", false);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "objMapping", (_io(gba, GBA_REG_DISPCNT) >> 6) & 1 ? "1D" : "2D");
	cJSON_AddBoolToObject(json, "spritesEnabled", (_io(gba, GBA_REG_DISPCNT) >> 12) & 1);
	cJSON* list = cJSON_AddArrayToObject(json, "sprites");
	static const char* const modes[] = { "normal", "semi_transparent", "obj_window", "invalid" };
	int i, count = 0;
	for (i = 0; i < 128; ++i) {
		struct MCPSprite s;
		if (!_decodeSprite(gba, i, &s) || (s.hidden && !includeHidden)) {
			continue;
		}
		int boxW = s.doubleSize ? s.width * 2 : s.width;
		int boxH = s.doubleSize ? s.height * 2 : s.height;
		bool onScreen = s.x + boxW > 0 && s.x < 240 && s.y + boxH > 0 && s.y < 160;
		if (onlyVisible && (!onScreen || s.hidden)) {
			continue;
		}
		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "index", i);
		cJSON_AddNumberToObject(item, "x", s.x);
		cJSON_AddNumberToObject(item, "y", s.y);
		cJSON_AddNumberToObject(item, "width", s.width);
		cJSON_AddNumberToObject(item, "height", s.height);
		cJSON_AddNumberToObject(item, "tile", s.tile);
		cJSON_AddNumberToObject(item, "bpp", s.bpp8 ? 8 : 4);
		if (!s.bpp8) {
			cJSON_AddNumberToObject(item, "palette", s.palette);
		}
		cJSON_AddNumberToObject(item, "priority", s.priority);
		cJSON_AddStringToObject(item, "mode", modes[s.mode]);
		if (s.affine) {
			cJSON_AddNumberToObject(item, "affineIndex", s.affineIndex);
			cJSON_AddBoolToObject(item, "doubleSize", s.doubleSize);
		} else if (s.hflip || s.vflip) {
			cJSON_AddBoolToObject(item, "hflip", s.hflip);
			cJSON_AddBoolToObject(item, "vflip", s.vflip);
		}
		if (s.mosaic) {
			cJSON_AddBoolToObject(item, "mosaic", true);
		}
		if (s.hidden) {
			cJSON_AddBoolToObject(item, "hidden", true);
		}
		cJSON_AddBoolToObject(item, "onScreen", onScreen);
		cJSON_AddItemToArray(list, item);
		++count;
	}
	cJSON_AddNumberToObject(json, "count", count);
	_resultJson(result, json);
}

static void _toolDumpSprite(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct GBA* gba = _requireGBA(_core(server), result);
	if (!gba) {
		return;
	}
	int index = _argInt(args, "index", -1);
	if (index < 0 || index > 127) {
		_resultError(result, "'index' must be 0-127 (see list_sprites)");
		return;
	}
	struct MCPSprite s;
	if (!_decodeSprite(gba, index, &s)) {
		_resultError(result, "Sprite %d has an invalid shape", index);
		return;
	}
	int scale = _clampInt(_argInt(args, "scale", 1), 1, 8);
	bool map1D = (_io(gba, GBA_REG_DISPCNT) >> 6) & 1;
	struct mImage* sprite = mImageCreate(s.width, s.height, mCOLOR_ABGR8);
	int tilesW = s.width / 8, tilesH = s.height / 8;
	int tx, ty;
	for (ty = 0; ty < tilesH; ++ty) {
		for (tx = 0; tx < tilesW; ++tx) {
			unsigned tile;
			if (map1D) {
				tile = s.tile + (ty * tilesW + tx) * (s.bpp8 ? 2 : 1);
			} else {
				tile = s.tile + ty * 32 + tx * (s.bpp8 ? 2 : 1);
			}
			_drawTile(gba, sprite, 0x10000 + (tile & 0x3FF) * 32, s.bpp8, s.palette, true, tx * 8, ty * 8, false, false);
		}
	}
	struct mImage* out = sprite;
	if (scale > 1 || s.hflip || s.vflip) {
		out = mImageCreate(s.width * scale, s.height * scale, mCOLOR_ABGR8);
		int x, y;
		for (y = 0; y < s.height * scale; ++y) {
			for (x = 0; x < s.width * scale; ++x) {
				int sx = x / scale, sy = y / scale;
				if (s.hflip) {
					sx = s.width - 1 - sx;
				}
				if (s.vflip) {
					sy = s.height - 1 - sy;
				}
				mImageSetPixel(out, x, y, mImageGetPixel(sprite, sx, sy));
			}
		}
	}
	_resultPrintf(result, "Sprite %d: %dx%d at (%d, %d), tile %u, %dbpp%s. Drawn unrotated%s.", index, s.width, s.height, s.x, s.y,
	              s.tile, s.bpp8 ? 8 : 4, s.hidden ? ", currently hidden" : "", s.affine ? " (it uses an affine transform in game)" : "");
	_resultImage(result, out, args);
	if (out != sprite) {
		mImageDestroy(out);
	}
	mImageDestroy(sprite);
}

/* ---------------------------------------------------------------------------
 * GBA audio and hardware state
 * ------------------------------------------------------------------------- */

static void _toolGetAudioState(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct GBA* gba = _requireGBA(_core(server), result);
	if (!gba) {
		return;
	}
	uint16_t cntL = _io(gba, GBA_REG_SOUNDCNT_LO), cntH = _io(gba, GBA_REG_SOUNDCNT_HI), cntX = _io(gba, GBA_REG_SOUNDCNT_X);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddBoolToObject(json, "masterEnable", (cntX >> 7) & 1);
	cJSON* active = cJSON_AddObjectToObject(json, "psgPlaying");
	cJSON_AddBoolToObject(active, "square1", cntX & 1);
	cJSON_AddBoolToObject(active, "square2", (cntX >> 1) & 1);
	cJSON_AddBoolToObject(active, "wave", (cntX >> 2) & 1);
	cJSON_AddBoolToObject(active, "noise", (cntX >> 3) & 1);
	cJSON* psg = cJSON_AddObjectToObject(json, "psgMix");
	cJSON_AddNumberToObject(psg, "volumeRight", cntL & 7);
	cJSON_AddNumberToObject(psg, "volumeLeft", (cntL >> 4) & 7);
	_addHex(psg, "enableRightMask", (cntL >> 8) & 0xF);
	_addHex(psg, "enableLeftMask", (cntL >> 12) & 0xF);
	static const char* const psgRatio[] = { "25%", "50%", "100%", "invalid" };
	cJSON_AddStringToObject(psg, "ratio", psgRatio[cntH & 3]);
	int ch;
	cJSON* direct = cJSON_AddArrayToObject(json, "directSound");
	for (ch = 0; ch < 2; ++ch) {
		int shift = ch ? 12 : 8;
		cJSON* d = cJSON_CreateObject();
		cJSON_AddStringToObject(d, "channel", ch ? "B" : "A");
		cJSON_AddStringToObject(d, "volume", (cntH >> (2 + ch)) & 1 ? "100%" : "50%");
		cJSON_AddBoolToObject(d, "right", (cntH >> shift) & 1);
		cJSON_AddBoolToObject(d, "left", (cntH >> (shift + 1)) & 1);
		cJSON_AddNumberToObject(d, "timer", (cntH >> (shift + 2)) & 1);
		cJSON_AddItemToArray(direct, d);
	}
	cJSON* regs = cJSON_AddObjectToObject(json, "channels");
	cJSON* s1 = cJSON_AddObjectToObject(regs, "square1");
	uint16_t s1l = _io(gba, GBA_REG_SOUND1CNT_LO), s1h = _io(gba, GBA_REG_SOUND1CNT_HI), s1x = _io(gba, GBA_REG_SOUND1CNT_X);
	cJSON_AddNumberToObject(s1, "sweepShift", s1l & 7);
	cJSON_AddBoolToObject(s1, "sweepDecrease", (s1l >> 3) & 1);
	cJSON_AddNumberToObject(s1, "sweepTime", (s1l >> 4) & 7);
	cJSON_AddNumberToObject(s1, "duty", (s1h >> 6) & 3);
	cJSON_AddNumberToObject(s1, "envelopeInitial", (s1h >> 12) & 0xF);
	cJSON_AddNumberToObject(s1, "frequencyRaw", s1x & 0x7FF);
	if ((s1x & 0x7FF) < 2048) {
		cJSON_AddNumberToObject(s1, "frequencyHz", 131072.0 / (2048 - (s1x & 0x7FF)));
	}
	cJSON* s2 = cJSON_AddObjectToObject(regs, "square2");
	uint16_t s2l = _io(gba, GBA_REG_SOUND2CNT_LO), s2h = _io(gba, GBA_REG_SOUND2CNT_HI);
	cJSON_AddNumberToObject(s2, "duty", (s2l >> 6) & 3);
	cJSON_AddNumberToObject(s2, "envelopeInitial", (s2l >> 12) & 0xF);
	cJSON_AddNumberToObject(s2, "frequencyRaw", s2h & 0x7FF);
	cJSON_AddNumberToObject(s2, "frequencyHz", 131072.0 / (2048 - (s2h & 0x7FF)));
	cJSON* w = cJSON_AddObjectToObject(regs, "wave");
	uint16_t wl = _io(gba, GBA_REG_SOUND3CNT_LO), wh = _io(gba, GBA_REG_SOUND3CNT_HI), wx = _io(gba, GBA_REG_SOUND3CNT_X);
	cJSON_AddBoolToObject(w, "enabled", (wl >> 7) & 1);
	cJSON_AddStringToObject(w, "bankMode", (wl >> 5) & 1 ? "2x32" : "1x32");
	cJSON_AddNumberToObject(w, "volumeCode", (wh >> 13) & 7);
	cJSON_AddNumberToObject(w, "sampleRateRaw", wx & 0x7FF);
	cJSON* n = cJSON_AddObjectToObject(regs, "noise");
	uint16_t nl = _io(gba, GBA_REG_SOUND4CNT_LO), nh = _io(gba, GBA_REG_SOUND4CNT_HI);
	cJSON_AddNumberToObject(n, "envelopeInitial", (nl >> 12) & 0xF);
	cJSON_AddNumberToObject(n, "divider", nh & 7);
	cJSON_AddBoolToObject(n, "width7bit", (nh >> 3) & 1);
	cJSON_AddNumberToObject(n, "shift", (nh >> 4) & 0xF);
	_addHex(json, "SOUNDBIAS", _io(gba, GBA_REG_SOUNDBIAS));
	_resultJson(result, json);
}

static const char* const _irqNames[] = {
	"vblank", "hblank", "vcounter", "timer0", "timer1", "timer2", "timer3", "serial",
	"dma0", "dma1", "dma2", "dma3", "keypad", "gamepak"
};

static cJSON* _irqBits(uint16_t value) {
	cJSON* list = cJSON_CreateArray();
	int i;
	for (i = 0; i < 14; ++i) {
		if (value & (1 << i)) {
			cJSON_AddItemToArray(list, cJSON_CreateString(_irqNames[i]));
		}
	}
	return list;
}

static void _toolGetHardwareState(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct GBA* gba = _requireGBA(_core(server), result);
	if (!gba) {
		return;
	}
	const char* subsystem = _argString(args, "subsystem");
	if (!subsystem) {
		subsystem = "all";
	}
	bool all = strcmp(subsystem, "all") == 0;
	bool any = false;
	cJSON* json = cJSON_CreateObject();
	if (all || strcmp(subsystem, "dma") == 0) {
		any = true;
		static const char* const timing[] = { "immediate", "vblank", "hblank", "special" };
		static const char* const control[] = { "increment", "decrement", "fixed", "increment_reload" };
		cJSON* list = cJSON_AddArrayToObject(json, "dma");
		int i;
		for (i = 0; i < 4; ++i) {
			struct GBADMA* dma = &gba->memory.dma[i];
			uint16_t reg = dma->reg;
			cJSON* d = cJSON_CreateObject();
			cJSON_AddNumberToObject(d, "channel", i);
			cJSON_AddBoolToObject(d, "enabled", (reg >> 15) & 1);
			_addHex(d, "source", dma->source);
			_addHex(d, "dest", dma->dest);
			cJSON_AddNumberToObject(d, "count", dma->count);
			cJSON_AddStringToObject(d, "timing", timing[(reg >> 12) & 3]);
			cJSON_AddStringToObject(d, "destControl", control[(reg >> 5) & 3]);
			cJSON_AddStringToObject(d, "sourceControl", control[(reg >> 7) & 3]);
			cJSON_AddBoolToObject(d, "repeat", (reg >> 9) & 1);
			cJSON_AddNumberToObject(d, "unitBytes", (reg >> 10) & 1 ? 4 : 2);
			cJSON_AddBoolToObject(d, "irq", (reg >> 14) & 1);
			cJSON_AddItemToArray(list, d);
		}
	}
	if (all || strcmp(subsystem, "timers") == 0) {
		any = true;
		static const int prescale[] = { 1, 64, 256, 1024 };
		cJSON* list = cJSON_AddArrayToObject(json, "timers");
		int i;
		for (i = 0; i < 4; ++i) {
			GBATimerUpdateRegister(gba, i, 0);
			uint16_t control = _io(gba, GBA_REG_TM0CNT_HI + i * 4);
			cJSON* t = cJSON_CreateObject();
			cJSON_AddNumberToObject(t, "timer", i);
			cJSON_AddBoolToObject(t, "enabled", (control >> 7) & 1);
			cJSON_AddNumberToObject(t, "counter", _io(gba, GBA_REG_TM0CNT_LO + i * 4));
			cJSON_AddNumberToObject(t, "reload", gba->timers[i].reload);
			cJSON_AddNumberToObject(t, "prescaler", prescale[control & 3]);
			cJSON_AddBoolToObject(t, "countUp", (control >> 2) & 1);
			cJSON_AddBoolToObject(t, "irq", (control >> 6) & 1);
			cJSON_AddItemToArray(list, t);
		}
	}
	if (all || strcmp(subsystem, "interrupts") == 0) {
		any = true;
		cJSON* irq = cJSON_AddObjectToObject(json, "interrupts");
		uint16_t ie = _io(gba, GBA_REG_IE), iflag = _io(gba, GBA_REG_IF);
		cJSON_AddBoolToObject(irq, "masterEnable", _io(gba, GBA_REG_IME) & 1);
		cJSON_AddItemToObject(irq, "enabled", _irqBits(ie));
		cJSON_AddItemToObject(irq, "requested", _irqBits(iflag));
		cJSON_AddItemToObject(irq, "pending", _irqBits(ie & iflag));
		cJSON_AddBoolToObject(irq, "cpuHalted", ((struct ARMCore*) gba->cpu)->halted != 0);
	}
	if (all || strcmp(subsystem, "keypad") == 0) {
		any = true;
		cJSON* pad = cJSON_AddObjectToObject(json, "keypad");
		uint16_t keys = _io(gba, GBA_REG_KEYINPUT);
		cJSON_AddItemToObject(pad, "pressedInRegister", _keysToJson(~keys & 0x3FF));
		uint16_t keycnt = _io(gba, GBA_REG_KEYCNT);
		cJSON_AddBoolToObject(pad, "irqEnabled", (keycnt >> 14) & 1);
		cJSON_AddStringToObject(pad, "irqCondition", (keycnt >> 15) & 1 ? "all" : "any");
		cJSON_AddItemToObject(pad, "irqKeys", _keysToJson(keycnt & 0x3FF));
	}
	if (all || strcmp(subsystem, "system") == 0) {
		any = true;
		cJSON* sys = cJSON_AddObjectToObject(json, "system");
		uint16_t waitcnt = _io(gba, GBA_REG_WAITCNT);
		static const int sramWait[] = { 4, 3, 2, 8 };
		static const int ws0n[] = { 4, 3, 2, 8 };
		cJSON_AddNumberToObject(sys, "sramWait", sramWait[waitcnt & 3]);
		cJSON_AddNumberToObject(sys, "ws0First", ws0n[(waitcnt >> 2) & 3]);
		cJSON_AddNumberToObject(sys, "ws0Second", (waitcnt >> 4) & 1 ? 1 : 2);
		cJSON_AddBoolToObject(sys, "prefetch", (waitcnt >> 14) & 1);
		_addHex(sys, "WAITCNT", waitcnt);
		cJSON_AddBoolToObject(sys, "realBios", gba->memory.fullBios != 0);
	}
	if (!any) {
		_resultError(result, "'subsystem' must be dma, timers, interrupts, keypad, system or all");
		cJSON_Delete(json);
		return;
	}
	_resultJson(result, json);
}
#endif

/* ---------------------------------------------------------------------------
 * Input: sequences and controller state
 * ------------------------------------------------------------------------- */

static void _sequenceStart(struct MCPServer* server) {
	struct mCore* core = _core(server);
	server->sequenceIndex = 0;
	server->sequenceFramesLeft = 0;
	if (server->sequenceLength) {
		core->addKeys(core, server->sequence[0].keys);
		server->sequenceFramesLeft = server->sequence[0].frames;
		server->sequenceIndex = 1;
	}
}

/* Called at the end of each frame while a queued sequence plays. */
static void _sequenceTick(struct MCPServer* server) {
	if (!server->sequenceLength) {
		return;
	}
	struct mCore* core = _core(server);
	if (--server->sequenceFramesLeft > 0) {
		return;
	}
	core->clearKeys(core, server->sequence[server->sequenceIndex - 1].keys);
	if (server->sequenceIndex >= server->sequenceLength) {
		server->sequenceLength = 0;
		return;
	}
	core->addKeys(core, server->sequence[server->sequenceIndex].keys);
	server->sequenceFramesLeft = server->sequence[server->sequenceIndex].frames;
	++server->sequenceIndex;
}

static void _toolInputSequence(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	struct mCore* core = _core(server);
	const cJSON* steps = cJSON_GetObjectItemCaseSensitive(args, "steps");
	if (!cJSON_IsArray(steps) || !cJSON_GetArraySize(steps)) {
		_resultError(result, "'steps' must be a non-empty array of {buttons: [...], frames: N}");
		return;
	}
	if (cJSON_GetArraySize(steps) > MCP_MAX_SEQUENCE) {
		_resultError(result, "At most %d steps", MCP_MAX_SEQUENCE);
		return;
	}
	struct MCPInputStep parsed[MCP_MAX_SEQUENCE];
	unsigned n = 0;
	int totalFrames = 0;
	const cJSON* step;
	cJSON_ArrayForEach(step, steps) {
		if (!_parseButtons(step, "buttons", &parsed[n].keys, result)) {
			return;
		}
		parsed[n].frames = _argInt(step, "frames", 1);
		if (parsed[n].frames < 1 || parsed[n].frames > MCP_MAX_FRAMES) {
			_resultError(result, "Each step's 'frames' must be between 1 and %d", MCP_MAX_FRAMES);
			return;
		}
		totalFrames += parsed[n].frames;
		++n;
	}
	if (context != MCP_CONTEXT_PAUSED) {
		/* Running: play it in the background from the frame hook. */
		if (server->sequenceLength && server->sequenceIndex) {
			core->clearKeys(core, server->sequence[server->sequenceIndex - 1].keys);
		}
		memcpy(server->sequence, parsed, n * sizeof(parsed[0]));
		server->sequenceLength = n;
		_sequenceStart(server);
		_resultPrintf(result, "Playing %u steps (%d frames) while the game runs.", n, totalFrames);
		return;
	}
	if (totalFrames > MCP_MAX_FRAMES * 4) {
		_resultError(result, "While paused, a sequence can be at most %d frames in total", MCP_MAX_FRAMES * 4);
		return;
	}
	int done = 0;
	unsigned i;
	server->stopHit = false;
	for (i = 0; i < n && !server->stopHit; ++i) {
		core->addKeys(core, parsed[i].keys);
		done += _runFrames(server, parsed[i].frames);
		core->clearKeys(core, parsed[i].keys);
	}
	_finishRun(server, totalFrames, done, _argBool(args, "screenshot", false), result);
}

static void _toolGetControllerState(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mCore* core = _core(server);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddItemToObject(json, "heldByEmulator", _keysToJson(core->getKeys(core)));
#ifdef M_CORE_GBA
	struct GBA* gba = _gba(core);
	if (gba) {
		cJSON_AddItemToObject(json, "seenByGame", _keysToJson(~_io(gba, GBA_REG_KEYINPUT) & 0x3FF));
	}
#endif
	if (server->timedKeysFrames > 0) {
		cJSON* timed = cJSON_AddObjectToObject(json, "timedHold");
		cJSON_AddItemToObject(timed, "buttons", _keysToJson(server->timedKeys));
		cJSON_AddNumberToObject(timed, "framesLeft", server->timedKeysFrames);
	}
	if (server->sequenceLength) {
		cJSON* seq = cJSON_AddObjectToObject(json, "sequence");
		cJSON_AddNumberToObject(seq, "step", server->sequenceIndex);
		cJSON_AddNumberToObject(seq, "steps", server->sequenceLength);
		cJSON_AddNumberToObject(seq, "framesLeftInStep", server->sequenceFramesLeft);
	}
	cJSON_AddStringToObject(json, "note", "One controller. heldByEmulator includes buttons held by MCP; in the GUI, keyboard/gamepad input is applied by the frontend.");
	_resultJson(result, json);
}

/* ---------------------------------------------------------------------------
 * System: speed, settings, save states, ROM, save data, game control
 * ------------------------------------------------------------------------- */

static void _toolSetSpeed(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	struct mCoreThread* thread = mCoreThreadGet();
	if (!thread || !thread->impl) {
		_resultText(result, "This frontend has no speed limiter (the headless build always runs as fast as possible).");
		return;
	}
	float base = (float) core->frequency(core) / (float) core->frameCycles(core);
	bool fastForward = _argBool(args, "fast_forward", false);
	const cJSON* speedItem = cJSON_GetObjectItemCaseSensitive(args, "speed");
	double speed = cJSON_IsNumber(speedItem) ? speedItem->valuedouble : 1.0;
	if (speed < 0.05 || speed > 20) {
		_resultError(result, "'speed' must be between 0.05 and 20");
		return;
	}
	struct mCoreSync* sync = &thread->impl->sync;
	cJSON* json = cJSON_CreateObject();
	if (fastForward) {
		sync->audioWait = false;
		sync->videoFrameWait = false;
		cJSON_AddStringToObject(json, "speed", "unlimited");
	} else {
		bool audioSync = true, videoSync = false;
		mCoreConfigGetBoolValue(&core->config, "audioSync", &audioSync);
		mCoreConfigGetBoolValue(&core->config, "videoSync", &videoSync);
		sync->fpsTarget = base * (float) speed;
		sync->audioWait = audioSync || speed != 1.0;
		sync->videoFrameWait = videoSync && speed == 1.0;
		cJSON_AddNumberToObject(json, "speed", speed);
		cJSON_AddNumberToObject(json, "targetFps", sync->fpsTarget);
	}
	cJSON_AddStringToObject(json, "note", "The GUI's own fast-forward controls may override this.");
	_resultJson(result, json);
}

static const char* const _settingKeys[] = {
	"frameskip", "volume", "mute", "fpsTarget", "audioSync", "videoSync", "skipBios", "useBios", "gba.bios",
	"idleOptimization", "interframeBlending", "allowOpposingDirections", "rewindEnable", "rewindBufferCapacity",
	"fastForwardRatio", "savegamePath", "savestatePath", "screenshotPath", "cheatsPath", "patchPath",
	"gba.forceGbp", "gba.audioHle", "hwaccelVideo", "videoScale", NULL
};

static void _toolGetSettings(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	cJSON* json = cJSON_CreateObject();
	const char* key = _argString(args, "key");
	if (key) {
		const char* value = mCoreConfigGetValue(&core->config, key);
		cJSON_AddStringToObject(json, "key", key);
		if (value) {
			cJSON_AddStringToObject(json, "value", value);
		} else {
			cJSON_AddNullToObject(json, "value");
		}
	} else {
		cJSON* settings = cJSON_AddObjectToObject(json, "settings");
		size_t i;
		for (i = 0; _settingKeys[i]; ++i) {
			const char* value = mCoreConfigGetValue(&core->config, _settingKeys[i]);
			if (value) {
				cJSON_AddStringToObject(settings, _settingKeys[i], value);
			}
		}
		cJSON_AddStringToObject(json, "note", "Only keys that have a value are listed; pass 'key' to read any other mGBA config key.");
	}
	_resultJson(result, json);
}

static void _toolSetSetting(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	const char* key = _argString(args, "key");
	const cJSON* valueItem = cJSON_GetObjectItemCaseSensitive(args, "value");
	if (!key || !valueItem) {
		_resultError(result, "Pass 'key' and 'value'");
		return;
	}
	char value[256];
	if (cJSON_IsString(valueItem)) {
		snprintf(value, sizeof(value), "%s", valueItem->valuestring);
	} else if (cJSON_IsBool(valueItem)) {
		snprintf(value, sizeof(value), "%d", cJSON_IsTrue(valueItem) ? 1 : 0);
	} else if (cJSON_IsNumber(valueItem)) {
		if (valueItem->valuedouble == (double) (long long) valueItem->valuedouble) {
			snprintf(value, sizeof(value), "%lld", (long long) valueItem->valuedouble);
		} else {
			snprintf(value, sizeof(value), "%g", valueItem->valuedouble);
		}
	} else {
		_resultError(result, "'value' must be a string, number or boolean");
		return;
	}
	mCoreConfigSetOverrideValue(&core->config, key, value);
	mCoreConfigMap(&core->config, &core->opts);
	core->reloadConfigOption(core, key, NULL);
	core->reloadConfigOption(core, NULL, NULL);
	_resultPrintf(result, "%s = %s (applied for this session; the GUI settings window may change it back).", key, value);
}

#if defined(ENABLE_VFS) && defined(ENABLE_DIRECTORIES)
static struct VFile* _openStateSlot(struct mCore* core, int slot, int mode) {
	if (!core->dirs.state) {
		return NULL;
	}
	char name[PATH_MAX + 16];
	snprintf(name, sizeof(name), "%s.ss%i", core->dirs.baseName, slot);
	return core->dirs.state->openFile(core->dirs.state, name, mode);
}
#endif

static void _addStateMeta(struct mCore* core, struct VFile* vf, cJSON* item) {
	cJSON_AddNumberToObject(item, "size", (double) vf->size(vf));
	struct mStateExtdata extdata;
	mStateExtdataInit(&extdata);
	vf->seek(vf, 0, SEEK_SET);
	if (mCoreExtractExtdata(core, vf, &extdata)) {
		struct mStateExtdataItem meta;
		if (mStateExtdataGet(&extdata, EXTDATA_META_TIME, &meta) && meta.size >= 8) {
			uint64_t usec;
			memcpy(&usec, meta.data, sizeof(usec));
			time_t seconds = (time_t) (usec / 1000000);
			char when[64];
			struct tm tmValue;
			struct tm* tmp = localtime_r(&seconds, &tmValue);
			if (tmp && strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tmp)) {
				cJSON_AddStringToObject(item, "savedAt", when);
			}
		}
		if (mStateExtdataGet(&extdata, EXTDATA_META_CREATOR, &meta) && meta.size > 0) {
			char creator[128];
			snprintf(creator, sizeof(creator), "%.*s", (int) meta.size, (const char*) meta.data);
			cJSON_AddStringToObject(item, "creator", creator);
		}
		cJSON_AddBoolToObject(item, "hasScreenshot", mStateExtdataGet(&extdata, EXTDATA_SCREENSHOT, &meta));
	}
	mStateExtdataDeinit(&extdata);
	vf->seek(vf, 0, SEEK_SET);
	uint8_t magic[8] = {0};
	if (vf->read(vf, magic, sizeof(magic)) == sizeof(magic) && memcmp(magic, "\x89PNG", 4) == 0) {
		/* PNG savestates carry the screenshot as the image itself. */
		cJSON_DeleteItemFromObject(item, "hasScreenshot");
		cJSON_AddBoolToObject(item, "hasScreenshot", true);
	}
}

static void _toolListSaveStates(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
#if defined(ENABLE_VFS) && defined(ENABLE_DIRECTORIES)
	struct mCore* core = _core(server);
	cJSON* json = cJSON_CreateObject();
	cJSON* list = cJSON_AddArrayToObject(json, "states");
	int slot;
	for (slot = 0; slot <= 9; ++slot) {
		struct VFile* vf = _openStateSlot(core, slot, O_RDONLY);
		if (!vf) {
			continue;
		}
		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "slot", slot);
		_addStateMeta(core, vf, item);
		vf->close(vf);
		cJSON_AddItemToArray(list, item);
	}
	cJSON_AddBoolToObject(json, "undoAvailable", server->undoState != NULL);
	_resultJson(result, json);
#else
	UNUSED(server);
	_resultError(result, "This build has no savestate directory support.");
#endif
}

static void _toolGetSaveStateInfo(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	const char* path = _argString(args, "path");
	struct VFile* vf = NULL;
	if (path) {
		vf = VFileOpen(path, O_RDONLY);
	} else {
#if defined(ENABLE_VFS) && defined(ENABLE_DIRECTORIES)
		int slot = _argInt(args, "slot", -1);
		if (slot < 0 || slot > 9) {
			_resultError(result, "Pass 'slot' (0-9) or 'path'");
			return;
		}
		vf = _openStateSlot(core, slot, O_RDONLY);
#endif
	}
	if (!vf) {
		_resultError(result, "No such savestate");
		return;
	}
	cJSON* json = cJSON_CreateObject();
	_addStateMeta(core, vf, json);
	bool png = false;
	vf->seek(vf, 0, SEEK_SET);
	uint8_t magic[4] = {0};
	if (vf->read(vf, magic, 4) == 4 && memcmp(magic, "\x89PNG", 4) == 0) {
		png = true;
	}
	_resultJson(result, json);
	if (png && _argBool(args, "screenshot", true)) {
		/* mGBA savestates with screenshots are PNG files whose image is the screenshot. */
		size_t size = vf->size(vf);
		void* data = malloc(size);
		vf->seek(vf, 0, SEEK_SET);
		if (vf->read(vf, data, size) == (ssize_t) size) {
			char* encoded = _base64Encode(data, size);
			cJSON* item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "type", "image");
			cJSON_AddStringToObject(item, "mimeType", "image/png");
			cJSON_AddStringToObject(item, "data", encoded);
			cJSON_AddItemToArray(result->content, item);
			free(encoded);
		}
		free(data);
	}
	vf->close(vf);
}

static void _toolDeleteSaveState(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	int slot = _argInt(args, "slot", -1);
	if (slot < 0 || slot > 9) {
		_resultError(result, "'slot' must be 0-9");
		return;
	}
#if defined(ENABLE_VFS) && defined(ENABLE_DIRECTORIES)
	struct VFile* vf = _openStateSlot(core, slot, O_RDONLY);
	if (!vf) {
		_resultError(result, "Slot %d is empty", slot);
		return;
	}
	vf->close(vf);
	mCoreDeleteState(core, slot);
	_resultPrintf(result, "Deleted savestate slot %d.", slot);
#else
	UNUSED(core);
	_resultError(result, "This build has no savestate directory support.");
#endif
}

static void _toolUndoLoadState(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mCore* core = _core(server);
	if (!server->undoState) {
		_resultError(result, "Nothing to undo: no state has been loaded through MCP since the game started.");
		return;
	}
	if (server->undoStateSize != core->stateSize(core) || !core->loadState(core, server->undoState)) {
		_resultError(result, "Could not restore the previous state.");
		return;
	}
	free(server->undoState);
	server->undoState = NULL;
	server->undoStateSize = 0;
	_resultPrintf(result, "Restored the state from before the last load_state; now at frame %u.", core->frameCounter(core));
}

static void _rememberUndoState(struct MCPServer* server) {
	struct mCore* core = _core(server);
	size_t size = core->stateSize(core);
	void* state = malloc(size);
	if (!state) {
		return;
	}
	if (!core->saveState(core, state)) {
		free(state);
		return;
	}
	free(server->undoState);
	server->undoState = state;
	server->undoStateSize = size;
}

static const char* _saveTypeName(int type) {
#ifdef M_CORE_GBA
	switch (type) {
	case GBA_SAVEDATA_AUTODETECT:
		return "autodetect (not used yet)";
	case GBA_SAVEDATA_FORCE_NONE:
		return "none";
	case GBA_SAVEDATA_SRAM:
		return "SRAM 32KiB";
	case GBA_SAVEDATA_FLASH512:
		return "Flash 64KiB";
	case GBA_SAVEDATA_FLASH1M:
		return "Flash 128KiB";
	case GBA_SAVEDATA_EEPROM:
		return "EEPROM 8KiB";
	case GBA_SAVEDATA_EEPROM512:
		return "EEPROM 512B";
	case GBA_SAVEDATA_SRAM512:
		return "SRAM 64KiB";
	}
#endif
	UNUSED(type);
	return "unknown";
}

static void _toolGetRomInfo(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(args);
	UNUSED(context);
	struct mCore* core = _core(server);
	cJSON* json = cJSON_CreateObject();
	struct mGameInfo info = {0};
	core->getGameInfo(core, &info);
	cJSON_AddStringToObject(json, "platform", core->platform(core) == mPLATFORM_GB ? "GB" : "GBA");
	cJSON_AddStringToObject(json, "title", info.title);
	cJSON_AddStringToObject(json, "code", info.code);
	cJSON_AddStringToObject(json, "maker", info.maker);
	cJSON_AddNumberToObject(json, "version", info.version);
	cJSON_AddStringToObject(json, "fileBaseName", core->dirs.baseName);
	uint32_t crc = 0;
	core->checksum(core, &crc, mCHECKSUM_CRC32);
	char hex[16];
	snprintf(hex, sizeof(hex), "%08X", crc);
	cJSON_AddStringToObject(json, "crc32", hex);
#ifdef M_CORE_GBA
	struct GBA* gba = _gba(core);
	if (gba) {
		cJSON_AddNumberToObject(json, "romSize", (double) gba->memory.romSize);
		cJSON* save = cJSON_AddObjectToObject(json, "save");
		cJSON_AddStringToObject(save, "type", _saveTypeName(gba->memory.savedata.type));
		cJSON_AddNumberToObject(save, "size", (double) GBASavedataSize(&gba->memory.savedata));
		cJSON* hw = cJSON_AddArrayToObject(json, "cartridgeHardware");
		uint32_t devices = gba->memory.hw.devices;
		static const struct { uint32_t bit; const char* name; } devs[] = {
			{ HW_RTC, "rtc" }, { HW_RUMBLE, "rumble" }, { HW_LIGHT_SENSOR, "light_sensor" }, { HW_GYRO, "gyro" },
			{ HW_TILT, "tilt" }, { HW_GB_PLAYER, "gb_player" }, { HW_EREADER, "e_reader" }
		};
		size_t i;
		for (i = 0; i < sizeof(devs) / sizeof(devs[0]); ++i) {
			if (devices & devs[i].bit) {
				cJSON_AddItemToArray(hw, cJSON_CreateString(devs[i].name));
			}
		}
		cJSON* bios = cJSON_AddObjectToObject(json, "bios");
		cJSON_AddStringToObject(bios, "kind", gba->memory.fullBios ? "real BIOS file" : "built-in HLE BIOS");
		if (gba->memory.fullBios) {
			snprintf(hex, sizeof(hex), "%08X", gba->biosChecksum);
			cJSON_AddStringToObject(bios, "checksum", hex);
			cJSON_AddStringToObject(bios, "identified", gba->biosChecksum == GBA_BIOS_CHECKSUM ? "official GBA BIOS" :
			                        (gba->biosChecksum == GBA_DS_BIOS_CHECKSUM ? "DS GBA-mode BIOS" : "unknown"));
		}
	}
#endif
	_resultJson(result, json);
}

static void _toolExportSave(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	void* data = NULL;
	size_t size = core->savedataClone(core, &data);
	if (!size || !data) {
		free(data);
		_resultError(result, "This game has no save data (yet).");
		return;
	}
	char buffer[PATH_MAX];
	const char* path = _outputPath(args, "save", "sav", buffer, sizeof(buffer));
	bool ok = _writeFile(path, data, size);
	free(data);
	if (!ok) {
		_resultError(result, "Could not write %s", path);
		return;
	}
	cJSON* json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "path", path);
	cJSON_AddNumberToObject(json, "size", (double) size);
	_resultJson(result, json);
}

static void _toolImportSave(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	const char* path = _argString(args, "path");
	if (!path) {
		_resultError(result, "Missing required argument 'path'");
		return;
	}
	size_t size;
	void* data = _readFile(path, &size, 0x200000);
	if (!data) {
		_resultError(result, "Could not read %s (missing, or larger than 2 MiB)", path);
		return;
	}
	/* Back up the current save first; overwriting a save cannot be undone otherwise. */
	void* old = NULL;
	size_t oldSize = core->savedataClone(core, &old);
	char backup[PATH_MAX] = "";
	if (oldSize && old) {
		snprintf(backup, sizeof(backup), "%s%cmgba-mcp-save-backup-%u.sav", _tempDir(), PATH_SEP[0], (unsigned) time(NULL));
		if (!_writeFile(backup, old, oldSize)) {
			backup[0] = '\0';
		}
	}
	free(old);
	bool writeback = _argBool(args, "write_to_save_file", true);
	bool ok = core->savedataRestore(core, data, size, writeback);
	free(data);
	if (!ok) {
		_resultError(result, "The core rejected the save data.");
		return;
	}
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "size", (double) size);
	cJSON_AddBoolToObject(json, "writtenToSaveFile", writeback);
	if (backup[0]) {
		cJSON_AddStringToObject(json, "previousSaveBackup", backup);
	}
	cJSON_AddStringToObject(json, "note", "Most games only read their save at boot: reset to see it.");
	_resultJson(result, json);
}

static void _toolLoadRom(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	if (mCoreThreadGet()) {
		_resultError(result, "In the GUI, open games with File > Load ROM (the window manages the game's lifetime). load_rom works in mgba-headless.");
		return;
	}
	const char* path = _argString(args, "path");
	if (!path) {
		_resultError(result, "Missing required argument 'path'");
		return;
	}
	struct VFile* probe = VFileOpen(path, O_RDONLY);
	if (!probe) {
		_resultError(result, "Could not open %s", path);
		return;
	}
	enum mPlatform platform = mCoreIsCompatible(probe);
	probe->close(probe);
	if (platform == mPLATFORM_NONE) {
		_resultError(result, "Not a supported ROM: %s", path);
		return;
	}
	if (platform != core->platform(core)) {
		_resultError(result, "The running core can only switch to a ROM of the same platform; restart mgba-headless with this file instead.");
		return;
	}
	struct mCheatDevice* device = core->cheatDevice ? core->cheatDevice(core) : NULL;
	if (device) {
		mCheatDeviceClear(device);
	}
	if (!mCoreLoadFile(core, path)) {
		_resultError(result, "Could not load %s", path);
		return;
	}
	mCoreAutoloadSave(core);
	core->reset(core);
	mCoreMemorySearchResultsClear(&server->searchResults);
	server->searchActive = false;
	free(server->undoState);
	server->undoState = NULL;
	struct mGameInfo info = {0};
	core->getGameInfo(core, &info);
	_resultPrintf(result, "Loaded %s (%s).", path, info.title);
}

static void _toolShutdown(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	if (!_argBool(args, "confirm", false)) {
		_resultError(result, "This closes the game (and exits mgba-headless). Unsaved progress that isn't in the game's save is lost. Call again with confirm=true.");
		return;
	}
	_resultText(result, "Shutting down.");
	mDebuggerShutdown(server->d.p);
}

static void _toolListRoms(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(server);
	UNUSED(context);
	const char* path = _argString(args, "directory");
	if (!path) {
		_resultError(result, "Missing required argument 'directory'");
		return;
	}
	struct VDir* dir = VDirOpen(path);
	if (!dir) {
		_resultError(result, "Could not open directory %s", path);
		return;
	}
	static const char* const exts[] = { ".gba", ".agb", ".mb", ".gb", ".gbc", ".sgb", ".zip", ".7z", NULL };
	cJSON* json = cJSON_CreateObject();
	cJSON* list = cJSON_AddArrayToObject(json, "roms");
	struct VDirEntry* entry;
	int count = 0;
	while ((entry = dir->listNext(dir)) && count < 1000) {
		if (entry->type(entry) == VFS_DIRECTORY) {
			continue;
		}
		const char* name = entry->name(entry);
		const char* dot = strrchr(name, '.');
		if (!dot) {
			continue;
		}
		size_t i;
		for (i = 0; exts[i]; ++i) {
			if (strcasecmp(dot, exts[i]) == 0) {
				cJSON_AddItemToArray(list, cJSON_CreateString(name));
				++count;
				break;
			}
		}
	}
	dir->close(dir);
	cJSON_AddNumberToObject(json, "count", count);
	_resultJson(result, json);
}

/* ---------------------------------------------------------------------------
 * Cheats: import/export, validation, one-shot application
 * ------------------------------------------------------------------------- */

static void _toolImportCheats(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCheatDevice* device = _cheatDevice(_core(server), result);
	if (!device) {
		return;
	}
	const char* text = _argString(args, "text");
	const char* path = _argString(args, "path");
	if ((text != NULL) == (path != NULL)) {
		_resultError(result, "Pass either 'text' (contents of a .cheats file) or 'path'");
		return;
	}
	struct VFile* vf = text ? VFileMemChunk(text, strlen(text)) : VFileOpen(path, O_RDONLY);
	if (!vf) {
		_resultError(result, "Could not open %s", path ? path : "text");
		return;
	}
	size_t before = mCheatSetsSize(&device->cheats);
	bool ok = mCheatParseFile(device, vf);
	vf->close(vf);
	size_t added = mCheatSetsSize(&device->cheats) - before;
	if (!ok && !added) {
		_resultError(result, "Could not parse the cheats. Expected mGBA's .cheats format (\"# Name\" lines followed by codes; \"!disabled\" to add disabled). For bare codes use add_cheat.");
		return;
	}
	size_t i;
	for (i = before; i < mCheatSetsSize(&device->cheats); ++i) {
		struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, i);
		if (set->refresh) {
			set->refresh(set, device);
		}
	}
	_cheatAutosave(device);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "imported", (double) added);
	cJSON_AddItemToObject(json, "cheats", _cheatListJson(device));
	_resultJson(result, json);
}

static void _toolExportCheats(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCheatDevice* device = _cheatDevice(_core(server), result);
	if (!device) {
		return;
	}
	char buffer[PATH_MAX];
	const char* path = _outputPath(args, "cheats", "cheats", buffer, sizeof(buffer));
	struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
	if (!vf || !mCheatSaveFile(device, vf)) {
		if (vf) {
			vf->close(vf);
		}
		_resultError(result, "Could not write %s", path);
		return;
	}
	vf->close(vf);
	_resultPrintf(result, "Exported %zu cheats to %s", mCheatSetsSize(&device->cheats), path);
}

static void _toolValidateCheat(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	struct mCheatDevice* device = _cheatDevice(core, result);
	if (!device) {
		return;
	}
	const char* code = _argString(args, "code");
	if (!code) {
		_resultError(result, "Missing required argument 'code'");
		return;
	}
	int type;
	if (!_cheatType(core, _argString(args, "type"), &type, result)) {
		return;
	}
	struct mCheatSet* set = device->createSet(device, "validate");
	int failures = 0;
	size_t added = _addCheatCode(set, code, type, &failures);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddBoolToObject(json, "valid", added > 0 && failures == 0);
	cJSON_AddNumberToObject(json, "linesAccepted", (double) added);
	cJSON_AddNumberToObject(json, "wordsRejected", failures);
	cJSON_AddNumberToObject(json, "operations", (double) mCheatListSize(&set->list));
	mCheatSetDeinit(set);
	_resultJson(result, json);
}

static void _toolApplyCheatOnce(struct MCPServer* server, const cJSON* args, enum MCPContext context, struct MCPToolResult* result) {
	UNUSED(context);
	struct mCore* core = _core(server);
	struct mCheatDevice* device = _cheatDevice(core, result);
	if (!device) {
		return;
	}
	const char* code = _argString(args, "code");
	if (code) {
		int type;
		if (!_cheatType(core, _argString(args, "type"), &type, result)) {
			return;
		}
		struct mCheatSet* set = device->createSet(device, "once");
		int failures = 0;
		if (!_addCheatCode(set, code, type, &failures)) {
			mCheatSetDeinit(set);
			_resultError(result, "No valid cheat code found.");
			return;
		}
		set->enabled = true;
		mCheatRefresh(device, set);
		mCheatSetDeinit(set);
		_resultText(result, "Applied once. The game may overwrite the values again; use add_cheat to keep them.");
		return;
	}
	struct mCheatSet* set = _cheatAt(device, args, result);
	if (!set) {
		return;
	}
	bool wasEnabled = set->enabled;
	set->enabled = true;
	mCheatRefresh(device, set);
	set->enabled = wasEnabled;
	if (!wasEnabled) {
		mCheatRefresh(device, set);
	}
	_resultPrintf(result, "Applied '%s' once.", set->name ? set->name : "");
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
	  "Capture the current screen as a PNG image. Pass 'path' to also save it to a file.",
	  "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Also save the PNG here.\"},\"inline\":{\"type\":\"boolean\",\"default\":true,\"description\":\"Return the image in the response.\"}}}",
	  MCP_REQ_ANY, _toolScreenshot },
	{ "reset",
	  "Reset the emulated console.",
	  EMPTY_SCHEMA, MCP_REQ_QUIESCENT, _toolReset },
	{ "read_memory",
	  "Read memory without side effects. format=dump returns a hex dump; u8/u16/u32 return little-endian values. GBA map: 0x02000000 EWRAM, 0x03000000 IWRAM, 0x04000000 I/O, 0x05000000 palette, 0x06000000 VRAM, 0x07000000 OAM, 0x08000000 ROM.",
	  "{\"type\":\"object\",\"properties\":{\"address\":" ADDRESS_SCHEMA("Start address.") ",\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":16,\"description\":\"Bytes to read.\"},\"format\":{\"type\":\"string\",\"enum\":[\"dump\",\"u8\",\"u16\",\"u32\",\"base64\"],\"default\":\"dump\",\"description\":\"dump/u8/u16/u32 up to 4096 bytes; base64 up to 4 MiB.\"},\"path\":{\"type\":\"string\",\"description\":\"Write the raw bytes to this file instead (up to 4 MiB).\"},\"segment\":{\"type\":\"integer\",\"default\":-1,\"description\":\"Bank for banked memory; -1 = current.\"}},\"required\":[\"address\"]}",
	  MCP_REQ_ANY, _toolReadMemory },
	{ "write_memory",
	  "Write memory. Pass 'data' as hex bytes (\"DE AD BE EF\"), 'base64', a file 'path' (up to 4 MiB), or 'value' with 'width' (1, 2 or 4 bytes, little-endian).",
	  "{\"type\":\"object\",\"properties\":{\"address\":" ADDRESS_SCHEMA("Start address.") ",\"data\":{\"type\":\"string\"},\"base64\":{\"type\":\"string\"},\"path\":{\"type\":\"string\",\"description\":\"Read the bytes to write from this file.\"},\"value\":{\"type\":[\"integer\",\"string\"]},\"width\":{\"type\":\"integer\",\"enum\":[1,2,4],\"default\":1},\"segment\":{\"type\":\"integer\",\"default\":-1}},\"required\":[\"address\"]}",
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
	  "Pause execution when the CPU is about to execute the instruction at an address, optionally only when 'condition' holds. Returns the breakpoint id.",
	  "{\"type\":\"object\",\"properties\":{\"address\":" ADDRESS_SCHEMA("Instruction address.") ",\"condition\":{\"type\":\"string\",\"description\":\"mGBA debugger expression, e.g. \\\"r0 == 5\\\" or \\\"*0x02000010 == 3\\\" (* reads one byte).\"},\"segment\":{\"type\":\"integer\",\"default\":-1}},\"required\":[\"address\"]}",
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
	{ "step_over",
	  "Execute one instruction, but run a whole function call (BL/BLX) to its return instead of stepping into it. Stops early on breakpoints/watchpoints.",
	  "{\"type\":\"object\",\"properties\":{\"max_instructions\":{\"type\":\"integer\",\"default\":1000000}}}",
	  MCP_REQ_EXECUTE, _toolStepOver },
	{ "step_out",
	  "Run until the current function returns. Exact when stack tracing was enabled (backtrace enable=true) before the function was entered; otherwise runs until PC reaches LR.",
	  "{\"type\":\"object\",\"properties\":{\"max_instructions\":{\"type\":\"integer\",\"default\":1000000}}}",
	  MCP_REQ_EXECUTE, _toolStepOut },
	{ "backtrace",
	  "Show the call stack. Pass enable=true to start stack tracing (calls are tracked from then on; also makes step_out exact) or enable=false to stop it.",
	  "{\"type\":\"object\",\"properties\":{\"enable\":{\"type\":\"boolean\"}}}",
	  MCP_REQ_ANY, _toolBacktrace },
	{ "trace",
	  "Execute N instructions, logging each instruction with the registers. Inline up to 2000; pass 'path' to log up to 5,000,000 to a file.",
	  "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\",\"minimum\":1,\"default\":100},\"path\":{\"type\":\"string\"}}}",
	  MCP_REQ_EXECUTE, _toolTrace },
	{ "set_breakpoint_enabled",
	  "Enable or disable a breakpoint or watchpoint without deleting it.",
	  "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"id\",\"enabled\"]}",
	  MCP_REQ_ANY, _toolSetBreakpointEnabled },
	{ "clear_breakpoints",
	  "Delete all breakpoints and watchpoints set through MCP.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolClearBreakpoints },
	{ "get_cpu_state",
	  "Detailed ARM CPU state: decoded CPSR (flags, IRQ/FIQ masks, Thumb, mode), SPSR, banked registers of every mode, halt state and pending interrupts.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolGetCpuState },
	{ "dump_memory",
	  "Save a whole memory region (wram, iwram, io, palette, vram, oam, rom, sram, bios... see list_memory_blocks) to a file. Without 'path', a file in the temp folder is used.",
	  "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"region\"]}",
	  MCP_REQ_ANY, _toolDumpMemory },
	{ "search_bytes",
	  "Find a byte pattern (hex, ?? = wildcard) in memory, e.g. text, a pointer or a code sequence. For hunting changing values use memory_search_start instead.",
	  "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"e.g. \\\"48 45 ?? 4C 4F\\\"\"},\"mask\":{\"type\":\"string\",\"description\":\"Optional hex mask, FF = must match.\"},\"region\":{\"type\":\"string\",\"default\":\"ram\",\"description\":\"ram, all, or a block name such as rom or vram.\"},\"max_results\":{\"type\":\"integer\",\"default\":100,\"maximum\":1000}},\"required\":[\"pattern\"]}",
	  MCP_REQ_ANY, _toolSearchBytes },
	{ "snapshot_memory",
	  "Copy work RAM now, to compare later with diff_memory (what changed after an action?).",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolSnapshotMemory },
	{ "diff_memory",
	  "Compare work RAM with the last snapshot_memory and list addresses that changed (or increased / decreased).",
	  "{\"type\":\"object\",\"properties\":{\"width\":{\"type\":\"integer\",\"enum\":[1,2,4],\"default\":1},\"filter\":{\"type\":\"string\",\"enum\":[\"changed\",\"increased\",\"decreased\"],\"default\":\"changed\"},\"max_results\":{\"type\":\"integer\",\"default\":100,\"maximum\":2000},\"update_snapshot\":{\"type\":\"boolean\",\"default\":false}}}",
	  MCP_REQ_ANY, _toolDiffMemory },
	{ "find_free_ram",
	  "Find the largest runs of a fill byte (default 0x00) in work RAM: candidate free space for patches or injected code.",
	  "{\"type\":\"object\",\"properties\":{\"fill\":{\"type\":[\"integer\",\"string\"],\"default\":0},\"min_size\":{\"type\":\"integer\",\"default\":64},\"max_results\":{\"type\":\"integer\",\"default\":20}}}",
	  MCP_REQ_ANY, _toolFindFreeRam },
	{ "memory_watch",
	  "Keep a labeled list of addresses to check repeatedly. action=add/remove/clear/list; list shows each value, the value at the previous list, and whether it changed.",
	  "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"add\",\"remove\",\"list\",\"clear\"],\"default\":\"list\"},\"address\":{\"type\":[\"integer\",\"string\"]},\"width\":{\"type\":\"integer\",\"enum\":[1,2,4],\"default\":1},\"label\":{\"type\":\"string\"}}}",
	  MCP_REQ_ANY, _toolMemoryWatch },
#ifdef M_CORE_GBA
	{ "get_video_state",
	  "GBA display state, decoded: video mode, enabled layers, each background's settings (tile/map base, size, scroll, affine matrix), windows, blending, OBJ mapping, VCOUNT.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolGetVideoState },
	{ "dump_palette",
	  "Image of all 512 palette colors (background and sprite palettes). include_values=true also returns the raw BGR555 values.",
	  "{\"type\":\"object\",\"properties\":{\"include_values\":{\"type\":\"boolean\",\"default\":false},\"path\":{\"type\":\"string\"}}}",
	  MCP_REQ_ANY, _toolDumpPalette },
	{ "dump_tiles",
	  "Image of a VRAM tile block (16 KiB): blocks 0-3 hold background tiles, 4-5 sprite tiles. Choose 4bpp/8bpp and the palette.",
	  "{\"type\":\"object\",\"properties\":{\"char_block\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":5,\"default\":0},\"bpp\":{\"type\":\"integer\",\"enum\":[4,8],\"default\":4},\"palette\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":15,\"default\":0},\"palette_type\":{\"type\":\"string\",\"enum\":[\"background\",\"sprite\"]},\"path\":{\"type\":\"string\"}}}",
	  MCP_REQ_ANY, _toolDumpTiles },
	{ "dump_background",
	  "Image of a whole background layer (BG0-3) as stored in VRAM: the full tile map for text/affine layers, or the frame buffer in bitmap modes 3-5.",
	  "{\"type\":\"object\",\"properties\":{\"layer\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3,\"default\":0},\"frame\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"Bitmap modes 4/5: which frame buffer (default: the displayed one).\"},\"path\":{\"type\":\"string\"}}}",
	  MCP_REQ_ANY, _toolDumpBackground },
	{ "list_sprites",
	  "Decode OAM: every sprite's position, size, tile, palette, priority, flip/affine and whether it is on screen.",
	  "{\"type\":\"object\",\"properties\":{\"include_hidden\":{\"type\":\"boolean\",\"default\":false},\"on_screen_only\":{\"type\":\"boolean\",\"default\":false}}}",
	  MCP_REQ_ANY, _toolListSprites },
	{ "dump_sprite",
	  "Image of one sprite (by OAM index from list_sprites), optionally scaled up.",
	  "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":127},\"scale\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8,\"default\":1},\"path\":{\"type\":\"string\"}},\"required\":[\"index\"]}",
	  MCP_REQ_ANY, _toolDumpSprite },
	{ "get_audio_state",
	  "GBA sound state, decoded: master enable, which PSG channels are playing, mixing, DirectSound A/B routing and timers, and each channel's settings and frequency.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolGetAudioState },
	{ "get_hardware_state",
	  "GBA hardware state: DMA channels, timers, interrupts (enabled/requested/pending), keypad register and IRQ, wait states and BIOS.",
	  "{\"type\":\"object\",\"properties\":{\"subsystem\":{\"type\":\"string\",\"enum\":[\"dma\",\"timers\",\"interrupts\",\"keypad\",\"system\",\"all\"],\"default\":\"all\"}}}",
	  MCP_REQ_ANY, _toolGetHardwareState },
#endif
	{ "input_sequence",
	  "Play a timed sequence of button presses, e.g. [{buttons:[\"Down\"],frames:10},{buttons:[],frames:5},{buttons:[\"A\"],frames:2}]. When paused it runs right away (screenshot=true shows the end); when running it plays in the background.",
	  "{\"type\":\"object\",\"properties\":{\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"buttons\":" BUTTONS_SCHEMA ",\"frames\":{\"type\":\"integer\",\"minimum\":1,\"default\":1}}}},\"screenshot\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"steps\"]}",
	  MCP_REQ_ANY, _toolInputSequence },
	{ "get_controller_state",
	  "Which buttons are held by the emulator and seen by the game, plus any timed hold or sequence in progress.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolGetControllerState },
	{ "set_speed",
	  "Change emulation speed in the GUI: speed multiplier (1 = normal) or fast_forward=true for unlimited. The headless build is always unthrottled.",
	  "{\"type\":\"object\",\"properties\":{\"speed\":{\"type\":\"number\",\"default\":1},\"fast_forward\":{\"type\":\"boolean\",\"default\":false}}}",
	  MCP_REQ_ANY, _toolSetSpeed },
	{ "get_settings",
	  "Read emulator settings (frameskip, volume, mute, BIOS, paths, ...), or any mGBA config key with 'key'.",
	  "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}}}",
	  MCP_REQ_ANY, _toolGetSettings },
	{ "set_setting",
	  "Change an emulator setting for this session (e.g. key=mute value=true, key=frameskip value=1, key=volume value=128).",
	  "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"value\":{\"type\":[\"string\",\"number\",\"boolean\"]}},\"required\":[\"key\",\"value\"]}",
	  MCP_REQ_ANY, _toolSetSetting },
	{ "list_save_states",
	  "List savestate slots (0-9) that exist for this game, with save time and whether they have a screenshot.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolListSaveStates },
	{ "get_save_state_info",
	  "Details of a savestate (slot or path): save time, creator, and its screenshot image.",
	  "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":9},\"path\":{\"type\":\"string\"},\"screenshot\":{\"type\":\"boolean\",\"default\":true}}}",
	  MCP_REQ_ANY, _toolGetSaveStateInfo },
	{ "delete_save_state",
	  "Delete a savestate slot.",
	  "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":9}},\"required\":[\"slot\"]}",
	  MCP_REQ_ANY, _toolDeleteSaveState },
	{ "undo_load_state",
	  "Go back to the moment just before the last load_state.",
	  EMPTY_SCHEMA, MCP_REQ_QUIESCENT, _toolUndoLoadState },
	{ "get_rom_info",
	  "Information about the loaded game: title, code, maker, version, CRC32, ROM size, save type and size, cartridge hardware (RTC, gyro...) and BIOS.",
	  EMPTY_SCHEMA, MCP_REQ_ANY, _toolGetRomInfo },
	{ "export_save",
	  "Save the game's battery save data (SRAM/Flash/EEPROM) to a file.",
	  "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
	  MCP_REQ_ANY, _toolExportSave },
	{ "import_save",
	  "Replace the game's battery save data with a file (the previous save is backed up to the temp folder first). Reset afterwards so the game reads it.",
	  "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"write_to_save_file\":{\"type\":\"boolean\",\"default\":true}},\"required\":[\"path\"]}",
	  MCP_REQ_QUIESCENT, _toolImportSave },
	{ "list_roms",
	  "List ROM files (.gba, .gb, .gbc, .zip, .7z...) in a folder.",
	  "{\"type\":\"object\",\"properties\":{\"directory\":{\"type\":\"string\"}},\"required\":[\"directory\"]}",
	  MCP_REQ_ANY, _toolListRoms },
	{ "load_rom",
	  "Switch to another ROM of the same platform (mgba-headless only; in the GUI use File > Load ROM).",
	  "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
	  MCP_REQ_QUIESCENT, _toolLoadRom },
	{ "shutdown",
	  "Close the game (exits mgba-headless). Requires confirm=true.",
	  "{\"type\":\"object\",\"properties\":{\"confirm\":{\"type\":\"boolean\"}},\"required\":[\"confirm\"]}",
	  MCP_REQ_ANY, _toolShutdown },
	{ "import_cheats",
	  "Add cheats from mGBA .cheats text or a .cheats file.",
	  "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}}}",
	  MCP_REQ_ANY, _toolImportCheats },
	{ "export_cheats",
	  "Save all cheats to a .cheats file.",
	  "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
	  MCP_REQ_ANY, _toolExportCheats },
	{ "validate_cheat",
	  "Check whether a cheat code parses (and how many operations it has) without adding it.",
	  "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"type\":{\"type\":\"string\",\"enum\":[\"auto\",\"gameshark\",\"action_replay\",\"codebreaker\",\"vba\",\"game_genie\"],\"default\":\"auto\"}},\"required\":[\"code\"]}",
	  MCP_REQ_ANY, _toolValidateCheat },
	{ "apply_cheat_once",
	  "Apply a cheat code (or a listed cheat by index) a single time instead of every frame.",
	  "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"type\":{\"type\":\"string\",\"default\":\"auto\"},\"index\":{\"type\":\"integer\"}}}",
	  MCP_REQ_ANY, _toolApplyCheatOnce },
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
	_sequenceTick(server);
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
	/* Search results, snapshots and undo data belong to the game that is going away. */
	mCoreMemorySearchResultsClear(&server->searchResults);
	server->searchActive = false;
	free(server->undoState);
	server->undoState = NULL;
	server->undoStateSize = 0;
	free(server->snapshot);
	server->snapshot = NULL;
	server->snapshotRegions = 0;
	server->nWatches = 0;
	server->sequenceLength = 0;
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
