#
# Copyright (c) Cross-Engine Editor Project.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# ---------------------------------------------------------------------------------------
# External git submodules for CrossEngineEditor.
#
# These projects are pulled in as git submodules (see repo-root .gitmodules) and wired
# via their own official CMake, per each project's documentation:
#   - Qt-Advanced-Docking-System : https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System
#   - DiligentEngine             : https://github.com/DiligentGraphics/DiligentEngine
#   - tracy                      : https://github.com/wolfpld/tracy (client TU, compiled directly)
#
# To fetch:  git submodule update --init --recursive
#
# This file is included by Gems/CrossEngineEditor/Code/CMakeLists.txt. It creates the
# targets the editor links against (each gated on its CEE_HAVE_* flag):
#   - ads::qtadvanceddocking   (docking system)
#   - CrossEngine::Diligent    (RHI facade)
#   - CrossEngine::Rbfx        (Urho3D engine backend)
#   - CrossEngine::Godot       (libgodot header facade)
#   - CrossEngine::Tracy       (profiler client)
#
# Design notes:
#   * These are third-party sources compiled inside an O3DE build that enables /WX
#     (warnings-as-errors). We defensively clear /WX for the submodule targets so their
#     warnings never fail our build. O3DE first-party code keeps /WX.
#   * We build ADS as a STATIC library so there is no extra runtime DLL to windeployqt.
# ---------------------------------------------------------------------------------------

set(CEE_EXTERNAL_ROOT ${CMAKE_CURRENT_LIST_DIR})

# ---------------------------------------------------------------------------------------
# O3DE runtime-dependency walker integration (see cmake/Platform/Common/
# RuntimeDependencies_common.cmake): the walker recursively follows LINK_LIBRARIES of every
# target and only marks a target "cached" AFTER descending into its children. Third-party
# static-lib graphs (Diligent has cyclic target_link_libraries between its core libs) can
# therefore recurse until CMake's 1000-deep limit and abort configuration. Pre-seed the
# per-target memoization the walker itself uses (O3DE_DEPENDENCIES_CACHED_*) so it treats
# the whole subtree as already-walked leaves. This is correct: these are STATIC libraries
# with no runtime output to copy to the target directory.
#   NOTE: this couples to an O3DE internal implementation detail. If the walker's cache key
#   or semantics change in a future O3DE release, this macro is the single place to update.
# ---------------------------------------------------------------------------------------
macro(cee_mark_third_party_subtree_cached)
    foreach(_cee_cached ${ARGN})
        set_property(GLOBAL PROPERTY O3DE_COPY_DEPENDENCIES_${_cee_cached} "")
        set_property(GLOBAL PROPERTY O3DE_TARGET_DEPENDENCIES_${_cee_cached} "")
        set_property(GLOBAL PROPERTY O3DE_LINK_DEPENDENCIES_${_cee_cached} "")
        set_property(GLOBAL PROPERTY O3DE_IMPORTED_DEPENDENCIES_${_cee_cached} "")
        set_property(GLOBAL PROPERTY O3DE_DEPENDENCIES_CACHED_${_cee_cached} TRUE)
    endforeach()
endmacro()

# =======================================================================================
# Qt-Advanced-Docking-System (ADS)
# =======================================================================================
set(CEE_ADS_ROOT ${CEE_EXTERNAL_ROOT}/Qt-Advanced-Docking-System)

