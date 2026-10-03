cmake_minimum_required(VERSION 3.29)
project(D2RStreamChat VERSION 1.1.0 LANGUAGES CXX RC)

include(FetchContent)

# Disable SDK examples and tests to keep build lean and fast
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    PluginSDK
    URL https://github.com/D2RLoader/PluginSDK/archive/refs/heads/master.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(PluginSDK)

# Plugin DLL Target
add_library(D2RStreamChat SHARED
    stream_chat.cpp
    stream_chat.rc
)

target_include_directories(D2RStreamChat PRIVATE "${pluginsdk_SOURCE_DIR}/include")
target_link_libraries(D2RStreamChat PRIVATE D2RLPlugin::D2RLPlugin ws2_32 winhttp)
target_compile_features(D2RStreamChat PRIVATE cxx_std_20)

set_target_properties(D2RStreamChat PROPERTIES
    OUTPUT_NAME "d2rl-streamchat"
)

d2rlplugin_embed_config(
    TARGET D2RStreamChat
    FILE "${CMAKE_CURRENT_SOURCE_DIR}/stream-chat.toml"
)