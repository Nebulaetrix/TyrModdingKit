cmake_minimum_required(VERSION 3.22)
project(TyrModdingKit)

set(GAME_MODS_DIR
        "O:/SteamLibrary/steamapps/common/Tyr/Tyr/Binaries/Win64/ue4ss/Mods"
        CACHE PATH "Path to the game's UE4SS Mods directory")

set(GAME_EXE_DIR
        "O:/SteamLibrary/steamapps/common/Tyr/Tyr/Binaries/Win64"
        CACHE PATH "Path to the game's exe directory")


if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Game__Dev__Win64" CACHE STRING "Build configuration" FORCE)
endif()

add_subdirectory(RE-UE4SS)
add_subdirectory(ModdingAPI)
add_subdirectory(SkinBrowser)


set(PACKAGER_CSPROJ "${CMAKE_SOURCE_DIR}/TyrModPackager/UnrealReZen/UnrealReZen.csproj"
        CACHE FILEPATH "TyrModPackager project file")
set(PACKAGER_STAGE "${CMAKE_BINARY_DIR}/stage/TyrModPackager")

add_custom_target(TyrModPackager
        COMMAND dotnet publish "${PACKAGER_CSPROJ}" -c Release -r win-x64 --self-contained -o "${PACKAGER_STAGE}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/packager-deps" "${PACKAGER_STAGE}"
        COMMENT "Publishing TyrModPackager with retoc and native DLLs")