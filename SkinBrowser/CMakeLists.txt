cmake_minimum_required(VERSION 3.18)
set(TARGET SkinBrowser)
project(${TARGET})

add_library(${TARGET} SHARED "dllmain.cpp")

target_include_directories(${TARGET}
        PRIVATE .
        PRIVATE ../ModdingAPI/include
)

target_link_libraries(${TARGET} PUBLIC UE4SS ModdingAPI)


add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
        "${GAME_MODS_DIR}/${TARGET}/dlls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET}>"
        "${GAME_MODS_DIR}/${TARGET}/dlls/main.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:ModdingAPI>"
        "${GAME_MODS_DIR}/${TARGET}/dlls/ModdingAPI.dll"
        COMMENT "Deploying ${TARGET} (and its ModdingAPI dependency) to ${GAME_MODS_DIR}/${TARGET}/dlls/"
)