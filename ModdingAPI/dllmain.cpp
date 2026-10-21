#include <cwchar>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>

#define NOMINMAX
#if PLATFORM_WINDOWS
#include <Windows.h>
#endif

#include <nlohmann/json.hpp>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/NameTypes.hpp>

#include <ModdingAPI/API.hpp>

using namespace RC;
using namespace RC::Unreal;


using StaticLoadObjectInternalFunc = UObject*(*)(
    UClass*, UObject*, const wchar_t*, const wchar_t*,
    uint32_t, void*, bool, const void*);

static StaticLoadObjectInternalFunc g_static_load_object_internal = nullptr;

static void* find_static_load_object_internal()
{
    HMODULE game_module = GetModuleHandle(nullptr);
    if (!game_module) { return nullptr; }

    auto* dos_header = reinterpret_cast<IMAGE_DOS_HEADER*>(game_module);
    auto* nt_headers = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uint8_t*>(game_module) + dos_header->e_lfanew);
    uint8_t* module_base = reinterpret_cast<uint8_t*>(game_module);
    size_t module_size = nt_headers->OptionalHeader.SizeOfImage;

    static const wchar_t* target_str = L"Return an object still needing load from StaticLoadObjectInternal %s";
    size_t target_len_bytes = wcslen(target_str) * sizeof(wchar_t);

    uint8_t* string_addr = nullptr;
    for (uint8_t* p = module_base; p < module_base + module_size - target_len_bytes; ++p)
    {
        if (memcmp(p, target_str, target_len_bytes) == 0) { string_addr = p; break; }
    }
    if (!string_addr) { return nullptr; }

    uint8_t* indirect_ptr_addr = nullptr;
    {
        uint64_t needle = reinterpret_cast<uint64_t>(string_addr);
        for (uint8_t* p = module_base; p < module_base + module_size - sizeof(uint64_t); ++p)
        {
            if (*reinterpret_cast<uint64_t*>(p) == needle) { indirect_ptr_addr = p; break; }
        }
    }

    auto scan_for_rip_refs = [&](uint8_t* target) -> uint8_t* {
        for (uint8_t* p = module_base; p < module_base + module_size - 7; ++p)
        {
            if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05)
            {
                int32_t disp = *reinterpret_cast<int32_t*>(p + 3);
                if (p + 7 + disp == target) { return p; }
            }
        }
        return nullptr;
    };

    uint8_t* xref = scan_for_rip_refs(string_addr);
    if (!xref && indirect_ptr_addr) { xref = scan_for_rip_refs(indirect_ptr_addr); }
    if (!xref) { return nullptr; }

    DWORD64 image_base = 0;
    auto* runtime_func = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(xref), &image_base, nullptr);
    if (!runtime_func) { return nullptr; }

    return reinterpret_cast<void*>(image_base + runtime_func->BeginAddress);
}

static std::filesystem::path get_mods_paks_directory()
{
    HMODULE this_module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&get_mods_paks_directory),
        &this_module);

    if (!this_module) { return {}; }

    wchar_t path_buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(this_module, path_buf, MAX_PATH);
    if (len == 0) { return {}; }

    std::filesystem::path dll_path(path_buf);
    auto game_root = dll_path
        .parent_path() // dlls
        .parent_path() // ModdingAPI
        .parent_path() // Mods
        .parent_path() // ue4ss
        .parent_path() // Win64
        .parent_path() // Binaries
        .parent_path(); // <GameRoot>

    return game_root / "Content" / "Paks" / "~mods";
}

#include <thread>
#include <chrono>


static std::filesystem::path get_own_dll_directory()
{
    HMODULE this_module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&get_own_dll_directory),
        &this_module);

    if (!this_module) { return {}; }

    wchar_t path_buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(this_module, path_buf, MAX_PATH);
    if (len == 0) { return {}; }

    return std::filesystem::path(path_buf).parent_path();
}

static std::filesystem::path get_preferences_path()
{
    auto dir = get_own_dll_directory();
    if (dir.empty()) { return {}; }
    return dir / "skin_preferences.json";
}