if(EXISTS ${CEE_ADS_ROOT}/CMakeLists.txt)
    # ADS declares BUILD_STATIC/BUILD_EXAMPLES via option() and reads QT_VERSION_MAJOR as a plain
    # variable (v5.1.1: src/CMakeLists.txt uses find_package(Qt${QT_VERSION_MAJOR})). Setting them
    # as ordinary (non-CACHE) variables shadows ADS's options for the add_subdirectory below
    # WITHOUT polluting the global cache — these are generic names any other subproject may use.
    # The submodule is pinned to v5.1.1 (gitlink), whose CMakeLists hardcodes VERSION 5.1.1 —
    # no git-describe / version override needed at this revision.
    set(BUILD_STATIC ON) # Build a static lib for Qt6.
    set(BUILD_EXAMPLES OFF)
    set(QT_VERSION_MAJOR 6)

    # ADS's own CMake calls find_package(Qt6 ...), which needs Qt6Config.cmake via Qt6_DIR.
    # O3DE resolves Qt per-component (Qt6Core_DIR, ...) but does not set Qt6_DIR/QT_DIR, so point
    # ADS at the vendored Qt6 cmake package dir (from O3DE's QT_PATH). No FORCE: an existing
    # user-provided Qt6_DIR/QT_DIR in the cache is respected.
    if(DEFINED QT_PATH AND EXISTS ${QT_PATH}/lib/cmake/Qt6/Qt6Config.cmake)
        set(QT_DIR ${QT_PATH}/lib/cmake/Qt6 CACHE PATH "")
        set(Qt6_DIR ${QT_PATH}/lib/cmake/Qt6 CACHE PATH "")
    endif()

    # v5.1.1 discovers Qt in two stages: find_package(QT NAMES Qt${QT_VERSION_MAJOR}) umbrella
    # first, then find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core Gui Widgets). Both resolve
    # through Qt6_DIR above against the same vendored Qt SDK that O3DE's 3rdParty::Qt targets
    # use, so the whole editor links one Qt.
    add_subdirectory(${CEE_ADS_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/ads EXCLUDE_FROM_ALL)

    # ADS names its target "qtadvanceddocking-qt6" (Qt6) or "qtadvanceddocking".
    # Normalise to a stable alias the editor links against.
    if(TARGET qtadvanceddocking-qt6)
        set(CEE_ADS_TARGET qtadvanceddocking-qt6)
    elseif(TARGET qtadvanceddocking)
        set(CEE_ADS_TARGET qtadvanceddocking)
    else()
        message(FATAL_ERROR "ADS submodule present but neither qtadvanceddocking-qt6 nor qtadvanceddocking target was created.")
    endif()

    # v5.1.1 (pinned) already creates the ads::qtadvanceddocking alias itself; this guard is
    # forward-compat only, for a hypothetical older ADS revision that does not.
    if(NOT TARGET ads::qtadvanceddocking)
        add_library(ads::qtadvanceddocking ALIAS ${CEE_ADS_TARGET})
    endif()

    # Third-party code must not fail our /WX build.
    if(MSVC)
        target_compile_options(${CEE_ADS_TARGET} PRIVATE /WX-)
    else()
        target_compile_options(${CEE_ADS_TARGET} PRIVATE -Wno-error)
    endif()

    set(CEE_HAVE_ADS TRUE PARENT_SCOPE)
    # No include-dir export needed: the pinned ADS target PUBLIC-exports its src/ dir, so
    # linking ads::qtadvanceddocking propagates <DockManager.h> to consumers by itself.
else()
    # ADS is UI infrastructure, not an optional backend: a configure-time failure with the
    # one-line fix beats silently building an untested FancyDocking fallback editor (dock
    # style review round, dock_style_review.md ch.3-1). Backends degrade gracefully by
    # design (C2 removability); the dock system has no second supported path.
    message(FATAL_ERROR
        "Qt-Advanced-Docking-System submodule not found at ${CEE_ADS_ROOT}. "
        "Run: git submodule update --init --recursive and re-configure. "
        "ADS is required - the FancyDocking fallback was removed.")
endif()

# =======================================================================================
# DiligentEngine (RHI) - optional, gated so the editor still builds without it.
# =======================================================================================
set(CEE_DILIGENT_ROOT ${CEE_EXTERNAL_ROOT}/DiligentEngine)

# DiligentEngine is a very large recursive submodule. Building it is opt-in so a plain
# `git submodule update --init` (non-recursive) does not force a multi-minute RHI build.
option(CEE_ENABLE_DILIGENT "Build the DiligentEngine RHI backend for CrossEngineEditor" ON)

# rbfx VENDORS its own full copy of DiligentEngine (Source/ThirdParty/Diligent) and always
# builds it as its renderer. Our standalone DiligentEngine submodule declares the SAME
# Diligent-* target names, which is a fatal duplicate-target collision. rbfx + Godot are the
# primary real-engine backends, so when rbfx is enabled we suppress our standalone Diligent
# submodule (the DiligentBackend is only a bring-up RHI and is superseded by rbfx here).
option(CEE_ENABLE_RBFX "Build the rbfx (Urho3D) engine backend for CrossEngineEditor" ON)
if(CEE_ENABLE_RBFX AND CEE_ENABLE_DILIGENT)
    message(STATUS "CrossEngineEditor: rbfx enabled -> disabling standalone DiligentEngine submodule "
                   "(rbfx vendors its own Diligent; avoids duplicate Diligent-* targets).")
    set(CEE_ENABLE_DILIGENT OFF)
endif()

if(CEE_ENABLE_DILIGENT AND EXISTS ${CEE_DILIGENT_ROOT}/CMakeLists.txt)
    # Diligent build knobs (see DiligentEngine/BuildTools + DiligentCore CMake options).
    # The editor's first bring-up presents through D3D12 only, so disable every other backend.
    # This drops the SPIRV-Cross / SPIRV-Tools / volk / glslang third-party trees entirely (they
    # only exist for the Vulkan/Metal HLSL->SPIRV path), which both slashes build time and removes
    # their many /WX (warnings-as-errors) failures under O3DE's global compile flags.
    set(DILIGENT_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_FX OFF CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(DILIGENT_NO_OPENGL ON CACHE BOOL "" FORCE)
    set(DILIGENT_NO_VULKAN ON CACHE BOOL "" FORCE)
    set(DILIGENT_NO_DIRECT3D11 ON CACHE BOOL "" FORCE)
    set(DILIGENT_NO_METAL ON CACHE BOOL "" FORCE)
    set(DILIGENT_NO_WEBGPU ON CACHE BOOL "" FORCE)
    set(DILIGENT_NO_ARCHIVER ON CACHE BOOL "" FORCE)
    set(DILIGENT_INSTALL_CORE OFF CACHE BOOL "" FORCE)

    # O3DE injects /W4 /WX (warnings-as-errors) and force-includes VSCompat.h through the global
    # CMAKE_CXX_FLAGS, which every Diligent third-party TU inherits and fails on. Rather than chase
    # each of Diligent's dozens of nested targets, strip /WX and force /W0 in THIS directory scope
    # before add_subdirectory: add_subdirectory inherits the parent directory's CMAKE_*_FLAGS, so
    # all Diligent targets compile warning-free. First-party O3DE code is unaffected (it is compiled
    # in its own directory scopes that keep the original flags).
    string(REGEX REPLACE "/WX( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/WX( |$)" " " CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE "/W4( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/W4( |$)" " " CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    # O3DE also promotes a set of individual warnings to errors via /weNNNN (e.g. /we4296, /we4774).
    # /WX- does NOT undo /weNNNN, so strip every /we#### token or Diligent third-party code that
    # legitimately triggers them (constant-conditional, non-literal printf format, ...) still fails.
    string(REGEX REPLACE "/we[0-9]+( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/we[0-9]+( |$)" " " CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    if(MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W0 /WX-")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W0 /WX-")
    endif()

    add_subdirectory(${CEE_DILIGENT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/diligent EXCLUDE_FROM_ALL)

    # Diligent exposes namespaced imported targets. We link the STATIC engine libs (not the
    # -shared DLL variants): this avoids shipping/deploying Diligent DLLs and keeps O3DE's runtime
    # dependency walker from recursing the shared-lib graph. GetEngineFactoryD3D12() links directly
    # against the static lib. Roll a facade INTERFACE target so the editor depends on one name.
    add_library(CrossEngine_Diligent INTERFACE)
    add_library(CrossEngine::Diligent ALIAS CrossEngine_Diligent)

    set(_cee_diligent_link "")
    foreach(_diligent_tgt
            Diligent-GraphicsEngineD3D12-static
            Diligent-Common
            Diligent-GraphicsAccessories
            Diligent-GraphicsTools
            Diligent-BuildSettings)
        if(TARGET ${_diligent_tgt})
            list(APPEND _cee_diligent_link ${_diligent_tgt})
            # Only real (non-INTERFACE) targets accept PRIVATE compile options. Some Diligent
            # targets (e.g. Diligent-BuildSettings) are INTERFACE libraries and must be skipped.
            get_target_property(_diligent_tgt_type ${_diligent_tgt} TYPE)
            if(MSVC AND NOT _diligent_tgt_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${_diligent_tgt} PRIVATE /WX-)
            endif()
        endif()
    endforeach()

    target_link_libraries(CrossEngine_Diligent INTERFACE ${_cee_diligent_link})
    target_include_directories(CrossEngine_Diligent INTERFACE
        ${CEE_DILIGENT_ROOT}/DiligentCore)
    target_compile_definitions(CrossEngine_Diligent INTERFACE CEE_HAVE_DILIGENT=1)

    # Keep O3DE's runtime-dependency walker out of Diligent's static-lib graph (see the
    # cee_mark_third_party_subtree_cached macro at the top of this file).
    cee_mark_third_party_subtree_cached(${_cee_diligent_link} CrossEngine_Diligent)

    set(CEE_HAVE_DILIGENT TRUE PARENT_SCOPE)
else()
    if(CEE_ENABLE_DILIGENT)
        message(WARNING
            "DiligentEngine submodule not found at ${CEE_DILIGENT_ROOT}. "
            "Run: git submodule update --init --recursive. "
            "CrossEngineEditor will build with the NullBackend (no GPU rendering).")
    endif()
    set(CEE_HAVE_DILIGENT FALSE PARENT_SCOPE)
endif()

# =======================================================================================
# rbfx (Urho3D fork) - optional real engine backend, gated so the editor still builds
# without it. Uses the local checkout via add_subdirectory (Plan §B1).
# =======================================================================================
# rbfx is large; building it is opt-in. Default ON per project requirement.
# Path to the rbfx source checkout. Override with -DCEE_RBFX_ROOT=<path> if needed.
set(CEE_RBFX_ROOT "I:/rbfx" CACHE PATH "Path to the rbfx (Urho3D) source checkout")

if(CEE_ENABLE_RBFX AND EXISTS ${CEE_RBFX_ROOT}/CMakeLists.txt)
    # Build-log provenance: the checkout is a plain path (not a pinned submodule), so record
    # which revision this configure actually builds.
    execute_process(COMMAND git -C ${CEE_RBFX_ROOT} describe --tags --always --dirty
        OUTPUT_VARIABLE _cee_rbfx_desc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_cee_rbfx_desc)
        message(STATUS "CrossEngineEditor: rbfx backend = ${_cee_rbfx_desc} (${CEE_RBFX_ROOT})")
    else()
        message(STATUS "CrossEngineEditor: rbfx backend at ${CEE_RBFX_ROOT} (not a git checkout - revision unknown)")
    endif()

    # rbfx build knobs: we only need the engine library, no samples/tools/tests.
    set(URHO3D_SAMPLES OFF CACHE BOOL "" FORCE)
    set(URHO3D_TOOLS OFF CACHE BOOL "" FORCE)
    set(URHO3D_PLAYER OFF CACHE BOOL "" FORCE)
    set(URHO3D_EDITOR OFF CACHE BOOL "" FORCE)
    set(URHO3D_EXTRAS OFF CACHE BOOL "" FORCE)
    set(URHO3D_PCH ON CACHE BOOL "" FORCE)
    # O3DE's RecastNavigation Gem pulls recastnavigation via FetchContent, which creates the
    # targets Recast/Detour/DetourCrowd/DetourTileCache. rbfx bundles its OWN copies of these
    # same-named ThirdParty targets under Source/ThirdParty, causing a fatal duplicate-target
    # collision at configure time. The editor backend needs no navmesh, so disable rbfx's
    # navigation subsystem (drops its Detour/Recast targets entirely).
    set(URHO3D_NAVIGATION OFF CACHE BOOL "" FORCE)
    # rbfx bundles its OWN copies of ImGui / OpenSSL / tracy / libdatachannel under
    # Source/ThirdParty. Those collide with O3DE's ImGui Gem target and other 3rd-party
    # targets already in this build. None are needed by a headless editor backend, so
    # disable the subsystems that pull them in.
    set(URHO3D_SYSTEMUI OFF CACHE BOOL "" FORCE)   # drops bundled ImGui/ImGuizmo
    set(URHO3D_RMLUI OFF CACHE BOOL "" FORCE)       # drops RmlUi (also uses ImGui)
    set(URHO3D_PROFILING OFF CACHE BOOL "" FORCE)   # drops tracy/TracyImGui
    set(URHO3D_NETWORK OFF CACHE BOOL "" FORCE)     # drops libdatachannel/OpenSSL alias
    set(URHO3D_TESTING OFF CACHE BOOL "" FORCE)
    # The editor presents through D3D12 only (matching the DiligentBackend bring-up). Disable the
    # Vulkan/OpenGL/D3D11 renderer backends so rbfx does not pull vulkan/vulkan.h, volk, GLEW, etc.
    # Keep the shader translator ON so the NO_* options are honoured (they are forced ON when the
    # translator is OFF, which would disable D3D12 too).
    set(URHO3D_SHADER_TRANSLATOR ON CACHE BOOL "" FORCE)
    set(URHO3D_GRAPHICS_NO_VULKAN ON CACHE BOOL "" FORCE)
    set(URHO3D_GRAPHICS_NO_GL ON CACHE BOOL "" FORCE)
    set(URHO3D_GRAPHICS_NO_D3D11 ON CACHE BOOL "" FORCE)
    set(URHO3D_GRAPHICS_NO_D3D12 OFF CACHE BOOL "" FORCE)

    # Same /WX handling as Diligent: strip warnings-as-errors in this directory scope so
    # rbfx third-party code (and EASTL) compiles under O3DE's global /W4 /WX flags. O3DE
    # first-party code is compiled in its own scopes and is unaffected.
    string(REGEX REPLACE "/WX( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/WX( |$)" " " CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE "/W4( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/W4( |$)" " " CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE "/we[0-9]+( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/we[0-9]+( |$)" " " CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    # Embree forces /Ox on its perf-critical targets even in Debug, which MSVC rejects
    # combined with O3DE's Debug runtime checks (/RTCsu, D8016). Drop the checks for the
    # rbfx subtree only (third-party code; O3DE first-party Debug keeps them).
    string(REGEX REPLACE "/RTC[sum1]+( |$)" " " CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REGEX REPLACE "/RTC[sum1]+( |$)" " " CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    if(MSVC)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W0 /WX-")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W0 /WX-")
    endif()

    # rbfx has an extremely deep ThirdParty tree. Under the default binary dir
    # (build/windows/External/CrossEngineEditor-XXXX/Code/CrossEngineExternal/rbfx/...) MSBuild's
    # per-project .tlog paths blow past Windows' 260-char MAX_PATH and fail with MSB3501. Build
    # rbfx under a short, fixed root instead. Override with -DCEE_RBFX_BUILD_DIR=<short path>.
    set(CEE_RBFX_BUILD_DIR "${CMAKE_BINARY_DIR}/rbfx" CACHE PATH "Short binary dir for rbfx subbuild")
    add_subdirectory(${CEE_RBFX_ROOT} ${CEE_RBFX_BUILD_DIR} EXCLUDE_FROM_ALL)
    if(NOT TARGET Urho3D)
        message(FATAL_ERROR "rbfx present at ${CEE_RBFX_ROOT} but no 'Urho3D' target was created.")
    endif()

    if(MSVC)
        target_compile_options(Urho3D PRIVATE /WX-)
    endif()

    # Facade target the editor links against (single stable name), mirroring CrossEngine::Diligent.
    add_library(CrossEngine_Rbfx INTERFACE)
    add_library(CrossEngine::Rbfx ALIAS CrossEngine_Rbfx)
    target_link_libraries(CrossEngine_Rbfx INTERFACE Urho3D)
    # rbfx public headers are included as <Urho3D/...>, so expose the source root.
    target_include_directories(CrossEngine_Rbfx INTERFACE ${CEE_RBFX_ROOT}/Source)
    target_compile_definitions(CrossEngine_Rbfx INTERFACE CEE_HAVE_RBFX=1)

    # Keep O3DE's runtime-dependency walker out of rbfx's static-lib graph (see the
    # cee_mark_third_party_subtree_cached macro at the top of this file).
    cee_mark_third_party_subtree_cached(Urho3D CrossEngine_Rbfx)

    set(CEE_HAVE_RBFX TRUE PARENT_SCOPE)
else()
    if(CEE_ENABLE_RBFX)
        message(WARNING
            "rbfx checkout not found at ${CEE_RBFX_ROOT}. "
            "Set -DCEE_RBFX_ROOT=<path> to your rbfx source. "
            "CrossEngineEditor will build without the rbfx backend.")
    endif()
    set(CEE_HAVE_RBFX FALSE PARENT_SCOPE)
endif()

# =======================================================================================
# Filament (PBR rendering engine) - optional real engine backend, PREBUILT consumption.
#
# Filament is a rendering-only library (no scene serialization, no game logic). It is built
# STANDALONE (its own CMake tree, e.g. G:/o3de/build/filament-standalone) and consumed here
# through an INTERFACE facade. add_subdirectory is NOT possible: Filament's flat target
# namespace (math, gtest, meshoptimizer, glslang, spirv-*) collides with targets that already
# exist in this O3DE build (O3DE's FetchContent gtest/meshoptimizer, rbfx's vendored
# embree math + glslang) - the collision audit gate failed, so the out-of-tree pattern
# (same family as the Godot scons build) is the honest fallback.
#
# One-time bootstrap (MSVC, /MD to match O3DE):
#   cmake -E env --unset=MSYSTEM cmake -S I:/filament -B <prebuilt> -G "Visual Studio 18 2026" ^
#     -DUSE_STATIC_CRT=OFF -DFILAMENT_SKIP_SAMPLES=ON -DFILAMENT_SKIP_SDL2=ON ^
#     -DFILAMENT_SUPPORTS_VULKAN=OFF -DFILAMENT_ENABLE_MATDBG=OFF ^
#     -DFILAMENT_SHORTEN_MSVC_COMPILATION=OFF  (else Debug libs bake
#     _ITERATOR_DEBUG_LEVEL=0, root CMakeLists.txt:328, and MSVC refuses to link them
#     into the editor's IDL-2 binary: LNK2038)
#   cmake --build <prebuilt> --config Debug --target filament gltfio_core uberarchive stb ^
#     matc resgen filaflat filabridge math utils backend geometry image imageio ktxreader bluegl ^
#     basis_transcoder mikktspace
#   (then repeat --config RelWithDebInfo for the same targets: the editor's Profile/Release
#    links those; editor Debug links the Debug libs - see the CONFIG pair below)
# (--unset=MSYSTEM: Filament rejects MSYS2 environments on sight; Git Bash sets it.)
# =======================================================================================
option(CEE_ENABLE_FILAMENT "Build the Filament rendering engine backend for CrossEngineEditor" ON)
set(CEE_FILAMENT_ROOT "I:/filament" CACHE PATH "Path to the Filament source checkout (headers)")
set(CEE_FILAMENT_PREBUILT "G:/o3de/build/filament-standalone" CACHE PATH "Prebuilt standalone Filament build tree (see bootstrap above)")
# Editor Debug links filament Debug; editor Profile/Release link filament RelWithDebInfo
# (both release-CRT /MD, so no _ITERATOR_DEBUG_LEVEL clash). Bootstrap both configs once.
set(CEE_FILAMENT_CONFIG_DEBUG "Debug" CACHE STRING "Filament config paired with editor Debug")
set(CEE_FILAMENT_CONFIG_OPT "RelWithDebInfo" CACHE STRING "Filament config paired with editor Profile/Release")

if(CEE_ENABLE_FILAMENT AND EXISTS ${CEE_FILAMENT_ROOT}/filament/include/filament/Engine.h AND EXISTS ${CEE_FILAMENT_PREBUILT})
    # Build-log provenance (same pattern as rbfx).
    execute_process(COMMAND git -C ${CEE_FILAMENT_ROOT} describe --tags --always --dirty
        OUTPUT_VARIABLE _cee_filament_desc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_cee_filament_desc)
        message(STATUS "CrossEngineEditor: filament backend = ${_cee_filament_desc} (${CEE_FILAMENT_ROOT}, prebuilt ${CEE_FILAMENT_PREBUILT} ${CEE_FILAMENT_CONFIG_OPT})")
    else()
        message(STATUS "CrossEngineEditor: filament backend at ${CEE_FILAMENT_ROOT} (prebuilt ${CEE_FILAMENT_PREBUILT})")
    endif()

    # Locate a prebuilt artifact (.lib / .exe) by name under the standalone tree. VS
    # multi-config default layout puts each target's output at a target-dir of varying
    # depth (filament/, libs/gltfio/, third_party/stb/tnt/...), so glob everything once
    # and filter by /<Config>/<name><ext>.
    file(GLOB_RECURSE _cee_filament_artifacts
        "${CEE_FILAMENT_PREBUILT}/*.lib" "${CEE_FILAMENT_PREBUILT}/*.exe")
    function(_cee_find_filament_artifact out name ext config)
        set(_cee_matches ${_cee_filament_artifacts})
        list(FILTER _cee_matches INCLUDE REGEX "/${config}/${name}${ext}$")
        list(SORT _cee_matches)
        if(_cee_matches)
            list(GET _cee_matches 0 _cee_first)
            set(${out} ${_cee_first} PARENT_SCOPE)
        else()
            set(${out} "" PARENT_SCOPE)
        endif()
    endfunction()

    # Static-lib closure needed by the editor backend (gltfio_core pulls ktxreader /
    # basis_transcoder / dracodec / meshoptimizer; geometry pulls mikktspace; filament
    # pulls backend+bluegl, filaflat, filabridge; uberarchive + stb for the ubershader
    # provider and image decoding). Resolved per config; generator expressions pick the
    # matching list for the editor config being linked.
    set(_cee_filament_lib_names filament backend bluegl math utils filaflat filabridge
        gltfio_core uberarchive stb ktxreader basis_transcoder mikktspace dracodec
        meshoptimizer webpdecoder uberzlib image imageio geometry astcenc smol fusion
        jpeg png zlib_static tinyexr)

    set(_cee_filament_debug_libs "")
    set(_cee_filament_opt_libs "")
    foreach(_cee_lib ${_cee_filament_lib_names})
        _cee_find_filament_artifact(_cee_dbg ${_cee_lib} ".lib" "${CEE_FILAMENT_CONFIG_DEBUG}")
        if(_cee_dbg)
            list(APPEND _cee_filament_debug_libs ${_cee_dbg})
        endif()
        _cee_find_filament_artifact(_cee_opt ${_cee_lib} ".lib" "${CEE_FILAMENT_CONFIG_OPT}")
        if(_cee_opt)
            list(APPEND _cee_filament_opt_libs ${_cee_opt})
        endif()
    endforeach()
    # Configure-time closure check: everything the editor links must exist, in BOTH paired
    # configs. A half-built Debug tree fails here instead of as LNK2019 at editor-Debug link
    # time; no Debug libs at all is legal for Profile/Release-only workflows (warn once).
    set(_cee_filament_required filament backend bluegl math utils filaflat filabridge
        gltfio_core uberarchive stb geometry image imageio ktxreader basis_transcoder
        mikktspace dracodec meshoptimizer)
    foreach(_cee_required ${_cee_filament_required})
        _cee_find_filament_artifact(_cee_path ${_cee_required} ".lib" "${CEE_FILAMENT_CONFIG_OPT}")
        if(NOT _cee_path)
            message(FATAL_ERROR
                "CrossEngineEditor: prebuilt Filament tree ${CEE_FILAMENT_PREBUILT} "
                "(${CEE_FILAMENT_CONFIG_OPT}) is missing: ${_cee_required}. "
                "Run the bootstrap build documented in External/CMakeLists.txt.")
        endif()
    endforeach()
    if(_cee_filament_debug_libs)
        foreach(_cee_required ${_cee_filament_required})
            _cee_find_filament_artifact(_cee_path ${_cee_required} ".lib" "${CEE_FILAMENT_CONFIG_DEBUG}")
            if(NOT _cee_path)
                message(FATAL_ERROR
                    "CrossEngineEditor: prebuilt Filament tree ${CEE_FILAMENT_PREBUILT} "
                    "(${CEE_FILAMENT_CONFIG_DEBUG}) is missing: ${_cee_required}. "
                    "Run the bootstrap build for both configs (External/CMakeLists.txt).")
            endif()
        endforeach()
    else()
        message(WARNING
            "CrossEngineEditor: no ${CEE_FILAMENT_CONFIG_DEBUG} filament libs found in "
            "${CEE_FILAMENT_PREBUILT}; editor Debug will not link (LNK2038 iterator-level "
            "mismatch). Build filament Debug (bootstrap above) or use Profile/Release.")
    endif()

    _cee_find_filament_artifact(CEE_MATC_EXE matc ".exe" "${CEE_FILAMENT_CONFIG_OPT}")
    _cee_find_filament_artifact(CEE_RESGEN_EXE resgen ".exe" "${CEE_FILAMENT_CONFIG_OPT}")
    if(NOT CEE_MATC_EXE OR NOT CEE_RESGEN_EXE)
        message(FATAL_ERROR
            "CrossEngineEditor: prebuilt Filament tree ${CEE_FILAMENT_PREBUILT} is missing "
            "the matc/resgen tools. Build them (see bootstrap in External/CMakeLists.txt).")
    endif()

    # Facade target - editor links this single name. Debug editor picks the filament Debug
    # libs, Profile/Release pick RelWithDebInfo (CRT/iterator ABI has to match).
    add_library(CrossEngine_Filament INTERFACE)
    add_library(CrossEngine::Filament ALIAS CrossEngine_Filament)
    if(_cee_filament_debug_libs)
        target_link_libraries(CrossEngine_Filament INTERFACE
            $<$<CONFIG:Debug>:${_cee_filament_debug_libs}>)
    endif()
    target_link_libraries(CrossEngine_Filament INTERFACE
        $<$<NOT:$<CONFIG:Debug>>:${_cee_filament_opt_libs}>)
    # Keep this include list in sync with the BEFORE PRIVATE copy in Code/CMakeLists.txt
    # (that copy exists to outrank rbfx's INTERFACE include order; both must name the same
    # filament roots).
    target_include_directories(CrossEngine_Filament INTERFACE
        ${CEE_FILAMENT_ROOT}/filament/include
        ${CEE_FILAMENT_ROOT}/filament/backend/include
        ${CEE_FILAMENT_ROOT}/libs/gltfio/include
        ${CEE_FILAMENT_ROOT}/libs/filabridge/include
        ${CEE_FILAMENT_ROOT}/libs/math/include
        ${CEE_FILAMENT_ROOT}/libs/utils/include
        ${CEE_FILAMENT_PREBUILT}/libs/gltfio   # generated materials/uberarchive.h
        ${CEE_FILAMENT_ROOT}/third_party/cgltf
        ${CEE_FILAMENT_ROOT}/third_party/robin-map/include
        ${CEE_FILAMENT_ROOT}/third_party/stb)
    target_compile_definitions(CrossEngine_Filament INTERFACE CEE_HAVE_FILAMENT=1)

    # Keep O3DE's runtime-dependency walker out of the prebuilt static-lib graph.
    cee_mark_third_party_subtree_cached(CrossEngine_Filament)

    # --- CEE material pipeline: .mat -> matc -> .filamat -> resgen -> embedded blob -------
    # matc and the engine libs come from the same checkout + build, so the .filamat version
    # lock (M5) holds. resgen invocation mirrors filament's get_resgen_vars Windows branch:
    # -qcx <dir> -p <prefix> emits <dir>/<prefix>.c + <prefix>.h with
    # <PREFIX>_<STEM>_DATA/SIZE macros (root CMakeLists.txt:874-900).
    set(CEE_FILAMENT_ASSETS "${CMAKE_CURRENT_LIST_DIR}/../Assets/Filament")
    set(CEE_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/filament_resources")
    file(MAKE_DIRECTORY "${CEE_RESOURCE_DIR}")

    set(CEE_FILAMAT_BINS "")
    foreach(_cee_mat cee_overlay cee_lit_opaque)
        set(_cee_filamat "${CEE_RESOURCE_DIR}/${_cee_mat}.filamat")
        add_custom_command(
            OUTPUT ${_cee_filamat}
            COMMAND ${CEE_MATC_EXE} -o ${_cee_filamat} ${CEE_FILAMENT_ASSETS}/${_cee_mat}.mat
            DEPENDS ${CEE_MATC_EXE} ${CEE_FILAMENT_ASSETS}/${_cee_mat}.mat
            COMMENT "CEE: compiling ${_cee_mat}.mat"
        )
        list(APPEND CEE_FILAMAT_BINS ${_cee_filamat})
    endforeach()

    set(CEE_RESGEN_HEADER "${CEE_RESOURCE_DIR}/cee_filamat.h")
    set(CEE_RESGEN_SOURCE "${CEE_RESOURCE_DIR}/cee_filamat.c")
    add_custom_command(
        OUTPUT ${CEE_RESGEN_HEADER} ${CEE_RESGEN_SOURCE} "${CEE_RESOURCE_DIR}/cee_filamat.bin"
        COMMAND ${CEE_RESGEN_EXE} -qcx ${CEE_RESOURCE_DIR} -p cee_filamat ${CEE_FILAMAT_BINS}
        DEPENDS ${CEE_RESGEN_EXE} ${CEE_FILAMAT_BINS}
        COMMENT "CEE: embedding Filament material blobs"
    )

    # dummy.c content is constant - write it at configure time (a `>` redirect inside
    # add_custom_command only works when the generator routes through a shell; Ninja does not).
    set(CEE_DUMMY_SRC "${CEE_RESOURCE_DIR}/dummy.c")
    file(WRITE ${CEE_DUMMY_SRC} "//\n")

    add_library(cee_filamat_resources STATIC ${CEE_DUMMY_SRC} ${CEE_RESGEN_SOURCE})
    set_target_properties(cee_filamat_resources PROPERTIES FOLDER Libs/CrossEngine)
    if(MSVC)
        target_compile_options(cee_filamat_resources PRIVATE /WX-)
    endif()
    target_include_directories(cee_filamat_resources PUBLIC ${CEE_RESOURCE_DIR})

    target_link_libraries(CrossEngine_Filament INTERFACE cee_filamat_resources)

    set(CEE_HAVE_FILAMENT TRUE PARENT_SCOPE)
else()
    if(CEE_ENABLE_FILAMENT)
        message(WARNING
            "Filament prebuilt not found (source ${CEE_FILAMENT_ROOT}, prebuilt "
            "${CEE_FILAMENT_PREBUILT}). Build Filament standalone once - see the bootstrap "
            "comment in External/CMakeLists.txt. CrossEngineEditor will build without the "
            "Filament backend.")
    endif()
    set(CEE_HAVE_FILAMENT FALSE PARENT_SCOPE)
endif()

# =======================================================================================
# Godot (libgodot) - optional real engine backend, gated so the editor still builds
# without it. Default ON per project requirement.
#
# Unlike rbfx/Diligent we do NOT add_subdirectory Godot: it is built out-of-tree with scons
# (scons platform=windows target=editor library_type=shared_library) into a libgodot DLL,
# and the editor loads that DLL at RUNTIME via LoadLibrary + libgodot_create_godot_instance
# (Plan §B1 / godot_migration.md §2). At BUILD time we only need Godot's GDExtension C-API headers so GodotBackend
# can call libgodot.h / gdextension_interface.h. This keeps Godot's enormous build graph out
# of our configure entirely and means enabling Godot never forces a multi-hour scons build.
# =======================================================================================
option(CEE_ENABLE_GODOT "Build the Godot (libgodot) engine backend for CrossEngineEditor" ON)
set(CEE_GODOT_ROOT "I:/godot" CACHE PATH "Path to the Godot source checkout (for libgodot GDExtension headers)")

if(CEE_ENABLE_GODOT AND EXISTS ${CEE_GODOT_ROOT}/core/extension/libgodot.h)
    # Header-only INTERFACE facade: exposes Godot's include roots so GodotBackend can
    # #include "core/extension/libgodot.h" and the generated gdextension_interface header.
    # No link libraries: libgodot is resolved at runtime via LoadLibrary.
    add_library(CrossEngine_Godot INTERFACE)
    add_library(CrossEngine::Godot ALIAS CrossEngine_Godot)
    target_include_directories(CrossEngine_Godot INTERFACE
        ${CEE_GODOT_ROOT}
        ${CEE_GODOT_ROOT}/core/extension)
    target_compile_definitions(CrossEngine_Godot INTERFACE CEE_HAVE_GODOT=1)

    # gdextension_interface.gen.h is emitted by Godot's scons build (make_interface_header.py over
    # gdextension_interface.json). If the checkout has never been built, run that same script here:
    # byte-identical output, and *.gen.* is gitignored in the Godot tree.
    if(NOT EXISTS ${CEE_GODOT_ROOT}/core/extension/gdextension_interface.gen.h)
        # PYTHONPATH=<godot root>: the script imports Godot's build helper `methods`.
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${CEE_GODOT_ROOT}"
                ${LY_PYTHON_CMD} -c
                "import make_interface_header; make_interface_header.run(['gdextension_interface.gen.h'], ['gdextension_interface.json'], None)"
            WORKING_DIRECTORY ${CEE_GODOT_ROOT}/core/extension
            RESULT_VARIABLE _cee_gen_result
            ERROR_VARIABLE _cee_gen_error
            OUTPUT_QUIET)
        if(_cee_gen_result)
            message(WARNING
                "CrossEngineEditor: failed to generate gdextension_interface.gen.h in ${CEE_GODOT_ROOT} "
                "(${_cee_gen_error}). Build Godot once with scons before compiling the Godot backend.")
        else()
            message(STATUS "CrossEngineEditor: generated gdextension_interface.gen.h in ${CEE_GODOT_ROOT}")
        endif()
    endif()

    # Build-log provenance + header/runtime handshake: the checkout is a plain path (not a pinned
    # submodule) and the libgodot DLL is built independently with scons, so the two can drift.
    # Record the checkout revision and inject its major.minor as compile definitions for the
    # backend's startup version check (GodotBackend.cpp).
    execute_process(COMMAND git -C ${CEE_GODOT_ROOT} describe --tags --always --dirty
        OUTPUT_VARIABLE _cee_godot_desc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_cee_godot_desc)
        message(STATUS "CrossEngineEditor: godot headers = ${_cee_godot_desc} (${CEE_GODOT_ROOT})")
        if(_cee_godot_desc MATCHES "^([0-9]+)\\.([0-9]+)")
            target_compile_definitions(CrossEngine_Godot INTERFACE
                CEE_GODOT_HEADER_VERSION_MAJOR=${CMAKE_MATCH_1}
                CEE_GODOT_HEADER_VERSION_MINOR=${CMAKE_MATCH_2})
        else()
            message(STATUS "CrossEngineEditor: godot describe '${_cee_godot_desc}' has no version "
                           "tag - version check disabled")
        endif()
    else()
        message(STATUS "CrossEngineEditor: godot headers at ${CEE_GODOT_ROOT} (not a git checkout - version check disabled)")
    endif()

    set(CEE_HAVE_GODOT TRUE PARENT_SCOPE)
else()
    if(CEE_ENABLE_GODOT)
        message(WARNING
            "Godot checkout (libgodot.h) not found at ${CEE_GODOT_ROOT}. "
            "Set -DCEE_GODOT_ROOT=<path> to your Godot source. "
            "CrossEngineEditor will build without the Godot backend.")
    endif()
    set(CEE_HAVE_GODOT FALSE PARENT_SCOPE)
endif()

# =======================================================================================
# Tracy profiler (client) - https://github.com/wolfpld/tracy  (submodule pinned to v0.14.1)
#
# Official standard integration (see tracy/manual, "Client markup"): compile the single
# TU public/TracyClient.cpp into a library, expose the public/ include dir, and define
# TRACY_ENABLE on everything that either builds that TU or #includes <tracy/Tracy.hpp>.
# Without TRACY_ENABLE every Tracy macro compiles to nothing, so the same instrumented
# source builds untouched when profiling is off.
#
# We build the client as a STATIC lib (no extra runtime DLL to deploy, matching ADS/
# Diligent), and gate it behind CEE_ENABLE_TRACY so a release build can drop it entirely.
# =======================================================================================
option(CEE_ENABLE_TRACY "Build the Tracy profiler client into CrossEngineEditor" ON)
set(CEE_TRACY_ROOT ${CEE_EXTERNAL_ROOT}/tracy CACHE PATH "Path to the Tracy source checkout")

if(CEE_ENABLE_TRACY AND EXISTS ${CEE_TRACY_ROOT}/public/TracyClient.cpp)
    add_library(CrossEngine_TracyClient STATIC ${CEE_TRACY_ROOT}/public/TracyClient.cpp)

    # TRACY_ENABLE must be set both when compiling TracyClient.cpp and in every consumer TU that
    # includes <tracy/Tracy.hpp>, otherwise client and instrumentation disagree on whether the
    # zones exist (no-op mismatch). PUBLIC on the client TU + on the facade below covers both.
    target_compile_definitions(CrossEngine_TracyClient PUBLIC TRACY_ENABLE)
    target_include_directories(CrossEngine_TracyClient PUBLIC ${CEE_TRACY_ROOT}/public)

    # Call-stack sampling (Windows ETW) is the source of the multi-MB traces and the SymbolWorker /
    # DbgHelp symbol-resolution noise that polluted the §13 profiling. For reading our own
    # instrumentation zones/plots we do NOT want it: keep sampling OFF by default so a trace only
    # carries the ZoneScoped zones and TracyPlot values we explicitly emit. Flip CEE_TRACY_SAMPLING
    # ON (and run the editor as administrator) only when you specifically need the sampled call
    # stacks in the GUI's Statistics -> Sampling page.
    option(CEE_TRACY_SAMPLING "Enable Tracy call-stack / system-trace sampling (noisy, needs admin)" OFF)
    if(NOT CEE_TRACY_SAMPLING)
        # TRACY_NO_SAMPLING: disable per-thread call-stack sampling.
        # TRACY_NO_SYSTEM_TRACING: disable OS-level (ETW) context-switch / systrace capture.
        target_compile_definitions(CrossEngine_TracyClient PUBLIC TRACY_NO_SAMPLING TRACY_NO_SYSTEM_TRACING)
    endif()

    # Tracy's own TU trips O3DE's /W4 /WX; it is third-party, so silence warnings for it only.
    if(MSVC)
        target_compile_options(CrossEngine_TracyClient PRIVATE /W0 /WX-)
    else()
        target_compile_options(CrossEngine_TracyClient PRIVATE -w)
    endif()

    # Tracy's listen socket + symbol resolution need these Win32 libs.
    if(WIN32)
        target_link_libraries(CrossEngine_TracyClient PUBLIC ws2_32 dbghelp)
    endif()

    # Facade the editor links against (single stable name), mirroring CrossEngine::Diligent /
    # CrossEngine::Rbfx. It is also where the walker-cache pre-seed below lives, keeping O3DE's
    # runtime-dependency machinery away from the static client lib.
    add_library(CrossEngine_Tracy INTERFACE)
    add_library(CrossEngine::Tracy ALIAS CrossEngine_Tracy)
    target_link_libraries(CrossEngine_Tracy INTERFACE CrossEngine_TracyClient)

    # Keep O3DE's runtime-dependency walker out of these static libs (see the
    # cee_mark_third_party_subtree_cached macro at the top of this file).
    cee_mark_third_party_subtree_cached(CrossEngine_TracyClient CrossEngine_Tracy)

    set(CEE_HAVE_TRACY TRUE PARENT_SCOPE)
else()
    if(CEE_ENABLE_TRACY)
        message(WARNING
            "Tracy checkout not found at ${CEE_TRACY_ROOT}/public/TracyClient.cpp. "
            "Run: git submodule update --init --recursive (pins v0.14.1). "
            "CrossEngineEditor will build without the Tracy profiler.")
    endif()
    set(CEE_HAVE_TRACY FALSE PARENT_SCOPE)
endif()
