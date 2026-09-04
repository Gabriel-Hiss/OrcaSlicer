cmake_minimum_required(VERSION 3.20)
# @GENERATED@ CMakeLists.txt — template for consumer plugin, parameterized per build.
# Emitter fills @ORCA_*@ placeholders from manifest build fields.
project(orca_hook_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# ABI boundary is C-only; plugin may use C++17/20 internally.

# Consumer locates per-build SDK via CMAKE_PREFIX_PATH=generated/plugin-sdk/<build-id>
find_package(OrcaHook REQUIRED)

# Plugin shared library — must be self-contained (static deps only), no SDK DLL dep.
add_library(${ORCA_PLUGIN_ID} SHARED
    src/plugin_entry.cpp
    ${ORCA_PLUGIN_SOURCES}
)

target_include_directories(${ORCA_PLUGIN_ID} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OrcaHook_INCLUDE_DIRS}
)

target_link_libraries(${ORCA_PLUGIN_ID} PRIVATE OrcaHook::OrcaHook)

# Exports: only the two C ABI symbols. Hide everything else.
if(WIN32)
    # .def alternative: use __declspec(dllexport) in plugin_entry.cpp.in
    set_target_properties(${ORCA_PLUGIN_ID} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
else()
    target_link_options(${ORCA_PLUGIN_ID} PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.lds")
    target_link_options(${ORCA_PLUGIN_ID} PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# Metadata embedding — PE resource on Windows, ELF note on Linux.
orca_hook_add_metadata(${ORCA_PLUGIN_ID})

# No RTTI/exceptions cross ABI. Wrappers catch internally; plugin may use them locally.
# LTO off to preserve hookable addresses (matches Orca's post-link flags: no ICF, no incremental).

# Install to data_dir()/orca_plugins/<id>/ — host copies artifact there.
install(TARGETS ${ORCA_PLUGIN_ID} DESTINATION .)
