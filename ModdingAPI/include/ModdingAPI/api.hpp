#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

#ifdef MODDING_API_EXPORTS
#define MODDING_API_EXPORT __declspec(dllexport)
#else
#define MODDING_API_EXPORT __declspec(dllimport)
#endif

#include <Unreal/UObject.hpp>

namespace ModdingAPI
{
    // loads an asset by object path
    // tries the asset registry first, then falls back to StaticLoadObjectInternal (after boot mounting)
    MODDING_API_EXPORT RC::Unreal::UObject* LoadAsset(const std::wstring& objectPath);


    struct SkinMaterialSlot
    {
        int32_t slotIndex = -1;
        std::wstring path; // material or material instance
    };


    struct SkinModEntry
    {
        std::wstring label;
        std::wstring className; // vehicle blueprint class, e.g. BP_Stealth_C

        // "mesh": the skeletal mesh to swap in
        // "material": copy of materials[0].path
        std::wstring meshPath;

        // "mesh" (default) or "material"
        std::wstring type = L"mesh";

        // Material skins only. Comes from the manifest's "materials" array
        std::vector<SkinMaterialSlot> materials;

        // Copy of materials[0].slotIndex. -1 if unset
        int32_t slotIndex = -1;

        // Folder the manifest was found in
        std::wstring sourceModName;
    };


    // Finds every skinmod.json under Content/Paks/~mods  and returns all the skins in them
    MODDING_API_EXPORT std::vector<SkinModEntry> ScanForSkinMods();


    // memory
    MODDING_API_EXPORT void SaveSkinPreference(const std::wstring& className, const std::wstring& skinLabel);
    MODDING_API_EXPORT std::unordered_map<std::wstring, std::wstring> LoadSkinPreferences();
}