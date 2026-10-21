// Illuminated - Soul Gem Lighting NG - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The gems and their lights.
//
// WHAT IS LIT (FindGems, once at data load): every soul gem record - lit when it holds a soul, or when the gem placed in the
// world does (a soul trapped into an empty one), and always for Azura's Star and the Black Star - and the items and pieces known
// by their own model (the Soul Essence Gem, the Soul Cairn's gem bowl and chunks). A gem's look - its color and strength -
// comes from the model its record wears in this load order, so it follows whatever mesh mod wins: Praedy's Soul Gems (Subtle
// Soul Gems' copies of them too), Rustic Soul Gems (either download), or the game's own gems by their size. Never a static or
// a museum display that only wears a gem's model (his rule: displays never get lit) - only the Soul Cairn's pieces.
//
// HOW (once a frame, after the player's update, on the main thread): every quarter second the gems around the player are
// looked for; each gets one light of ours hung on its model, made and registered the way ReLight does it (the credit below),
// in its color, at its strength and reach for the lighting found (Lighting.cpp - Community Shaders' inverse-square words, or
// the same light drawn plain on ENB and Vanilla), breathing softly when the pulse is on. A gem picked up, unloaded or turned off
// loses its light; a game loaded drops them all.
//
// NOTHING ANOTHER MOD ALREADY DOES (his rule 2026-10-19): a gem another light mod lights is left to it - a Light Placer file's
// gem (Emissive Soul Gems', CS Light's, the CS Lights add-ons'), a gem whose model already holds a light that is not ours (Light
// Placer's, RE::Light's, one inside the mesh - looked at every scan), and a gem whose own model carries a light (the ENB particle
// lights of Praedy's, Subtle's, Skyking's, Kanjs' ... meshes) wherever that light shines: always on ENB, and on Community
// Shaders when a particle-light config (Data\ParticleLights\<texture>.ini - Bottled Shaders ships one for fxglowENB) names it.
//
// CREDIT: a light is made and registered the way ReLight by Truman does it (github.com/TrumanGIT/ReLight, GPL-3.0-or-later,
// used with his permission, as Illuminated uses it): a master NiPointLight cloned for every use and handed to the shadow scene
// node.

#include "Plugin.h"

#include <nlohmann/json.hpp>

namespace Plugin
{
	namespace
	{
		constexpr const char* kName = "SoulGemLight";
		constexpr float       kAmbient = 0.1f;     // a plain light's ambient: a tenth of its color (RE::Light's rule)
		constexpr float       kScanEvery = 0.25f;  // seconds between looks around the player
		constexpr std::size_t kMaxLights = 24;     // the most gem lights at once - the nearest gems (review 2026-10-19)
		constexpr auto        kRetryAfter = std::chrono::seconds(15);
		constexpr float       kRange = 4096.0f;                               // how far from the player a gem is lit (its light reaches under 100 units)
		constexpr RE::FormID  kAzurasStar = 0x063B27, kBlackStar = 0x063B29;  // in Skyrim.esm

		// ---------------------------------------------------------- the light, as Illuminated's LightKit.h makes it
		// Community Shaders' inverse-square lighting reads a flag and the cutoff from the first eight bytes of a light's runtime data
		// (as the game lays it out, its ambient color's red and green); read and written as bytes, always after SetLightAttenuation
		namespace Isl
		{
			constexpr std::uint32_t kFlag = 1u << 10;        // InverseSquare
			constexpr std::uint32_t kInitialised = 1u << 8;  // Light Limit Fix marks every light it makes so

			void Write(RE::NiLight* a_light, const std::array<std::uint32_t, 2>& a_words) noexcept
			{
				std::memcpy(reinterpret_cast<std::byte*>(&a_light->GetLightRuntimeData()), a_words.data(), sizeof(a_words));
			}
			// a light of our own: the flag word written whole, the cutoff beside it, and the third word (Community Shaders' light
			// record id - a clone carries 1.0f there) cleared
			void SetOwn(RE::NiLight* a_light, float a_cutoff) noexcept
			{
				Write(a_light, { kFlag | kInitialised, std::bit_cast<std::uint32_t>(std::clamp(a_cutoff, Gems::kLowestCutoff, Gems::kHighestCutoff)) });
				a_light->GetLightRuntimeData().ambient.blue = 0.0f;
			}
		}

