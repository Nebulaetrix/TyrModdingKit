cmake_minimum_required(VERSION 3.18)
set(TARGET ModdingAPI)
project(${TARGET})

include(FetchContent)
FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

add_library(${TARGET} SHARED "dllmain.cpp")

target_include_directories(${TARGET}
        PUBLIC include
        PRIVATE .
)

target_link_libraries(${TARGET} PUBLIC UE4SS PRIVATE nlohmann_json::nlohmann_json)

target_compile_definitions(${TARGET} PRIVATE MODDING_API_EXPORTS)

add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
        "${GAME_MODS_DIR}/${TARGET}/dlls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET}>"
        "${GAME_MODS_DIR}/${TARGET}/dlls/main.dll"

        # SkinBrowser
        COMMAND ${CMAKE_COMMAND} -E make_directory
        "${GAME_MODS_DIR}/SkinBrowser/dlls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET}>"
        "${GAME_MODS_DIR}/SkinBrowser/dlls/$<TARGET_FILE_NAME:${TARGET}>"

        COMMENT "Deploying ${TARGET} to ${GAME_MODS_DIR}/${TARGET}/dlls/main.dll and SkinBrowser/dlls"
)

add_library(SignatureBypass SHARED SignatureBypass/asi_patches.cpp)
add_dependencies(${TARGET} SignatureBypass)

set_target_properties(SignatureBypass PROPERTIES
        PREFIX ""
        SUFFIX ".asi"
)

add_custom_command(TARGET SignatureBypass POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SignatureBypass>"
        "${GAME_EXE_DIR}/SignatureBypass.asi"
        COMMENT "Deploying SignatureBypass.asi to ${GAME_EXE_DIR}"
)