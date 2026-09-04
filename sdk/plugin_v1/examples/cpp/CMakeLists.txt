cmake_minimum_required(VERSION 3.20)
project(orca_cpp_example CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Per-build SDK — build_id taken from generated report, never hardcoded.
# The orca_hook_examples target passes ORCA_SDK_DIR via -D; fallback reads report for manual builds.
if(NOT DEFINED ORCA_SDK_DIR)
  set(_report_path "${CMAKE_CURRENT_SOURCE_DIR}/../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/hook-sdkgen/report/hook-sdkgen-report.json")
  if(EXISTS "${_report_path}")
    file(READ "${_report_path}" _report_json)
    string(JSON _build_id GET "${_report_json}" build build_id)
    set(ORCA_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/plugin-sdk/${_build_id}/cpp")
  else()
    # Fallback: pick latest SDK directory (for CI without report)
    file(GLOB _sdk_dirs "${CMAKE_CURRENT_SOURCE_DIR}/../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/plugin-sdk/*")
    list(SORT _sdk_dirs)
    if(_sdk_dirs)
      list(GET _sdk_dirs -1 _latest)
      set(ORCA_SDK_DIR "${_latest}/cpp")
    endif()
  endif()
endif()
list(APPEND CMAKE_PREFIX_PATH "${ORCA_SDK_DIR}")
find_package(OrcaHook REQUIRED)
# The SDK provides src/plugin_entry.cpp which implements orca_plugin_entry_v1/_exit_v1
add_library(orca_cpp_example SHARED
  src/plugin.cpp
  "${ORCA_SDK_DIR}/src/plugin_entry.cpp"
)
target_include_directories(orca_cpp_example PRIVATE ${OrcaHook_INCLUDE_DIRS})
target_link_libraries(orca_cpp_example PRIVATE OrcaHook::OrcaHook)

if(WIN32)
  set_target_properties(orca_cpp_example PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()

orca_hook_add_metadata(orca_cpp_example)
