#ifdef PROXY
#include "version/version.h"
#else
#include <Windows.h>
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <MinHook.h>

extern "C" {
#include "lua/lua.h"
#include "lua/lauxlib.h"
}

#pragma comment(lib, "libMinHook.x64.lib")

// Type definitions
using lua_pcall_t = int(__cdecl*)(lua_State* lua_State, int nargs, int nresults, int errfunc);

// Global variables
namespace globals {
	uintptr_t baseAddress = 0;
	std::vector<const char*> scriptQueue;
	bool isExecutingCustomScripts = false;
	int scriptsExecuted = 0;
	lua_pcall_t originalLuaPcall = nullptr;
}

// Hook implementation
int pcall_hook(lua_State* lua_State, int nargs, int nresults, int errfunc) {
	globals::scriptsExecuted++;

	if (globals::isExecutingCustomScripts) {
		return globals::originalLuaPcall(lua_State, nargs, nresults, errfunc);
	}

	globals::isExecutingCustomScripts = true;

	while (!globals::scriptQueue.empty()) {
		const char* currentScript = globals::scriptQueue.back();
		globals::scriptQueue.pop_back();

		std::cout << "Executing Lua script: " << std::string(currentScript).substr(0, 50) << "...\n";

		if (luaL_dostring(lua_State, currentScript) != 0) {
			std::cerr << "Failed to execute Lua script: " << lua_tostring(lua_State, -1) << '\n';
			lua_pop(lua_State, 1);
		}
		else {
			std::cout << "Lua script executed successfully.\n";
		}
	}

	globals::isExecutingCustomScripts = false;
	return globals::originalLuaPcall(lua_State, nargs, nresults, errfunc);
}

void pushLuaFileToQueue(const std::filesystem::path& filepath) {
	std::ifstream file(filepath, std::ios::binary);
	if (!file) {
		std::cerr << "Failed to open file: " << filepath << '\n';
		return;
	}

	std::string content((std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());

	if (content.empty()) {
		std::cerr << "File is empty: " << filepath << '\n';
		return;
	}

	char* script = new char[content.length() + 1];
	std::strcpy(script, content.c_str());
	globals::scriptQueue.push_back(script);

	std::cout << "Pushed Lua file to queue: " << filepath << '\n';
}

void initializeConsole() {
	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);
}

void cleanupConsole() {
	HWND consoleWindow = GetConsoleWindow();
	FreeConsole();
	if (consoleWindow) {
		PostMessage(consoleWindow, WM_CLOSE, 0, 0);
	}
}

bool initializeMinHook(uintptr_t baseAddr) {
	if (MH_Initialize() != MH_OK) {
		std::cerr << "Failed to initialize MinHook\n";
		return false;
	}

	auto lua_pcall_addr = reinterpret_cast<lua_pcall_t>(baseAddr + 0x489EE4);

	if (MH_CreateHook(
		reinterpret_cast<void**>(lua_pcall_addr),
		&pcall_hook,
		reinterpret_cast<void**>(&globals::originalLuaPcall)) != MH_OK) {
		std::cerr << "Failed to create hook\n";
		return false;
	}

	if (MH_EnableHook(reinterpret_cast<void*>(lua_pcall_addr)) != MH_OK) {
		std::cerr << "Failed to enable hook\n";
		return false;
	}

	return true;
}

void cleanupMinHook() {
	MH_DisableHook(MH_ALL_HOOKS);
	MH_Uninitialize();
}

DWORD WINAPI MainThread(LPVOID param) {
	Sleep(5000);  // Initial delay, wait for whgame to load into the executable

	// Get executable path and create script path
	char buffer[MAX_PATH];
	if (!GetModuleFileNameA(NULL, buffer, MAX_PATH)) {
		return 1;
	}

	std::filesystem::path executablePath(buffer);
	std::filesystem::path scriptPath = executablePath.parent_path() / "run.lua";

	// Initialize
	globals::baseAddress = reinterpret_cast<uintptr_t>(GetModuleHandleA("whgame.dll"));
	initializeConsole();

	if (!initializeMinHook(globals::baseAddress)) {
		return 1;
	}

	std::cout << "DLL loaded successfully! whgame.dll base address: 0x"
		<< std::hex << globals::baseAddress << std::dec << '\n';
	std::cout << "Lua hooked! Press Insert to execute run.lua, or Delete to uninject\n";

	bool insertKeyWasPressed = false;

	// Main loop
	while (!(GetAsyncKeyState(VK_DELETE) & 0x8000)) {
		if (GetAsyncKeyState(VK_INSERT) & 0x8000) {
			if (!insertKeyWasPressed) {
				insertKeyWasPressed = true;
				globals::scriptQueue.push_back("System.LogAlways('Running script file run.lua!')");
				pushLuaFileToQueue(scriptPath);
			}
		}
		else {
			insertKeyWasPressed = false;
		}
		Sleep(100);
	}

	// Cleanup
	cleanupMinHook();
	Sleep(1000);
	std::cout << "Exiting...\n";
	Sleep(1000);
	cleanupConsole();

	FreeLibraryAndExitThread(static_cast<HMODULE>(param), 0);
	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
	switch (reason) {
	case DLL_PROCESS_ATTACH:
#ifdef PROXY
		setupWrappers();
#endif
		CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
		break;

	case DLL_PROCESS_DETACH:
		cleanupMinHook();
		for (const char* script : globals::scriptQueue) {
			delete[] script;
		}
		globals::scriptQueue.clear();
		FreeConsole();
		break;
	}
	return TRUE;
}