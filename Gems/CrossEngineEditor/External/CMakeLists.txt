#
# Copyright (c) Cross-Engine Editor Project.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# ---------------------------------------------------------------------------------------
# External git submodules for CrossEngineEditor.
#
# Both libraries are pulled in as git submodules (see repo-root .gitmodules) and wired
# via their own official CMake, per each project's documentation:
#   - Qt-Advanced-Docking-System : https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System
#   - DiligentEngine             : https://github.com/DiligentGraphics/DiligentEngine
#
# To fetch:  git submodule update --init --recursive
#
# This file is included by Gems/CrossEngineEditor/Code/CMakeLists.txt. It creates the
# imported/interface targets the editor links against:
#   - ads::qtadvanceddocking   (docking system)
#   - CrossEngine::Diligent    (RHI facade, only when DILIGENT is present + enabled)
#
# Design notes:
#   * These are third-party sources compiled inside an O3DE build that enables /WX
#     (warnings-as-errors). We defensively clear /WX for the submodule targets so their
#     warnings never fail our build. O3DE first-party code keeps /WX.
#   * We build ADS as a STATIC library so there is no extra runtime DLL to windeployqt.
# ---------------------------------------------------------------------------------------

set(CEE_EXTERNAL_ROOT ${CMAKE_CURRENT_LIST_DIR})

# =======================================================================================
# Qt-Advanced-Docking-System (ADS)
# =======================================================================================
set(CEE_ADS_ROOT ${CEE_EXTERNAL_ROOT}/Qt-Advanced-Docking-System)

if(EXISTS ${CEE_ADS_ROOT}/CMakeLists.txt)
    # ADS honours these cache options (see its CMakeLists.txt). Build a static lib for Qt6.
    set(ADS_VERSION "" CACHE STRING "" FORCE) # let ADS derive its own version
    set(BUILD_STATIC ON CACHE BOOL "Build ADS as a static library" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(QT_VERSION_MAJOR 6 CACHE STRING "" FORCE)

    # ADS's own CMake calls find_package(QT NAMES Qt6 ...) + find_package(Qt6 ...), which need the
    # umbrella Qt6Config.cmake. O3DE resolves Qt per-component (Qt6Core_DIR, ...) but does not set
    # Qt6_DIR/QT_DIR, so point ADS at the vendored Qt6 cmake package dir (from O3DE's QT_PATH).
    if(DEFINED QT_PATH AND EXISTS ${QT_PATH}/lib/cmake/Qt6/Qt6Config.cmake)
        set(QT_DIR ${QT_PATH}/lib/cmake/Qt6 CACHE PATH "" FORCE)
        set(Qt6_DIR ${QT_PATH}/lib/cmake/Qt6 CACHE PATH "" FORCE)
    endif()

    # ADS's find_package(QT ...) uses versionless Qt; our vendored Qt package already
    # publishes QT_DEFAULT_MAJOR_VERSION=6 and promotes imported targets to GLOBAL, so
    # ads::qtadvanceddocking resolves Qt6::Core/Gui/Widgets from the same SDK.
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
    # Export the include dir so consumers relying on <DockManager.h> resolve it even if
    # the ADS target's INTERFACE include dirs are namespaced.
    set(CEE_ADS_INCLUDE_DIR ${CEE_ADS_ROOT}/src PARENT_SCOPE)
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

    # O3DE's runtime-dependency walker (RuntimeDependencies_common.cmake) recursively follows
    # LINK_LIBRARIES and only marks a target "cached" AFTER descending into its children. Diligent's
    # static libs have cyclic target_link_libraries between the core libs, so that walk recurses
    # until CMake's 1000-deep limit and aborts configuration. Pre-seed the per-target cache so the
    # walker treats the whole Diligent subtree (and our facade) as already-walked leaves. This is
    # correct: they are STATIC libraries with no runtime output to copy to the target directory.
    foreach(_diligent_cached ${_cee_diligent_link} CrossEngine_Diligent)
        set_property(GLOBAL PROPERTY O3DE_COPY_DEPENDENCIES_${_diligent_cached} "")
        set_property(GLOBAL PROPERTY O3DE_TARGET_DEPENDENCIES_${_diligent_cached} "")
        set_property(GLOBAL PROPERTY O3DE_LINK_DEPENDENCIES_${_diligent_cached} "")
        set_property(GLOBAL PROPERTY O3DE_IMPORTED_DEPENDENCIES_${_diligent_cached} "")
        set_property(GLOBAL PROPERTY O3DE_DEPENDENCIES_CACHED_${_diligent_cached} TRUE)
    endforeach()

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
