cmake_minimum_required(VERSION 3.21)
project(TrafficMonitorAIAPIQuota LANGUAGES CXX RC)

add_library(AIAPIQuota SHARED
    src/Plugin.cpp
    src/UsageApi.cpp
    resources/options.rc
)

target_include_directories(AIAPIQuota PRIVATE include resources src)
target_compile_features(AIAPIQuota PRIVATE cxx_std_17)
target_compile_definitions(AIAPIQuota PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX USE_EDGEMODE_JSRT)
target_link_libraries(AIAPIQuota PRIVATE winhttp crypt32 user32 comctl32 chakrart)

if(MSVC)
    target_compile_options(AIAPIQuota PRIVATE /W4 /permissive- /utf-8)
endif()

set_target_properties(AIAPIQuota PROPERTIES
    OUTPUT_NAME "AIAPIQuota"
    CXX_EXTENSIONS OFF
)
