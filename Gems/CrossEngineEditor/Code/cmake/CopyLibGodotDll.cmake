#
# Copyright (c) Cross-Engine Editor Project.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# ---------------------------------------------------------------------------------------
# Build-time step: copy the scons-built libgodot DLL next to the editor executable so the
# Godot backend finds it via LoadLibrary without PATH setup (GodotBackend.cpp tries the
# names below). Invoked from Code/CMakeLists.txt POST_BUILD with:
#   cmake -DCEE_GODOT_ROOT=<godot checkout> -DCEE_GODOT_DLL_DEST=<bin dir> -P CopyLibGodotDll.cmake
# Silently does nothing when Godot has not been built yet, so a header-only checkout still
# builds and the first scons build is picked up on the next editor build (a configure-time
# file(GLOB) would miss it).
# ---------------------------------------------------------------------------------------

if(NOT EXISTS ${CEE_GODOT_DLL_DEST})
    return()
endif()

# scons (platform=windows library_type=shared_library) outputs
# bin/libgodot.windows.editor[.dev].x86_64.dll; the backend loads these exact names.
file(GLOB _cee_dlls "${CEE_GODOT_ROOT}/bin/libgodot.windows.editor*.dll")
if(NOT _cee_dlls)
    return()
endif()

list(GET _cee_dlls 0 _cee_dll)
get_filename_component(_cee_dll_name ${_cee_dll} NAME)
file(COPY_FILE ${_cee_dll} "${CEE_GODOT_DLL_DEST}/${_cee_dll_name}" ONLY_IF_DIFFERENT)
message(STATUS "Copied ${_cee_dll} -> ${CEE_GODOT_DLL_DEST}/${_cee_dll_name}")
