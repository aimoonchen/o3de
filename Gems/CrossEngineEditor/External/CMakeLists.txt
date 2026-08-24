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
    message(WARNING
        "Qt-Advanced-Docking-System submodule not found at ${CEE_ADS_ROOT}. "
        "Run: git submodule update --init --recursive. "
        "CrossEngineEditor will fall back to AzQtComponents::FancyDocking.")
    set(CEE_HAVE_ADS FALSE PARENT_SCOPE)
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

    # The generated gdextension_interface.gen.h only exists after Godot has been built once.
    # Fall back to the checked-in gdextension_interface.h when the generated one is absent so
    # a from-scratch checkout still configures/compiles the header-facing scaffold.
    if(NOT EXISTS ${CEE_GODOT_ROOT}/core/extension/gdextension_interface.gen.h
       AND EXISTS ${CEE_GODOT_ROOT}/core/extension/gdextension_interface.h)
        target_compile_definitions(CrossEngine_Godot INTERFACE
            CEE_GODOT_GDEXTENSION_HEADER="core/extension/gdextension_interface.h")
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
