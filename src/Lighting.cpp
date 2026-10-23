// Illuminated - Soul Gem Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which lighting the game draws with - Illuminated's Lighting.cpp (his word for this mod: "use the auto lighting detector"),
// looked at once, at data load, never picked by the player:
//   Community Shaders   its inverse square shader, Light Limit Fix's built-in one, or CommunityShaders.dll loaded (Bottled Shaders
//                       keeps the name): a light's reach comes from its fade and its cutoff, written in Community Shaders' words
//   ENB                 } the game's own lighting: a light reaches its radius, drawn plain (GemMath.h ShownFor)
//   Vanilla             }
// Leftover ENB settings never make it ENB while Community Shaders runs (RELight 2.1's report, Illuminated's rule), and ENB's settings
// files alone never make it ENB at all: an ENB preset left in the game folder with no ENB running is Vanilla lighting.

#include "Plugin.h"

#include <Windows.h>
#include <TlHelp32.h>

namespace Plugin
{
	namespace
	{
		constexpr const char* kIslShader = "Data/Shaders/InverseSquareLighting/InverseSquareLighting.hlsli";
		// a Community Shaders that builds inverse square lights into Light Limit Fix (Bottled Shaders 1.0.10 dropped the separate
		// shader above): its attenuation reads each light's InverseSquare flag
		constexpr const char* kLlfAttenuation = "Data/Shaders/LightLimitFix/Attenuation.hlsli";
		constexpr const char* kEnbFiles[] = { "enbseries.ini", "enblocal.ini" };

		std::atomic<Lighting> gFound{ Lighting::kShaders };
		std::atomic<bool>     gIslShader{ false };
		std::atomic<bool>     gCsLoaded{ false }, gEnbRunning{ false }, gEnbFiles{ false };

		[[nodiscard]] bool EnbFilesLeft()
		{
			std::error_code ec;
			return std::ranges::any_of(kEnbFiles, [&](const char* a_file) { return std::filesystem::exists(std::filesystem::current_path() / a_file, ec); });
		}

		[[nodiscard]] bool IslBuiltIntoLightLimitFix()
		{
			std::ifstream in(std::filesystem::current_path() / kLlfAttenuation, std::ios::binary);
			if (!in) {
				return false;
			}
			const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
			return text.find("InverseSquare") != std::string::npos;
		}

		[[nodiscard]] bool CommunityShadersLoaded() { return REX::W32::GetModuleHandleA("CommunityShaders.dll") != nullptr; }

		// an ENB that is running: a loaded module exports its SDK - d3d11.dll as usual, or any other name an injector gave it
		[[nodiscard]] bool EnbRunning()
		{
			for (const char* dll : { "d3d11.dll", "dxgi.dll", "d3d9.dll" }) {
				if (const auto handle = REX::W32::GetModuleHandleA(dll); handle && REX::W32::GetProcAddress(handle, "ENBGetSDKVersion")) {
					return true;
				}
			}
			const auto snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
			if (snap == INVALID_HANDLE_VALUE) {
				return false;
			}
			bool           found = false;
			MODULEENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			for (bool more = ::Module32FirstW(snap, &entry); more && !found; more = ::Module32NextW(snap, &entry)) {
				found = ::GetProcAddress(entry.hModule, "ENBGetSDKVersion") != nullptr;
			}
			::CloseHandle(snap);
			return found;
		}
	}

	void ReadLighting()
	{
		std::error_code ec;
		const bool      shaderFile = std::filesystem::exists(std::filesystem::current_path() / kIslShader, ec);
		const bool      inLlf = IslBuiltIntoLightLimitFix();
		gCsLoaded = CommunityShadersLoaded();
		gIslShader = shaderFile || inLlf || gCsLoaded;
		gEnbRunning = EnbRunning();
		gEnbFiles = EnbFilesLeft();
		gFound = gIslShader ? Lighting::kShaders : gEnbRunning ? Lighting::kEnb :
		                                                         Lighting::kVanilla;
		SKSE::log::info(
			"lighting: {} (detected - CommunityShaders.dll {}, inverse square shader {}, in Light Limit Fix {}, ENB running {}, ENB "
			"settings in the game folder {}{}); lights drawn {}",
			LightingName(gFound), gCsLoaded ? "loaded" : "not loaded", shaderFile ? "installed" : "not installed", inLlf ? "yes" : "no",
			gEnbRunning ? "yes" : "no", gEnbFiles ? "yes" : "no", gEnbFiles && !gEnbRunning && !gIslShader ? " - left over, not used" : "",
			InverseSquare() ? "inverse square" : "plain");
	}

	Lighting LightingFound()
	{
		return gFound.load();
	}

	bool EnbRunningFound()
	{
		return gEnbRunning.load();
	}

	const char* LightingName(Lighting a_lighting)
	{
		switch (a_lighting) {
		case Lighting::kEnb:
			return "ENB";
		case Lighting::kVanilla:
			return "Vanilla";
		default:
			return "Community Shaders";
		}
	}

	bool InverseSquare()
	{
		return gFound.load() == Lighting::kShaders && gIslShader.load();
	}

	std::string LightingReport()
	{
		return std::format(
			R"({{"lighting":"{}","detected":true,"inverseSquareShader":{},"communityShadersLoaded":{},"enbRunning":{},"enbFiles":{},"inverseSquare":{}}})",
			LightingName(gFound), gIslShader.load(), gCsLoaded.load(), gEnbRunning.load(), gEnbFiles.load(), InverseSquare());
	}
}