		// a new light for the scene, cloned from one master made once: ReLight found that a freshly made light, attached straight
		// away, crashes. nullptr when the game could not make one
		[[nodiscard]] RE::NiPointLight* CloneLight()
		{
			static RE::NiPointer<RE::NiPointLight> master;
			if (!master) {
				const RE::NiPointer<RE::NiPointLight> fresh(RE::NiPointLight::Create());
				auto*                                 clone = fresh ? netimmerse_cast<RE::NiPointLight*>(fresh->Clone()) : nullptr;
				if (!clone) {
					return nullptr;
				}
				master.reset(clone);
			}
			return netimmerse_cast<RE::NiPointLight*>(master->Clone());
		}

		// a light already attached and updated once, registered as ReLight registers its own: dynamic, no shadow, kept inside its
		// cell (portal strict), lighting land and water too
		[[nodiscard]] RE::BSLight* AddToScene(RE::ShadowSceneNode* a_scene, RE::NiPointLight* a_light)
		{
			RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
			params.dynamic = true;
			params.shadowLight = false;
			params.portalStrict = true;
			params.affectLand = true;
			params.affectWater = true;
			params.neverFades = true;
			params.fov = 90.0f;
			params.falloff = 1.0f;
			params.nearDistance = 5.0f;
			params.depthBias = 1.0f;
			params.sceneGraphIndex = 0;
			params.restrictedNode = nullptr;
			params.lensFlareData = nullptr;
			return a_scene && a_light ? a_scene->AddLight(a_light, params) : nullptr;
		}

		// ---------------------------------------------------------- the gems, found once at data load
		struct Gem
		{
			Gems::Look look;
			bool       needsSoul{ false };  // an empty gem record: lit only when the placed gem holds a soul
		};
		std::unordered_map<const RE::TESBoundObject*, Gem> gGems;      // the gems this plugin lights; read on the main thread after data load
		std::vector<std::pair<std::string, std::size_t>>   gFamilies;  // the lit gem records by look, for the menu
		std::array<std::size_t, 4>                         gLeftTo{};  // gem records left to another light mod, by Gems::Owner
		std::string                                        gRustic;    // which Rustic Soul Gems download is loaded

		// What Light Placer's configs claim (models by their file name, formIDs tokens in lower case), read once when Light Placer
		// is loaded: a gem another config already lights is left to it, so no gem ever wears two lights (Emissive Soul Gems', CS
		// Light's, Subtle Soul Gems' ...)
		struct Claims
		{
			std::unordered_set<std::string> models, forms;
		};

		// a file's extension in lower case, as wide text: path::string() throws on a name the ANSI code page cannot show
		[[nodiscard]] std::wstring LowerExtension(const std::filesystem::path& a_path)
		{
			auto ext = a_path.extension().wstring();
			std::ranges::transform(ext, ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
			return ext;
		}

		[[nodiscard]] Claims ReadLightPlacerClaims()
		{
			Claims          out;
			std::error_code ec;
			const auto      root = std::filesystem::current_path() / "Data" / "LightPlacer";
			std::size_t     files = 0;
			for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
				if (!it->is_regular_file(ec) || LowerExtension(it->path()) != L".json") {
					continue;
				}
				std::ifstream in(it->path(), std::ios::binary);
				const auto    doc = nlohmann::json::parse(in, nullptr, false, true);  // no exceptions; a comment does not drop the file
				++files;
				if (!doc.is_array()) {
					continue;
				}
				for (const auto& entry : doc) {
					if (!entry.is_object()) {
						continue;
					}
					for (const auto& [key, into] : { std::pair{ "models", &out.models }, std::pair{ "formIDs", &out.forms } }) {
						const auto found = entry.find(key);
						if (found == entry.end() || !found->is_array()) {
							continue;
						}
						for (const auto& v : *found) {
							if (v.is_string()) {
								into->insert(key == std::string_view("models") ? Gems::NormalPath(v.get<std::string>()) : Lower(v.get<std::string>()));
							}
						}
					}
				}
			}
			SKSE::log::info("Light Placer is loaded: {} config file(s) read, {} model(s) and {} form(s) claimed", files, out.models.size(), out.forms.size());
			return out;
		}

