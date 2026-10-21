// Illuminated - Soul Gem Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// DevBench, when it is in the load order (it is optional; nothing here runs without it):
//   inspect kind=illuminatedsoulgems     the lighting found, the settings, the gems known (by look, and those left to another
//                                        light mod), and every gem light near the player now (its gem, color, fade, radius, place)
//   illuminatedsoulgems.control          action = set (setting + value: one menu setting changed the way the menu does - saved,
//                                        then applied on the main thread; the reply is the state after it) | rescan (look for
//                                        the gems around the player again now)

#include "Plugin.h"

#include "DevBenchGlue.h"

namespace Plugin
{
	namespace
	{
		using DevBenchGlue::json;

		constexpr const char* kKey = "illuminatedsoulgems";

		constexpr const char* kDescriptor =
			R"json({"description":"Illuminated - Soul Gem Lighting NG - the lighting found (automatic), the settings, the soul gems this plugin lights (by look: the game's own, Praedy's, Rustic, named, stars, Soul Cairn) and those it leaves to another light mod (Emissive Soul Gems on Light Placer, other Light Placer configs, RE::Light, and the gems near the player whose own model's light shines here - ENB particle lights, or a particle-light config on Community Shaders), and every gem light near the player now with its color, fade, radius and place. Read only.","inputSchema":{"type":"object","properties":{}},"readOnly":true})json";

		constexpr const char* kTool =
			R"json({"description":"Illuminated - Soul Gem Lighting NG (lights every soul gem in the world): change its settings the way its menu does, or look for gems again. action=set changes one setting (saved, then applied on the main thread) and replies with the state after it: lights (0/1), brightness (25-200, percent), reach (50-150, percent), pulse (0/1, the soft pulse). action=rescan looks for the gems around the player again at once. Read state with inspect kind=illuminatedsoulgems.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["set","rescan"]},"setting":{"type":"string","enum":["lights","brightness","reach","pulse"]},"value":{"type":"number"}},"required":["action"]}})json";

		json SettingsJson(const Gems::Settings& a_s)
		{
			return json{ { "lights", a_s.on }, { "brightness", a_s.brightness }, { "reach", a_s.reach }, { "pulse", a_s.pulse } };
		}

		// everything, read on the main thread (the lights are the main thread's)
		json StateNow()
		{
			auto now = DevBenchGlue::OnMainThread([]() {
				json j = json::parse(GemsReport(), nullptr, false);
				if (!j.is_object()) {
					j = json::object();
				}
				j["lighting"] = json::parse(LightingReport(), nullptr, false);
				j["settings"] = SettingsJson(SettingsNow());
				return j;
			});
			return now ? *now : DevBenchGlue::NotRunYet();
		}

		void Handler(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				DevBenchGlue::Reply(a_sink, a_write, StateNow());
			} catch (...) {
				DevBenchGlue::Reply(a_sink, a_write, DevBenchGlue::Refusal("the soul gem report could not be made"));
			}
		}

		json Act(const json& a_args)
		{
			const auto action = DevBenchGlue::Text(a_args, "action");
			if (action == "rescan") {
				if (!Later("devbench rescan", []() { ApplyNow(); })) {
					return DevBenchGlue::NotQueued();
				}
				return json{ { "ok", true }, { "queued", true } };
			}
			if (action != "set") {
				return DevBenchGlue::Refusal("unknown action '" + action + "'", json::array({ "set", "rescan" }));
			}
			const auto key = DevBenchGlue::Text(a_args, "setting");
			const auto value = DevBenchGlue::Number(a_args, "value");
			if (!value) {
				return DevBenchGlue::Refusal("set needs a number value");
			}
			auto      s = SettingsNow();
			const int v = static_cast<int>(std::clamp(*value, -100000.0, 100000.0));  // finite (Number); in range before the cast
			if (key == "lights") {
				s.on = v != 0;
			} else if (key == "brightness") {
				s.brightness = v;
			} else if (key == "reach") {
				s.reach = v;
			} else if (key == "pulse") {
				s.pulse = v != 0;
			} else {
				return DevBenchGlue::Refusal("unknown setting '" + key + "'", json::array({ "lights", "brightness", "reach", "pulse" }));
			}
			SetSettings(s);
			SaveSettings();
			auto now = DevBenchGlue::OnMainThread([]() {
				ApplyNow();
				return SettingsJson(SettingsNow());
			});
			if (!now) {
				return DevBenchGlue::NotRunYet();
			}
			if (DevBenchGlue::Refused(*now)) {
				return *now;
			}
			return json{ { "ok", true }, { "setting", key }, { "settings", *now } };
		}

		void Control(void*, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				DevBenchGlue::Reply(a_sink, a_write, Act(DevBenchGlue::Args(a_args)));
			} catch (...) {
				DevBenchGlue::Reply(a_sink, a_write, DevBenchGlue::Refusal("Illuminated - Soul Gem Lighting NG could not handle that"));
			}
		}
	}

	void OfferToDevBench()
	{
		auto* devbench = DevBenchAPI::GetDevBenchInterface001();
		if (!devbench) {
			SKSE::log::info("[DEVBENCH] DevBench is not in this load order");
			return;
		}
		const auto build = devbench->GetBuildNumber();
		if (build < DevBenchGlue::kNeedsBuild) {
			SKSE::log::warn("[DEVBENCH] DevBench build {} is older than {} and cannot take an inspect extension", build, DevBenchGlue::kNeedsBuild);
			return;
		}
		const bool fresh = devbench->RegisterToolExtension("inspect", kKey, kDescriptor, Handler, nullptr);
		devbench->RegisterTool("illuminatedsoulgems.control", kTool, Control, nullptr);
		SKSE::log::info("[DEVBENCH] offered to DevBench build {}: inspect kind={} ({}), the tool illuminatedsoulgems.control", build, kKey,
			fresh ? "a new entry" : "replacing one already registered");
	}
}
