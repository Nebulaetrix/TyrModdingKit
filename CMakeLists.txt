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