namespace ModdingAPI
{
    UObject* LoadAsset(const std::wstring& objectPath)
    {
        auto fname = FName(objectPath.c_str(), FNAME_Add);

        auto* asset_registry = static_cast<UAssetRegistry*>(
            UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);

        if (asset_registry)
        {
            FAssetData asset_data = asset_registry->GetAssetByObjectPath(fname);
            if (asset_data.PackageName().GetComparisonIndex())
            {
                UObject* loaded = UAssetRegistryHelpers::GetAsset(asset_data);
                if (loaded) { return loaded; }
            }
        }

        if (!g_static_load_object_internal)
        {
            g_static_load_object_internal =
                reinterpret_cast<StaticLoadObjectInternalFunc>(find_static_load_object_internal());
        }

        if (!g_static_load_object_internal)
        {
            Output::send(STR("[ModdingAPI] Could not locate StaticLoadObjectInternal\n"));
            return nullptr;
        }

        constexpr int max_attempts = 4;
        constexpr auto retry_delay = std::chrono::milliseconds(300);

        for (int attempt = 1; attempt <= max_attempts; ++attempt)
        {
            UObject* result = g_static_load_object_internal(
                UObject::StaticClass(), nullptr, objectPath.c_str(), nullptr,
                0, nullptr, true, nullptr);

            if (result) { return result; }

            if (attempt < max_attempts)
            {
                std::this_thread::sleep_for(retry_delay);
            }
        }

        return nullptr;
    }

