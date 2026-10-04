cmake_minimum_required(VERSION 3.29)
project(D2RLStreamChat VERSION 1.1.0 LANGUAGES CXX RC)

include(FetchContent)

# Disable SDK examples and tests to keep build lean and fast
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# Check for local PluginSDK or fetch via Git
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../PluginSDK-master/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../PluginSDK-master" "${CMAKE_CURRENT_BINARY_DIR}/pluginsdk-local" EXCLUDE_FROM_ALL)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../PluginSDK/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../PluginSDK" "${CMAKE_CURRENT_BINARY_DIR}/pluginsdk-local" EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(
        PluginSDK
        GIT_REPOSITORY https://github.com/D2RLoader/PluginSDK.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(PluginSDK)
endif()

# Generate the default d2rl-streamchat.toml dynamically in the build directory
set(GENERATED_DEFAULT_TOML "${CMAKE_CURRENT_BINARY_DIR}/d2rl-streamchat.toml")
file(WRITE "${GENERATED_DEFAULT_TOML}"
"# ==============================================================================
# D2R Stream Chat Configuration
# Injects Twitch and YouTube live chat directly into D2R native in-game chat.
# ==============================================================================

[general]
# Chat layout style:
# \"single\"  = Prefix, name, and message on 1 line (uses prefix_color).
# \"twoline\" = Prefix + name on line 1 (prefix_color), message indented on line 2 (message_color).
#     This allows distinct colors for name and message with the tradeoff of taking more chat space.
layout = \"twoline\"

# Filter out automated bot commands starting with '!' (true = hide, false = show)
filter_bot_commands = true

# Platform tag style: \"ttv\", \"twitch\", or \"none\"
prefix_mode = \"twitch\"

# YouTube platform tag style: \"yt\", \"youtube\", or \"none\"
youtube_prefix_mode = \"youtube\"

# Enable developer diagnostic and testing commands (/testcolor, /testchat, etc.)
# Anyone can set this to true to help with troubleshooting and testing without recompiling!
enable_dev_commands = false

[twitch]
enabled = true
# Your Twitch channel name (e.g. \"ghostdragon14712\")
channel = \"\"
username = \"\"
# Optional OAuth token to send replies back to Twitch using /tr.
# If left blank, you will still receive live chat in read-only mode anonymously.
# (\"https://twitchtokengenerator.com/\")
oauth = \"\"
# Color for the [Twitch] tag and username
# Working options: white, red, green, blue, gold, gray, tan, orange, yellow, dark_green, purple, light_green
# Note: \"black\" is supported but hard to see against the dark background.
prefix_color = \"purple\"
# Twitch message text color ( This is ignored if you are in layout single mode. )
message_color = \"white\"

[youtube]
enabled = true
# Your YouTube handle or channel name (e.g. \"@-Ghost.Dragon\")
# Automatically detects when you go live without needing a video ID!
channel = \"\"
# Color for the [YT] tag and username
# Working options: red, gold, white, green, blue, gray, tan, orange, yellow, dark_green, purple, light_green
# Note: \"black\" is supported but hard to see against the dark background.
prefix_color = \"red\"
# YouTube message text color ( This is ignored if you are in layout single mode. )
message_color = \"white\"
")

# Plugin DLL Target
add_library(D2RLStreamChat SHARED
    d2rl-streamchat.cpp
    d2rl-streamchat.rc
)

target_include_directories(D2RLStreamChat PRIVATE "${pluginsdk_SOURCE_DIR}/include")
target_link_libraries(D2RLStreamChat PRIVATE D2RLPlugin::D2RLPlugin)
target_compile_features(D2RLStreamChat PRIVATE cxx_std_20)

set_target_properties(D2RLStreamChat PROPERTIES
    OUTPUT_NAME "d2rl-streamchat"
)

# Embed the dynamically generated default configuration
d2rlplugin_embed_config(
    TARGET D2RLStreamChat
    FILE "${GENERATED_DEFAULT_TOML}"
)

# Set up release directory: build/release/D2RL-StreamChat <version>/
set(RELEASE_ROOT_DIR "${CMAKE_BINARY_DIR}/release")
set(RELEASE_PKG_NAME "D2RL-StreamChat ${PROJECT_VERSION}")
set(RELEASE_PKG_DIR "${RELEASE_ROOT_DIR}/${RELEASE_PKG_NAME}")
set(RELEASE_ZIP_FILE "${RELEASE_ROOT_DIR}/${RELEASE_PKG_NAME}.zip")

# Post-build packaging into release directory and .zip archive
add_custom_command(TARGET D2RLStreamChat POST_BUILD
    # 1. Package into release folder structure
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_PKG_DIR}/plugins"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_PKG_DIR}/config"
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:D2RLStreamChat>
        "${RELEASE_PKG_DIR}/plugins/d2rl-streamchat.dll"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${GENERATED_DEFAULT_TOML}"
        "${RELEASE_PKG_DIR}/config/d2rl-streamchat.toml"
    # 2. Package release folder into .zip archive
    COMMAND ${CMAKE_COMMAND} -E chdir "${RELEASE_ROOT_DIR}" ${CMAKE_COMMAND} -E tar "cf" "${RELEASE_ZIP_FILE}" --format=zip "${RELEASE_PKG_NAME}"
    COMMENT "Packaging release to ${RELEASE_PKG_DIR} and creating ${RELEASE_ZIP_FILE}..."
)

# ------------------------------------------------------------------------------
# Optional Local Dev Overrides (Ignored by Git via CMakeLists.local.cmake)
# ------------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.local.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.local.cmake")
endif()
