cmake_minimum_required(VERSION 3.13)
add_executable(patch2plugin
    src/main.cpp
    src/manifest.cpp
    src/diff_parser.cpp
    src/brace_scanner.cpp
    src/emitter.cpp
    src/hash_util.cpp
)
target_include_directories(patch2plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(patch2plugin PRIVATE cxx_std_17)
set_target_properties(patch2plugin PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(patch2plugin PRIVATE ZLIB::ZLIB)
    target_compile_definitions(patch2plugin PRIVATE HAVE_ZLIB=1)
    message(STATUS "patch2plugin: ZLIB found - gzip manifest support enabled")
else()
    message(STATUS "patch2plugin: ZLIB not found - gzip manifest will need manual fallback")
endif()
