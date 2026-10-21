#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>
#include <map>
#include <unordered_set>
#include <utility>

#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

#include <ModdingAPI/API.hpp>

using namespace RC;
using namespace RC::Unreal;

class SkinBrowserMod : public CppUserModBase
{
private:
    std::vector<ModdingAPI::SkinModEntry> m_skins;

    // className -> currently active skin label for that class
    std::unordered_map<std::wstring, std::wstring> m_active_skin_per_class;

    // className -> the assets we last set for that class's skin
    std::unordered_map<std::wstring, std::vector<UObject*>> m_expected_assets_per_class;

    // (component, slot) -> the original asset
    std::map<std::pair<UObject*, int32_t>, UObject*> m_original_asset_cache;

    // "className|label" of skins whose assets failed to load
    std::unordered_set<std::wstring> m_failed_skin_keys;

    // Class name of the vehicle the player is piloting right now
    std::wstring m_piloted_class;

    std::chrono::steady_clock::time_point m_last_watchdog_tick{};

public:
    SkinBrowserMod() : CppUserModBase()
    {
        ModName = STR("SkinBrowserMod");
        ModVersion = STR("1.1");
        ModDescription = STR("ImGui browser for vehicle skins - loads any mod's skinmod.json, with toggle + a watchdog that keeps reapplying against the game's own periodic mesh resync");
        ModAuthors = STR("You");

        register_tab(STR("Skin Browser"), [](CppUserModBase* instance) {
            auto* mod = dynamic_cast<SkinBrowserMod*>(instance);
            if (!mod) { return; }
            mod->draw_ui();
        });
    }

    ~SkinBrowserMod() override = default;

    auto on_ui_init() -> void override
    {
        UE4SS_ENABLE_IMGUI()
        refresh_skins();
        m_active_skin_per_class = ModdingAPI::LoadSkinPreferences();
        m_last_watchdog_tick = std::chrono::steady_clock::now();
    }

    auto on_update() -> void override
    {

        using namespace std::chrono;
        auto now = steady_clock::now();
        if (now - m_last_watchdog_tick < milliseconds(500))
        {
            return;
        }
        m_last_watchdog_tick = now;

        UObject* live_vehicle = find_player_vehicle();
        m_piloted_class.clear();
        if (live_vehicle)
        {
            if (UClass* live_class = live_vehicle->GetClassPrivate())
            {
                m_piloted_class = live_class->GetNamePrivate().ToString();
            }
        }

        watchdog_reapply_active_skin(live_vehicle);
    }

    auto refresh_skins() -> void
    {
        m_skins = ModdingAPI::ScanForSkinMods();
        m_failed_skin_keys.clear();
    }

    auto draw_ui() -> void
    {
        static bool window_open = true;

        ImGui::Begin("Skin Browser (floating)", &window_open);

        if (ImGui::Button("Refresh"))
        {
            refresh_skins();
        }

        ImGui::SameLine();
        std::string piloted_utf8(m_piloted_class.begin(), m_piloted_class.end());
        ImGui::TextDisabled("Piloting: %s", m_piloted_class.empty() ? "(nothing)" : piloted_utf8.c_str());

        ImGui::Separator();

        if (m_skins.empty())
        {
            ImGui::Text("No skin mods found. Drop a mod folder (with skinmod.json) into Content/Paks/~mods and hit Refresh.");
        }
        else
        {
            ImGui::Text("Click a skin to toggle it on/off for its vehicle class:");
            ImGui::Separator();

            std::wstring last_mod;
            int skin_index = 0;
            for (auto& skin : m_skins)
            {

                ImGui::PushID(skin_index++);

                if (skin.sourceModName != last_mod)
                {
                    std::string mod_name_utf8(skin.sourceModName.begin(), skin.sourceModName.end());
                    ImGui::TextDisabled("%s", mod_name_utf8.c_str());
                    last_mod = skin.sourceModName;
                }

                bool is_active = is_skin_active(skin);
                std::string label_utf8(skin.label.begin(), skin.label.end());

                if (is_active)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
                }

                if (ImGui::Button(label_utf8.c_str()))
                {
                    toggle_skin(skin);
                }

                if (is_active)
                {
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(active)");
                }

                ImGui::SameLine();
                std::string class_utf8(skin.className.begin(), skin.className.end());
                ImGui::TextDisabled("[%s]", class_utf8.c_str());

                ImGui::PopID();
            }
        }