		// does a formIDs token name this form ("SoulGemGrandFilled", "0x2E4FF~Skyrim.esm", "0002E4FF~skyrim.esm" ...)
		[[nodiscard]] bool Claimed(const Claims& a_claims, const RE::TESForm* a_form, std::string_view a_model)
		{
			if (a_claims.models.contains(Gems::NormalPath(a_model))) {
				return true;
			}
			if (const char* id = a_form->GetFormEditorID(); id && *id && a_claims.forms.contains(Lower(id))) {
				return true;
			}
			const auto* file = a_form->GetFile(0);
			if (!file) {
				return false;
			}
			const auto plugin = Lower(file->GetFilename());
			const auto local = a_form->GetLocalFormID();
			return std::ranges::any_of(a_claims.forms, [&](const std::string& a_token) { return Gems::TokenNamesForm(a_token, plugin, local); });
		}

		[[nodiscard]] Gems::Kind KindOfCapacity(RE::SOUL_LEVEL a_level) noexcept
		{
			switch (a_level) {
			case RE::SOUL_LEVEL::kPetty:
				return Gems::Kind::kPetty;
			case RE::SOUL_LEVEL::kLesser:
				return Gems::Kind::kLesser;
			case RE::SOUL_LEVEL::kCommon:
				return Gems::Kind::kCommon;
			case RE::SOUL_LEVEL::kGreater:
				return Gems::Kind::kGreater;
			default:
				return Gems::Kind::kGrand;
			}
		}

		[[nodiscard]] bool Loaded(RE::TESDataHandler* a_dh, std::string_view a_plugin)
		{
			return a_dh->LookupLoadedModByName(a_plugin) || a_dh->LookupLoadedLightModByName(a_plugin);
		}

		// the other light mods in this game, read once at data load
		struct Others
		{
			bool   lightPlacer{ false }, reLight{ false };
			Claims claims;
		};

		// a gem this plugin lights is kept; one another light mod lights is counted and left to it (Gems::OwnerOf)
		bool Take(const RE::TESBoundObject* a_form, std::string_view a_model, const Gem& a_gem, const Others& a_others)
		{
			const bool claimed = a_others.lightPlacer && Claimed(a_others.claims, a_form, a_model);
			const auto owner = Gems::OwnerOf(a_model, a_gem.look.family, a_others.lightPlacer, claimed, a_others.reLight);
			if (owner != Gems::Owner::kThisMod) {
				++gLeftTo[static_cast<std::size_t>(owner)];
				return false;
			}
			gGems.insert_or_assign(a_form, a_gem);
			return true;
		}

		// items and pieces that are not soul gem records, known by their own model: a_items lets an item take a named gem's model
		// (the Soul Essence Gem); the Soul Cairn's pieces are an activator, a misc item and a furniture, never a static - a static
		// wearing one of their models is decor (a display)
		template <class T>
		std::size_t AddByModel(bool a_items, const Others& a_others)
		{
			std::size_t added = 0;
			auto*       dh = RE::TESDataHandler::GetSingleton();
			// the plain form list, never GetFormArray<T>(): that one reinterprets the game's TESForm pointers as T*, which is wrong
			// for a class whose form part does not sit at its start - a movable static's sits 0x20 in (BGSMovableStatic.h), and its
			// typed list crashed the game at data load on 2026-10-19 (a call through a vtable read 0x20 off)
			for (auto* form : dh->GetFormArray(T::FORMTYPE)) {
				auto*       bound = form ? form->template As<RE::TESBoundObject>() : nullptr;
				const auto* model = bound ? form->template As<RE::TESModel>() : nullptr;
				const char* path = model ? model->GetModel() : nullptr;
				const auto  look = path && *path ? Gems::LookOfModel(path, false) : std::nullopt;
				if (!look || (look->family != Gems::Family::kSoulCairn && !(a_items && look->family == Gems::Family::kNamed))) {
					continue;
				}
				added += Take(bound, path, Gem{ *look, false }, a_others) ? 1 : 0;
			}
			return added;
		}

		// displays (statics wearing a gem's or a Soul Cairn piece's model, or a copy RE::Light's keys reach) that RE::Light lights:
		// his answer to checklist 138, "make our mod turn off displays" - with RE::Light loaded their light is put out (displays never
		// get lit)
		std::unordered_set<const RE::TESBoundObject*> gDisplays;
		std::atomic<std::size_t>                      gDisplaysDark{ 0 };

