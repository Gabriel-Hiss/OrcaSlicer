# libdwarf v2.3.2 — Linux-only, used only by hook-sdkgen
# Added to deps/ flow but compiled only on Linux x64; macOS/Windows builds never fetch it.
# Provides DWARF parsing for ELF/DWARF generation path (DwarfReader + ELF64 reader)
# before strip. Not linked into Orca runtime.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR LINUX)

orcaslicer_add_cmake_project(libdwarf
    URL "https://github.com/davea42/libdwarf-code/releases/download/v2.3.2/libdwarf-2.3.2.tar.xz"
    URL_HASH SHA256=7992e7b9019ebfabdda5773e86243517c48cf89fafed3209e853692bc9573efd
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_DWARFDUMP=OFF
        -DBUILD_DWARFGEN=OFF
)

endif()