        ImGui::End();
    }

    auto is_skin_active(const ModdingAPI::SkinModEntry& skin) -> bool
    {
        auto it = m_active_skin_per_class.find(skin.className);
        return it != m_active_skin_per_class.end() && it->second == skin.label;
    }

    auto toggle_skin(const ModdingAPI::SkinModEntry& skin) -> void
    {
        bool currently_active = is_skin_active(skin);
        m_failed_skin_keys.clear();

        if (currently_active)
        {
            m_active_skin_per_class.erase(skin.className);
            m_expected_assets_per_class.erase(skin.className);
            ModdingAPI::SaveSkinPreference(skin.className, L"");

            UObject* live_vehicle = find_matching_live_vehicle(skin.className);
            if (live_vehicle)
            {
                revert_skin(live_vehicle, skin);
            }
            return;
        }

        m_active_skin_per_class[skin.className] = skin.label;
        ModdingAPI::SaveSkinPreference(skin.className, skin.label);

        UObject* live_vehicle = find_matching_live_vehicle(skin.className);
        if (live_vehicle)
        {
            apply_skin_to_vehicle(live_vehicle, skin);
        }
        else
        {
            std::wstring msg = L"[SkinBrowser] " + skin.className +
                L" isn't currently piloted - the skin is saved and will apply automatically once you're in one\n";
            Output::send(msg.c_str());
        }


    }

    auto find_player_vehicle() -> UObject*
    {
        UObject* found_pawn = nullptr;
        std::wstring target = STR("PC_Vehicle_C");

        UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) -> LoopAction {
            if (!object) { return LoopAction::Continue; }
            auto* obj_class = object->GetClassPrivate();
            if (!obj_class) { return LoopAction::Continue; }

            if (obj_class->GetNamePrivate().ToString() != target) { return LoopAction::Continue; }
            if (object->HasAnyFlags(RF_ClassDefaultObject)) { return LoopAction::Continue; }

            void* pawn_slot = object->GetValuePtrByPropertyNameInChain(STR("Pawn"));
            if (!pawn_slot) { return LoopAction::Continue; }

            UObject* pawn_value = *static_cast<UObject**>(pawn_slot);
            if (!pawn_value) { return LoopAction::Continue; }

            found_pawn = pawn_value;
            return LoopAction::Break;
        });

        return found_pawn;
    }


    auto find_matching_live_vehicle(const std::wstring& class_name) -> UObject*
    {
        UObject* live_vehicle = find_player_vehicle();
        if (!live_vehicle) { return nullptr; }

        UClass* vehicle_class = live_vehicle->GetClassPrivate();
        if (!vehicle_class) { return nullptr; }

        if (vehicle_class->GetNamePrivate().ToString() != class_name) { return nullptr; }

        return live_vehicle;
    }

    auto find_skin_entry(const std::wstring& class_name, const std::wstring& label) -> const ModdingAPI::SkinModEntry*
    {
        for (auto& skin : m_skins)
        {
            if (skin.className == class_name && skin.label == label)
            {
                return &skin;
            }
        }
        return nullptr;
    }

    static auto skin_key(const ModdingAPI::SkinModEntry& skin) -> std::wstring
    {
        return skin.className + L"|" + skin.label;
    }


    static auto skin_target_count(const ModdingAPI::SkinModEntry& skin) -> size_t
    {
        return skin.type == L"material" ? skin.materials.size() : 1;
    }


    auto watchdog_reapply_active_skin(UObject* live_vehicle) -> void
    {
        if (m_active_skin_per_class.empty() || !live_vehicle) { return; }

        UClass* vehicle_class = live_vehicle->GetClassPrivate();
        if (!vehicle_class) { return; }

        std::wstring class_name = vehicle_class->GetNamePrivate().ToString();

        auto active_it = m_active_skin_per_class.find(class_name);
        if (active_it == m_active_skin_per_class.end()) { return; }

        const ModdingAPI::SkinModEntry* skin = find_skin_entry(class_name, active_it->second);
        if (!skin) { return; }
        if (m_failed_skin_keys.contains(skin_key(*skin))) { return; }

        void* visual_mesh_slot = live_vehicle->GetValuePtrByPropertyNameInChain(STR("VisualMesh"));
        if (!visual_mesh_slot) { return; }

        UObject* visual_mesh_component = *static_cast<UObject**>(visual_mesh_slot);
        if (!visual_mesh_component) { return; }

        auto expected_it = m_expected_assets_per_class.find(class_name);

        bool needs_apply = (expected_it == m_expected_assets_per_class.end())
            || (expected_it->second.size() != skin_target_count(*skin));

        if (!needs_apply)
        {
            const auto& expected = expected_it->second;
            if (skin->type == L"material")
            {
                for (size_t i = 0; i < skin->materials.size(); ++i)
                {
                    if (get_slot_asset(visual_mesh_component, true, skin->materials[i].slotIndex) != expected[i])
                    {
                        needs_apply = true;
                        break;
                    }
                }
            }
            else
            {
                needs_apply = get_slot_asset(visual_mesh_component, false, -1) != expected[0];
            }
        }

        if (needs_apply)
        {
            Output::send(STR("[SkinBrowser] Watchdog: mesh/material was reset externally, reapplying\n"));
            apply_skin_to_vehicle(live_vehicle, *skin);
        }
    }

    auto apply_skin_to_vehicle(UObject* vehicle, const ModdingAPI::SkinModEntry& skin) -> void
    {
        const bool is_material = (skin.type == L"material");

        std::vector<std::pair<int32_t, UObject*>> targets;
        if (is_material)
        {
            for (const auto& m : skin.materials)
            {
                UObject* asset = ModdingAPI::LoadAsset(m.path);
                if (!asset)
                {
                    std::wstring msg = L"[SkinBrowser] Failed to load " + m.path + L" for " + skin.label + L"\n";
                    Output::send(msg.c_str());
                    m_failed_skin_keys.insert(skin_key(skin));
                    return;
                }
                targets.emplace_back(m.slotIndex, asset);
            }
        }
        else
        {
            UObject* asset = ModdingAPI::LoadAsset(skin.meshPath);
            if (!asset)
            {
                std::wstring msg = L"[SkinBrowser] Failed to load asset for " + skin.label + L"\n";
                Output::send(msg.c_str());
                m_failed_skin_keys.insert(skin_key(skin));
                return;
            }
            targets.emplace_back(-1, asset);
        }

        void* visual_mesh_slot = vehicle->GetValuePtrByPropertyNameInChain(STR("VisualMesh"));
        if (!visual_mesh_slot)
        {
            Output::send(STR("[SkinBrowser] Vehicle has no VisualMesh property\n"));
            return;
        }

        UObject* visual_mesh_component = *static_cast<UObject**>(visual_mesh_slot);
        if (!visual_mesh_component)
        {
            Output::send(STR("[SkinBrowser] VisualMesh property was null\n"));
            return;
        }

        if (is_material)
        {
            UFunction* set_material_fn = visual_mesh_component->GetFunctionByNameInChain(STR("SetMaterial"));
            if (!set_material_fn)
            {
                Output::send(STR("[SkinBrowser] Could not find SetMaterial function on VisualMesh component\n"));
                return;
            }

            for (const auto& [slot, asset] : targets)
            {
                cache_original_if_needed(visual_mesh_component, true, slot);
                set_material_in_slot(visual_mesh_component, set_material_fn, slot, asset);
            }
        }
        else
        {
            void* skeletal_mesh_slot = visual_mesh_component->GetValuePtrByPropertyNameInChain(STR("SkeletalMesh"));
            if (!skeletal_mesh_slot)
            {
                Output::send(STR("[SkinBrowser] Component has no SkeletalMesh property\n"));
                return;
            }


            cache_original_if_needed(visual_mesh_component, false, -1);
            *static_cast<UObject**>(skeletal_mesh_slot) = targets.front().second;
        }

        force_render_refresh(visual_mesh_component);

        std::vector<UObject*> expected;
        for (const auto& t : targets) { expected.push_back(t.second); }
        m_expected_assets_per_class[skin.className] = std::move(expected);

        std::wstring msg = L"[SkinBrowser] Applied " + std::wstring(is_material ? L"material" : L"mesh")
            + L" skin: " + skin.label;
        if (is_material) { msg += L" (" + std::to_wstring(targets.size()) + L" slot(s))"; }
        msg += L"\n";
        Output::send(msg.c_str());
    }

    auto revert_skin(UObject* vehicle, const ModdingAPI::SkinModEntry& skin) -> void
    {
        void* visual_mesh_slot = vehicle->GetValuePtrByPropertyNameInChain(STR("VisualMesh"));
        if (!visual_mesh_slot) { return; }

        UObject* visual_mesh_component = *static_cast<UObject**>(visual_mesh_slot);
        if (!visual_mesh_component) { return; }

        bool restored_any = false;

        if (skin.type == L"material")
        {
            UFunction* set_material_fn = visual_mesh_component->GetFunctionByNameInChain(STR("SetMaterial"));
            if (!set_material_fn) { return; }

            for (const auto& m : skin.materials)
            {
                auto cache_it = m_original_asset_cache.find({ visual_mesh_component, m.slotIndex });
                if (cache_it == m_original_asset_cache.end()) { continue; }

                set_material_in_slot(visual_mesh_component, set_material_fn, m.slotIndex, cache_it->second);
                m_original_asset_cache.erase(cache_it);
                restored_any = true;
            }
        }
        else
        {
            auto cache_it = m_original_asset_cache.find({ visual_mesh_component, -1 });
            if (cache_it != m_original_asset_cache.end())
            {
                void* skeletal_mesh_slot = visual_mesh_component->GetValuePtrByPropertyNameInChain(STR("SkeletalMesh"));
                if (skeletal_mesh_slot)
                {
                    *static_cast<UObject**>(skeletal_mesh_slot) = cache_it->second;
                    m_original_asset_cache.erase(cache_it);
                    restored_any = true;
                }
            }
        }

        if (!restored_any)
        {
            return;
        }

        force_render_refresh(visual_mesh_component);
        Output::send(STR("[SkinBrowser] Reverted skin\n"));
    }

    auto set_material_in_slot(UObject* component, UFunction* set_material_fn, int32_t slot, UObject* material) -> void
    {
        struct SetMaterialParms { int32_t ElementIndex; UObject* Material; };
        SetMaterialParms parms{ slot, material };
        component->ProcessEvent(set_material_fn, &parms);
    }

    auto get_slot_asset(UObject* component, bool is_material, int32_t slot) -> UObject*
    {
        if (is_material)
        {
            UFunction* get_material_fn = component->GetFunctionByNameInChain(STR("GetMaterial"));
            if (!get_material_fn) { return nullptr; }

            struct GetMaterialParms { int32_t ElementIndex; UObject* ReturnValue; };
            GetMaterialParms parms{ slot, nullptr };
            component->ProcessEvent(get_material_fn, &parms);
            return parms.ReturnValue;
        }

        void* skeletal_mesh_slot = component->GetValuePtrByPropertyNameInChain(STR("SkeletalMesh"));
        if (!skeletal_mesh_slot) { return nullptr; }
        return *static_cast<UObject**>(skeletal_mesh_slot);
    }

    auto cache_original_if_needed(UObject* component, bool is_material, int32_t slot) -> void
    {
        const auto key = std::make_pair(component, slot);
        if (m_original_asset_cache.contains(key)) { return; }
        m_original_asset_cache[key] = get_slot_asset(component, is_material, slot);
    }

    auto force_render_refresh(UObject* component) -> void
    {
        UFunction* set_visibility_fn = component->GetFunctionByNameInChain(STR("SetVisibility"));
        if (!set_visibility_fn)
        {
            Output::send(STR("[SkinBrowser] Could not find SetVisibility function\n"));
            return;
        }

        struct SetVisibilityParms { bool bNewVisibility; bool bPropagateToChildren; };

        SetVisibilityParms hide_parms{ false, true };
        component->ProcessEvent(set_visibility_fn, &hide_parms);

        SetVisibilityParms show_parms{ true, true };
        component->ProcessEvent(set_visibility_fn, &show_parms);
    }

};

#define SKIN_BROWSER_MOD_EXPORT __declspec(dllexport)
extern "C"
{
    SKIN_BROWSER_MOD_EXPORT RC::CppUserModBase* start_mod()
    {
        return new SkinBrowserMod();
    }

    SKIN_BROWSER_MOD_EXPORT void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}