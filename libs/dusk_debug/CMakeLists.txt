cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(dusk_debug LANGUAGES CXX)

add_library(dusk_debug STATIC)

target_sources(dusk_debug
PRIVATE
        src/instrument.cpp
        src/TracyBitfield.cpp
        src/Channel.cpp
PUBLIC
FILE_SET HEADERS
BASE_DIRS include
FILES
        include/dusk_debug/instrument.hpp
        include/dusk_debug/TracyBitfield.hpp
        include/dusk_debug/channel.hpp
)

target_compile_features(dusk_debug PUBLIC cxx_std_20)
set_target_properties(dusk_debug PROPERTIES
    CXX_EXTENSIONS OFF          # Disallow compiler-specific extensions
    CXX_STANDARD_REQUIRED ON    # Disallow silently downgrading C++ target version
)

target_link_libraries(dusk_debug PUBLIC Tracy::TracyClient)

target_precompile_headers(dusk_debug
#PUBLIC
#    "include/dusk_debug/instrument.hpp"
PRIVATE
    <array>
    <cassert>
    <concepts>
    <cstddef>
    <limits>
    <memory>
    <mutex>
    <optional>
    <format>
    <string>
    <string_view>
    <type_traits>
    <unordered_map>
    <unordered_set>
    <thread>
)
