# Only orca_hook_runtime is built as C++23; Orca and SDKs remain at their current standard.
# Windows x64 first pass uses DIA + Zydis validation; Linux reuses same SafetyHook backend.
# Release asset is a flat ZIP with 4 files (safetyhook.cpp/.hpp + Zydis.c/.h) and no CMakeLists.
# so orca_hook_runtime can compile them directly as C++23 with ZYDIS_STATIC_BUILD.

# SafetyHook upstream: https://github.com/cursey/safetyhook

include(ExternalProject)

ExternalProject_Add(dep_SafetyHook
    URL "https://github.com/cursey/safetyhook/releases/download/v0.7.0/safetyhook-amalgamated-zydis.zip"
    URL_HASH SHA256=505d4c07ec1c5b94a17f3906ca86afbe1264e738d8becaa244866694c6200c2c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/SafetyHook
    INSTALL_DIR ${DESTDIR}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${DESTDIR}/include/safetyhook && ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR> ${DESTDIR}/include/safetyhook && ${CMAKE_COMMAND} -DDESTDIR=${DESTDIR} -P ${CMAKE_CURRENT_LIST_DIR}/normalize_safetyhook.cmake
)

if (MSVC)
    add_debug_dep(dep_SafetyHook)
endif()