    std::vector<SkinModEntry> ScanForSkinMods()
    {
        std::vector<SkinModEntry> results;

        auto mods_paks_dir = get_mods_paks_directory();
        {
            std::wstring msg = L"[ModdingAPI] Scanning for skin mods in: " + mods_paks_dir.wstring() + L"\n";
            Output::send(msg.c_str());
        }

        if (mods_paks_dir.empty() || !std::filesystem::exists(mods_paks_dir))
        {
            Output::send(STR("[ModdingAPI] Content/Paks/~mods directory not found - no packaged skin mods to load\n"));
            return results;
        }

        std::error_code ec;
        std::filesystem::recursive_directory_iterator dir_it(
            mods_paks_dir, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec)
        {
            Output::send(STR("[ModdingAPI] Could not iterate ~mods\n"));
            return results;
        }

        for (; dir_it != std::filesystem::recursive_directory_iterator(); dir_it.increment(ec))
        {
            if (ec) { break; }
            const auto& entry_path = *dir_it;
            if (!entry_path.is_regular_file()) { continue; }

            if (entry_path.path().filename() != "skinmod.json") { continue; }

            std::ifstream file(entry_path.path());
            if (!file.is_open())
            {
                std::wstring msg = L"[ModdingAPI] Could not open " + entry_path.path().wstring() + L"\n";
                Output::send(msg.c_str());
                continue;
            }

            nlohmann::json j;
            try
            {
                file >> j;
            }
            catch (const std::exception&)
            {
                std::wstring msg = L"[ModdingAPI] Malformed manifest: " + entry_path.path().wstring() + L", skipping\n";
                Output::send(msg.c_str());
                continue;
            }

            if (!j.contains("skins") || !j["skins"].is_array())
            {
                std::wstring msg = L"[ModdingAPI] Manifest has no 'skins' array: " + entry_path.path().wstring() + L", skipping\n";
                Output::send(msg.c_str());
                continue;
            }

            const std::wstring source_label = entry_path.path().parent_path().filename().wstring();

            for (const auto& skin_json : j["skins"])
            {
                auto to_wide = [](const std::string& str) -> std::wstring {
                    return std::wstring(str.begin(), str.end());
                };
                auto get_str = [&](const nlohmann::json& obj, const char* key) -> std::wstring {
                    if (!obj.contains(key) || !obj[key].is_string()) { return L""; }
                    return to_wide(obj[key].get<std::string>());
                };

                if (!skin_json.contains("label") || !skin_json.contains("className"))
                {
                    std::wstring msg = L"[ModdingAPI] Skipping incomplete skin entry in " + source_label + L" (needs label and className)\n";
                    Output::send(msg.c_str());
                    continue;
                }

                SkinModEntry skin;
                skin.label = get_str(skin_json, "label");
                skin.className = get_str(skin_json, "className");
                skin.meshPath = get_str(skin_json, "meshPath");
                skin.sourceModName = source_label;

                const bool has_materials_array = skin_json.contains("materials") && skin_json["materials"].is_array();

                if (skin_json.contains("type") && skin_json["type"].is_string())
                {
                    skin.type = get_str(skin_json, "type");
                }
                else if (has_materials_array)
                {
                    skin.type = L"material";
                }

                if (skin.type == L"material")
                {
                    if (has_materials_array)
                    {
                        for (const auto& m : skin_json["materials"])
                        {
                            if (!m.is_object() || !m.contains("slotIndex") || !m["slotIndex"].is_number_integer()
                                || !m.contains("path") || !m["path"].is_string())
                            {
                                std::wstring msg = L"[ModdingAPI] Skin '" + skin.label
                                    + L"' has a materials entry without a valid slotIndex/path, ignoring that entry\n";
                                Output::send(msg.c_str());
                                continue;
                            }

                            SkinMaterialSlot slot;
                            slot.slotIndex = m["slotIndex"].get<int32_t>();
                            slot.path = get_str(m, "path");
                            if (slot.slotIndex < 0 || slot.path.empty()) { continue; }
                            skin.materials.push_back(std::move(slot));
                        }
                    }

                    if (skin.materials.empty())
                    {
                        std::wstring msg = L"[ModdingAPI] Skin '" + skin.label
                            + L"' has type 'material' but no usable entries in its materials array, skipping\n";
                        Output::send(msg.c_str());
                        continue;
                    }
                }
                else if (skin.meshPath.empty())
                {
                    std::wstring msg = L"[ModdingAPI] Mesh skin '" + skin.label + L"' has no meshPath, skipping\n";
                    Output::send(msg.c_str());
                    continue;
                }

                bool duplicate = false;
                for (const auto& existing : results)
                {
                    if (existing.className == skin.className && existing.label == skin.label)
                    {
                        std::wstring msg = L"[ModdingAPI] Duplicate skin '" + skin.label + L"' for class "
                            + skin.className + L" in '" + skin.sourceModName + L"' - already defined by '"
                            + existing.sourceModName + L"', skipping this one.\n";
                        Output::send(msg.c_str());
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) { continue; }

                results.push_back(std::move(skin));
            }
        }

        {
            std::wstring msg = L"[ModdingAPI] Found " + std::to_wstring(results.size()) + L" skin(s) across scanned manifests\n";
            Output::send(msg.c_str());
        }
        return results;
    }

    void SaveSkinPreference(const std::wstring& className, const std::wstring& skinLabel)
    {
        auto path = get_preferences_path();
        if (path.empty())
        {
            Output::send(STR("[ModdingAPI] Could not resolve preferences file path\n"));
            return;
        }

        auto prefs = LoadSkinPreferences();

        auto to_utf8 = [](const std::wstring& w) -> std::string {
            return std::string(w.begin(), w.end());
        };

        if (skinLabel.empty())
        {
            prefs.erase(className);
        }
        else
        {
            prefs[className] = skinLabel;
        }

        nlohmann::json j;
        for (const auto& [key, value] : prefs)
        {
            j[to_utf8(key)] = to_utf8(value);
        }

        std::ofstream file(path);
        if (!file.is_open())
        {
            Output::send(STR("[ModdingAPI] Could not open preferences file for writing\n"));
            return;
        }
        file << j.dump(2);
    }

    std::unordered_map<std::wstring, std::wstring> LoadSkinPreferences()
    {
        std::unordered_map<std::wstring, std::wstring> result;

        auto path = get_preferences_path();
        if (path.empty() || !std::filesystem::exists(path))
        {
            return result;
        }

        std::ifstream file(path);
        if (!file.is_open()) { return result; }

        nlohmann::json j;
        try
        {
            file >> j;
        }
        catch (const std::exception&)
        {
            Output::send(STR("[ModdingAPI] Malformed skin_preferences.json, ignoring\n"));
            return result;
        }

        auto to_wide = [](const std::string& s) -> std::wstring {
            return std::wstring(s.begin(), s.end());
        };

        for (auto it = j.begin(); it != j.end(); ++it)
        {
            if (!it.value().is_string()) { continue; }
            result[to_wide(it.key())] = to_wide(it.value().get<std::string>());
        }

        return result;
    }
}

class ModdingAPIMod : public CppUserModBase
{
public:
    ModdingAPIMod() : CppUserModBase()
    {
        ModName = STR("ModdingAPI");
        ModVersion = STR("1.0");
        ModDescription = STR("Shared modding utilities: LoadAsset fallback, skin mod scanning");
        ModAuthors = STR("You");
    }

    ~ModdingAPIMod() override = default;
};

#define MODDING_API_MOD_EXPORT __declspec(dllexport)
extern "C"
{
    MODDING_API_MOD_EXPORT RC::CppUserModBase* start_mod()
    {
        return new ModdingAPIMod();
    }

    MODDING_API_MOD_EXPORT void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}