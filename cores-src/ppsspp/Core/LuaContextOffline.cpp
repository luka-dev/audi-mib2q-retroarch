// QNX/MHI2Q offline build: keep the lifecycle ABI without embedding Lua.

#include "Core/LuaContext.h"

LuaContext g_lua;

void LuaContext::Init() {
}

void LuaContext::Shutdown() {
}

void LuaContext::Print(LogLineType type, std::string_view text) {
	lines_.push_back({type, std::string(text), 0});
}

void LuaContext::ExecuteConsoleCommand(std::string_view) {
}