		template <class T>
		std::size_t AddDisplays()
		{
			std::size_t added = 0;
			for (auto* form : RE::TESDataHandler::GetSingleton()->GetFormArray(T::FORMTYPE)) {
				auto*       bound = form ? form->template As<RE::TESBoundObject>() : nullptr;
				const auto* model = bound ? form->template As<RE::TESModel>() : nullptr;
				const char* path = model ? model->GetModel() : nullptr;
				const bool  gemModel = path && *path && (Gems::LookOfModel(path, false) || Gems::ReLightKeyed(path));
				if (gemModel && !gGems.contains(bound)) {
					added += gDisplays.insert(bound).second ? 1 : 0;
				}
			}
			return added;
		}

		// every light on a display's model that is not ours goes out (culled, so nothing it would light is lit); true when one was
		[[nodiscard]] bool DarkenDisplay(RE::NiAVObject* a_root)
		{
			bool dark = false;
			RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
				if (netimmerse_cast<RE::NiLight*>(a_object) && a_object->name != kName) {
					if (!a_object->GetAppCulled()) {
						a_object->SetAppCulled(true);
					}
					dark = true;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return dark;
		}

		// ---------------------------------------------------------- the lights, main thread only
		struct Kept
		{
			RE::NiPointer<RE::NiNode>       root;  // the gem's model
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      bs;
			const Gem*                      gem{ nullptr };
			float                           phase{ 0.0f };  // where its pulse starts, so gems side by side do not breathe as one
			bool                            seen{ false };
		};
		std::unordered_map<RE::FormID, Kept> gKept;  // by the placed gem's form id
		// a gem's model carries a light that shines here - looked at once per gem RECORD (its model is the record's): never by a placed
		// gem's form id, which the game hands to the next gem placed after one is deleted (2026-10-19's check: two Creation Club gems
		// on the plain models were left dark as "own light" - the verdict of the Praedy's gem placed before them under the same id)
		std::unordered_map<const RE::TESBoundObject*, bool> gOwnLight;
		// a gem whose light could not be made or registered is tried again only after kRetryAfter (review 2026-10-19: it was
		// re-made four times a second, silently) - by the placed gem AND its model, so a new gem that gets the same form id or a
		// model at the same address (both are reused) is never held back by another gem's failure
		struct Retry
		{
			const RE::NiNode*                     root{ nullptr };
			std::chrono::steady_clock::time_point at;
		};
		std::unordered_map<RE::FormID, Retry> gRetryAt;
		std::unordered_set<std::string>       gParticleTextures;  // the textures Data\ParticleLights\*.ini name (lower case, no extension)
		std::atomic<std::size_t>              gCount{ 0 }, gMade{ 0 }, gOwnLightNow{ 0 };
		float                                 gClock = 0.0f, gSinceScan = kScanEvery;

		void Drop(Kept& a_kept, RE::ShadowSceneNode* a_scene)
		{
			if (a_scene && a_kept.bs) {
				a_scene->RemoveLight(a_kept.bs);
			}
			if (a_kept.light && a_kept.light->parent) {
				a_kept.light->parent->DetachChild(a_kept.light.get());
			}
			a_kept = {};
		}

		void DropAll(RE::ShadowSceneNode* a_scene)
		{
			for (auto& [id, kept] : gKept) {
				Drop(kept, a_scene);
			}
			gKept.clear();
			gCount = 0;
		}

		// this frame's numbers onto the light
		void Follow(Kept& a_kept, const Gems::Settings& a_s)
		{
			const auto& look = a_kept.gem->look;
			const float pulse = a_s.pulse ? Gems::PulseAt(gClock + a_kept.phase) : 1.0f;
			const auto  shown = Gems::ShownFor(Gems::kStrength[static_cast<std::size_t>(look.kind)], InverseSquare(),
				a_s.brightness / 100.0f, a_s.reach / 100.0f, pulse);
			auto&       data = a_kept.light->GetLightRuntimeData();
			data.diffuse = { look.rgb[0] / 255.0f, look.rgb[1] / 255.0f, look.rgb[2] / 255.0f };
			data.fade = shown.fade;
			if (data.radius.x != shown.radius) {
				data.radius = { shown.radius, shown.radius, Gems::kGemSize };
				a_kept.light->SetLightAttenuation(shown.radius);
			}
			if (InverseSquare()) {
				Isl::SetOwn(a_kept.light.get(), shown.cutoff);  // after SetLightAttenuation, which writes the same words
			} else {
				data.ambient = { data.diffuse.red * kAmbient, data.diffuse.green * kAmbient, data.diffuse.blue * kAmbient };
			}
		}

		[[nodiscard]] bool Make(Kept& a_kept, RE::NiNode* a_root, RE::ShadowSceneNode* a_scene, const Gems::Settings& a_s)
		{
			auto* light = CloneLight();
			if (!light) {
				return false;
			}
			light->name = kName;
			a_root->AttachChild(light, true);
			a_kept.light.reset(light);
			a_kept.root.reset(a_root);
			Follow(a_kept, a_s);
			RE::NiUpdateData update{};
			light->Update(update);
			auto* bs = AddToScene(a_scene, light);
			if (!bs) {
				a_root->DetachChild(light);
				a_kept = {};
				return false;
			}
			a_kept.bs.reset(bs);
			++gMade;
			return true;
		}

		// the textures an ENB particle light draws (ENB Light's, Rudy's): the names Light Limit Fix and Bottled Shaders ship particle
		// light configs for
		constexpr std::array<std::string_view, 3> kEnbLightTextures{ "fxglowenb", "glowsoft01_enbl", "enblightglow" };

		// the file name of the texture an effect-shader shape draws, lower case without folder or extension ("" for any other shape)
		[[nodiscard]] std::string EffectTexture(RE::NiAVObject* a_object)
		{
			auto* geometry = a_object ? a_object->AsGeometry() : nullptr;
			auto* shader = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			if (!shader || shader->GetMaterialType() != RE::BSShaderMaterial::Type::kEffect || !shader->material) {
				return {};
			}
			const char* path = static_cast<RE::BSEffectShaderMaterial*>(shader->material)->sourceTexturePath.c_str();
			auto        name = Gems::BareName(path ? path : "");
			if (const auto dot = name.rfind('.'); dot != std::string::npos) {
				name.erase(dot);
			}
			return name;
		}

		// does this placed gem's model already hold a light that is not ours (one Light Placer or RE::Light hung on it, or one inside
		// the mesh): looked at every scan, so a light another mod adds later still wins - ours is let go the same quarter second
		[[nodiscard]] bool HoldsOtherLight(RE::NiAVObject* a_root)
		{
			bool found = false;
			RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
				if (netimmerse_cast<RE::NiLight*>(a_object) && a_object->name != kName) {
					found = true;
					return RE::BSVisit::BSVisitControl::kStop;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return found;
		}

		// does this placed gem's model carry a light of its own that shines in this game: on ENB an ENB particle light (a shape named
		// "EnbParticleLight..." or one drawing an ENB light texture - fxglowENB, glowsoft01_enbl ...); on Community Shaders a shape whose
		// texture a particle-light config names. The game's own lighting draws neither
		[[nodiscard]] bool CarriesOwnLight(const RE::TESBoundObject* a_base, RE::NiAVObject* a_root, Lighting a_lighting)
		{
			const auto [it, added] = gOwnLight.try_emplace(a_base, false);
			if (added) {
				RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
					const auto texture = EffectTexture(a_object);
					const bool particle = !texture.empty() && gParticleTextures.contains(texture);
					const bool enb = a_lighting == Lighting::kEnb &&
					                 (particle || std::ranges::find(kEnbLightTextures, texture) != kEnbLightTextures.end() || (a_object && Lower(a_object->name.c_str()).starts_with("enbparticlelight")));
					if (enb || (a_lighting == Lighting::kShaders && particle)) {
						it->second = true;
						return RE::BSVisit::BSVisitControl::kStop;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			return it->second;
		}

		void Keep(RE::FormID a_id, RE::NiNode* a_root, const Gem& a_gem, RE::ShadowSceneNode* a_scene, const Gems::Settings& a_s)
		{
			auto& kept = gKept[a_id];
			if (kept.light && (kept.root.get() != a_root || kept.light->parent != a_root)) {
				Drop(kept, a_scene);  // the gem's model was rebuilt: a new light
			}
			kept.gem = &a_gem;
			if (!kept.light) {
				const auto now = std::chrono::steady_clock::now();
				if (const auto retry = gRetryAt.find(a_id); retry != gRetryAt.end() && retry->second.root == a_root && now < retry->second.at) {
					return;  // not seen: let go below
				}
				kept.phase = static_cast<float>(a_id % 997u) / 997.0f * Gems::kPulseSeconds;
				if (!Make(kept, a_root, a_scene, a_s)) {
					if (gRetryAt.size() > 256) {
						gRetryAt.clear();  // gems long gone
					}
					gRetryAt[a_id] = { a_root, now + kRetryAfter };
					static bool warned = false;
					if (!std::exchange(warned, true)) {
						SKSE::log::warn("a soul gem light could not be made or registered; such a gem is tried again every {} s", kRetryAfter.count());
					}
					return;  // not seen: let go below
				}
				gRetryAt.erase(a_id);
			}
			kept.seen = true;
		}

		void Scan(RE::ShadowSceneNode* a_scene, const Gems::Settings& a_s)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* tes = RE::TES::GetSingleton();
			if (!player || !tes || !player->GetParentCell()) {
				return;
			}
			for (auto& [id, kept] : gKept) {
				kept.seen = false;
			}
			const auto  lighting = LightingFound();
			const bool  ownLights = lighting == Lighting::kEnb || (lighting == Lighting::kShaders && !gParticleTextures.empty());
			std::size_t leftToOwn = 0, displaysDark = 0;
			struct Candidate
			{
				RE::FormID  id;
				RE::NiNode* root;
				const Gem*  gem;
				float       distance;  // squared, to the player
			};
			std::vector<Candidate> candidates;
			const auto             here = player->GetPosition();
			tes->ForEachReferenceInRange(player, kRange, [&](RE::TESObjectREFR* a_ref) {
				const auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				if (base && !gDisplays.empty() && gDisplays.contains(base) && a_ref->Is3DLoaded()) {
					if (auto* shown = a_ref->Get3D(); shown && DarkenDisplay(shown)) {
						++displaysDark;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto it = base ? gGems.find(base) : gGems.end();
				if (it == gGems.end() || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Is3DLoaded()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (it->second.needsSoul && a_ref->extraList.GetSoulLevel() == RE::SOUL_LEVEL::kNone) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto*      model = a_ref->Get3D();
				auto*      root = model ? model->AsNode() : nullptr;
				const auto id = a_ref->GetFormID();
				if (!root) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if ((ownLights && CarriesOwnLight(base, root, lighting)) || HoldsOtherLight(root)) {
					++leftToOwn;  // another light shines on it here: left to that light, never a second one
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// a gem that holds our light already counts as a fifth nearer, so two gems at the kMaxLights edge do not swap lights
				// four times a second as she walks (the code review 2026-10-19)
				const auto held = gKept.find(id);
				const bool lit = held != gKept.end() && held->second.light && held->second.root.get() == root;
				candidates.push_back({ id, root, &it->second, a_ref->GetPosition().GetSquaredDistance(here) * (lit ? 0.64f : 1.0f) });
				return RE::BSContainer::ForEachResult::kContinue;
			});
			gOwnLightNow = leftToOwn;
			gDisplaysDark = displaysDark;
			// the nearest kMaxLights gems keep a light: a shop, a stash or the Soul Cairn would otherwise hang one on every gem in reach
			// (review 2026-10-19); a farther gem is let go below and takes a light again when it is among the nearest
			if (candidates.size() > kMaxLights) {
				std::ranges::nth_element(candidates, candidates.begin() + kMaxLights, {}, &Candidate::distance);
				candidates.resize(kMaxLights);
			}
			for (const auto& c : candidates) {
				Keep(c.id, c.root, *c.gem, a_scene, a_s);
			}
			std::erase_if(gKept, [&](auto& a_entry) {
				if (!a_entry.second.seen) {
					Drop(a_entry.second, a_scene);
					return true;
				}
				return false;
			});
		}

		void Frame(float a_delta)
		{
			auto*      scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			const auto s = SettingsNow();
			if (!scene || !s.on) {
				DropAll(scene);
				gOwnLightNow = 0;
				return;
			}
			gClock = std::fmod(gClock + (std::max)(a_delta, 0.0f), 3600.0f);
			gSinceScan += (std::max)(a_delta, 0.0f);
			if (gSinceScan >= kScanEvery) {
				gSinceScan = 0.0f;
				Scan(scene, s);
			}
			std::erase_if(gKept, [&](auto& a_entry) {
				auto& kept = a_entry.second;
				if (!kept.light || !kept.root || !kept.root->parent) {  // picked up or unloaded since the last look
					Drop(kept, scene);
					return true;
				}
				Follow(kept, s);
				return false;
			});
			gCount = gKept.size();
		}

		// Actor::Update on the player: our work once a frame, on the main thread, after the game's own (Illuminated's hook)
		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				Guarded("player update", [a_delta]() { Frame(a_delta); });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void FindGems()
	{
		gGems.clear();
		gFamilies.clear();
		gLeftTo = {};
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			SKSE::log::error("gems: no data handler - nothing is lit");
			return;
		}
		const bool sorted = Loaded(dh, "RUSTIC SOULGEMS - Sorted.esp"), unsorted = Loaded(dh, "RUSTIC SOULGEMS - Unsorted.esp");
		gRustic = sorted && unsorted ? "both" : sorted ? "Sorted" :
		                                    unsorted   ? "Unsorted" :
		                                                 "";
		// the particle-light configs (Data\ParticleLights\<texture>.ini): a Community Shaders that draws particle lights (Bottled
		// Shaders, Community Shaders Expanded) lights every shape drawing such a texture itself
		gParticleTextures.clear();
		std::error_code ec;
		for (std::filesystem::directory_iterator it(std::filesystem::current_path() / "Data" / "ParticleLights", ec), end; !ec && it != end; it.increment(ec)) {
			if (LowerExtension(it->path()) == L".ini") {
				std::string stem;  // texture names are plain ASCII; path::string() throws on a name the ANSI code page cannot show
				for (const wchar_t c : it->path().stem().wstring()) {
					stem.push_back(c < 128 ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '?');
				}
				gParticleTextures.insert(std::move(stem));
			}
		}
		Others others;
		others.lightPlacer = REX::W32::GetModuleHandleA("po3_LightPlacer.dll") != nullptr;
		others.reLight = REX::W32::GetModuleHandleA("Relight.dll") != nullptr;  // RE::Light (ReLight)
		if (others.lightPlacer) {
			others.claims = ReadLightPlacerClaims();
		}
		const auto* azurasStar = dh->LookupForm(kAzurasStar, "Skyrim.esm");
		const auto* blackStar = dh->LookupForm(kBlackStar, "Skyrim.esm");
		std::size_t records = 0, lit = 0;
		for (auto* gem : dh->GetFormArray<RE::TESSoulGem>()) {
			if (!gem) {
				continue;
			}
			++records;
			const bool  star = gem == azurasStar || gem == blackStar;
			const bool  holds = gem->GetContainedSoul() != RE::SOUL_LEVEL::kNone;
			const char* path = gem->GetModel();
			const auto  model = std::string_view(path ? path : "");
			auto        look = model.empty() ? std::nullopt : Gems::LookOfModel(model, !gRustic.empty());
			if (star) {
				look = Gems::Look{ Gems::Family::kStar, Gems::Kind::kStar, gem == azurasStar ? Gems::kAzurasStarRgb : Gems::kBlackStarRgb };
			} else if (!look || look->family == Gems::Family::kSoulCairn) {
				look = Gems::GameLook(KindOfCapacity(gem->GetMaximumCapacity()), gem->CanHoldNPCSoul());
			}
			const bool always = star || holds || look->family == Gems::Family::kNamed;
			if (Take(gem, model, Gem{ *look, !always }, others) && always) {
				++lit;
				const auto name = Gems::FamilyName(look->family);
				const auto it = std::ranges::find(gFamilies, name, &std::pair<std::string, std::size_t>::first);
				if (it == gFamilies.end()) {
					gFamilies.emplace_back(std::string(name), 1);
				} else {
					++it->second;
				}
			}
		}
		const std::size_t pieces = AddByModel<RE::TESObjectMISC>(true, others) + AddByModel<RE::IngredientItem>(true, others) +
		                           AddByModel<RE::TESObjectACTI>(false, others) + AddByModel<RE::TESFurniture>(false, others);
		gDisplays.clear();
		if (others.reLight) {
			const auto displays = AddDisplays<RE::TESObjectSTAT>() + AddDisplays<RE::BGSMovableStatic>();
			SKSE::log::info("gems: {} display(s) wearing a gem's model - an RE::Light light on one is put out (displays never get lit)", displays);
		}
		std::string list;
		for (const auto& [name, n] : gFamilies) {
			list += std::format("{}{} {}", list.empty() ? "" : ", ", n, name);
		}
		using enum Gems::Owner;
		SKSE::log::info(
			"gems: {} soul gem record(s), {} lit always ({}), the rest when a placed gem holds a soul; {} other piece(s) by their "
			"model; Rustic Soul Gems {}; left to Emissive Soul Gems' models {}, to other Light Placer configs {}, to RE::Light {} "
			"(Light Placer {}, RE::Light {}); {} particle-light config(s) - a gem whose own model's light shines here is left to it",
			records, lit, list.empty() ? "none" : list, pieces, gRustic.empty() ? "not loaded" : gRustic,
			gLeftTo[static_cast<std::size_t>(kEmissive)], gLeftTo[static_cast<std::size_t>(kLightPlacer)],
			gLeftTo[static_cast<std::size_t>(kReLight)], others.lightPlacer ? "loaded" : "not loaded", others.reLight ? "loaded" : "not loaded",
			gParticleTextures.size());
	}

	void InstallFrame()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
		PlayerUpdate::func = vtbl.write_vfunc(REL::Relocate(0xAD, 0xAD, 0xAF), PlayerUpdate::thunk);  // Actor::Update
		SKSE::log::info("lights follow the player's update (Actor::Update)");
	}

	void DropGemLights()
	{
		DropAll(RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]);
		gOwnLight.clear();  // looked at again in the next game
		gRetryAt.clear();
		gSinceScan = kScanEvery;  // look around at once in the next game
	}

	void ApplyNow()
	{
		gSinceScan = kScanEvery;  // the next frame looks around again and draws every light with the settings as they are now
	}

	std::size_t GemLightCount() { return gCount.load(); }
	std::size_t GemLightsMade() { return gMade.load(); }

	std::vector<std::pair<std::string, std::size_t>> GemFamilies() { return gFamilies; }
	std::array<std::size_t, 4>                       GemsLeftTo() { return gLeftTo; }
	std::size_t                                      GemsLeftToOwnLight() { return gOwnLightNow.load(); }
	std::string                                      RusticFound() { return gRustic; }

	std::string GemsReport()
	{
		std::string lights;
		for (const auto& [id, kept] : gKept) {
			if (!kept.light || !kept.gem) {
				continue;
			}
			const auto& d = kept.light->GetLightRuntimeData();
			const auto& p = kept.light->world.translate;
			lights += std::format(R"({}{{"ref":"0x{:08X}","family":"{}","kind":{},"rgb":[{},{},{}],"fade":{:.4f},"radius":{:.1f},"at":[{:.0f},{:.0f},{:.0f}]}})",
				lights.empty() ? "" : ",", id, Gems::FamilyName(kept.gem->look.family), static_cast<int>(kept.gem->look.kind),
				kept.gem->look.rgb[0], kept.gem->look.rgb[1], kept.gem->look.rgb[2], d.fade, d.radius.x, p.x, p.y, p.z);
		}
		std::string families;
		for (const auto& [name, n] : gFamilies) {
			families += std::format(R"({}"{}":{})", families.empty() ? "" : ",", name, n);
		}
		return std::format(
			R"({{"gemForms":{},"litByLook":{{{}}},"rustic":"{}","leftTo":{{"emissiveSoulGems":{},"lightPlacer":{},"reLight":{},"ownLightNearYou":{}}},"particleLightConfigs":{},"displays":{},"displaysDarkNearYou":{},"lightsNow":{},"lightsMade":{},"lights":[{}]}})",
			gGems.size(), families, gRustic, gLeftTo[1], gLeftTo[2], gLeftTo[3], gOwnLightNow.load(), gParticleTextures.size(), gDisplays.size(),
			gDisplaysDark.load(), gKept.size(), gMade.load(), lights);
	}
}
