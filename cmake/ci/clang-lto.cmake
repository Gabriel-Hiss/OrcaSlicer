# Initial cache for the CI legs that build the dependencies and the slicer with
# clang and full LTO, so the optimiser can cross the dependency boundary instead
# of stopping at it.
#
# Passed with `cmake -C`, which keeps it to a single argument and therefore
# survives the word splitting the *_EXTRA_BUILD_ARGS hooks in build_linux.sh,
# build_release_macos.sh and build_release_vs.bat do. Compiler flags with spaces
# cannot be forwarded through those hooks any other way.
#
# Read before project(), so no compiler has been detected yet: CMAKE_HOST_WIN32
# is available here, MSVC and WIN32 are not.

if (CMAKE_HOST_WIN32)
    # clang-cl takes clang driver flags through /clang:. Linking goes straight to
    # lld-link, which reads bitcode without extra flags, so nothing is added to
    # the linker flags here.
    set(_orca_ci_opt_flags "/clang:-O2 /clang:-flto")
    set(CMAKE_C_COMPILER "clang-cl" CACHE FILEPATH "")
    set(CMAKE_CXX_COMPILER "clang-cl" CACHE FILEPATH "")
    set(CMAKE_LINKER_TYPE "LLD" CACHE STRING "")
else ()
    # build_linux.sh already selects clang and lld; macOS uses the Xcode clang.
    # Here the driver performs the link, so it needs the flag as well.
    #
    # macOS gets thin LTO: ld64 asserts on full LTO, seen linking NLopt's test
    # binary with "lto symbol should not be in layout" (Layout.cpp:1456). Thin is
    # also what CMake's own INTERPROCEDURAL_OPTIMIZATION picks for Clang.
    #
    # On Linux the two linker flag entries below are overridden: build_linux.sh
    # passes -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld on the command line, and -D wins
    # over -C. That is harmless, because lld runs LTO on bitcode inputs whether or
    # not the flag reaches the link line.
    if (CMAKE_HOST_APPLE)
        set(_orca_ci_opt_flags "-O2 -flto=thin")
    else ()
        set(_orca_ci_opt_flags "-O2 -flto")
    endif ()
    set(CMAKE_EXE_LINKER_FLAGS "${_orca_ci_opt_flags}" CACHE STRING "")
    set(CMAKE_SHARED_LINKER_FLAGS "${_orca_ci_opt_flags}" CACHE STRING "")
    set(DEPS_LINKER_FLAGS "${_orca_ci_opt_flags}" CACHE STRING "")

    # Every dependency here is a static library. GNU ar writes an index that omits
    # the symbols of bitcode members, so prefer the LLVM tools. Distributions ship
    # them versioned and do not always add the unsuffixed symlink, hence the glob.
    # macOS is left alone: Xcode's ar handles bitcode and llvm-ar is absent there.
    find_program(_orca_ci_llvm_ar NAMES llvm-ar)
    find_program(_orca_ci_llvm_ranlib NAMES llvm-ranlib)
    if (NOT _orca_ci_llvm_ar)
        file(GLOB _orca_ci_ar_candidates /usr/bin/llvm-ar-* /usr/lib/llvm-*/bin/llvm-ar)
        list(SORT _orca_ci_ar_candidates COMPARE NATURAL ORDER DESCENDING)
        list(LENGTH _orca_ci_ar_candidates _orca_ci_ar_count)
        if (_orca_ci_ar_count GREATER 0)
            list(GET _orca_ci_ar_candidates 0 _orca_ci_llvm_ar)
            string(REPLACE "llvm-ar" "llvm-ranlib" _orca_ci_llvm_ranlib "${_orca_ci_llvm_ar}")
        endif ()
    endif ()
    if (_orca_ci_llvm_ar AND EXISTS "${_orca_ci_llvm_ranlib}")
        message(STATUS "clang-lto: using ${_orca_ci_llvm_ar}")
        set(CMAKE_AR "${_orca_ci_llvm_ar}" CACHE FILEPATH "")
        set(CMAKE_RANLIB "${_orca_ci_llvm_ranlib}" CACHE FILEPATH "")
    endif ()
endif ()

# The slicer configure reads the CMAKE_* pair, the deps superbuild reads the
# DEPS_* pair. Each configure ignores the ones it has no use for.
set(CMAKE_C_FLAGS "${_orca_ci_opt_flags}" CACHE STRING "")
set(CMAKE_CXX_FLAGS "${_orca_ci_opt_flags}" CACHE STRING "")
set(DEPS_C_FLAGS "${_orca_ci_opt_flags}" CACHE STRING "")
set(DEPS_CXX_FLAGS "${_orca_ci_opt_flags}" CACHE STRING "")
