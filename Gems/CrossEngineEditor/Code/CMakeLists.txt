#
# Copyright (c) Cross-Engine Editor Project.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#

if(NOT PAL_TRAIT_BUILD_HOST_TOOLS)
    return()
endif()

# ---------------------------------------------------------------------------------------
# External integrations (git submodules Qt-Advanced-Docking-System / DiligentEngine / tracy,
# plus the rbfx / Godot source checkouts). Sets the CEE_HAVE_* flags and creates the
# ads::qtadvanceddocking / CrossEngine::Diligent / CrossEngine::Rbfx / CrossEngine::Godot /
# CrossEngine::Tracy targets when the sources are present. Included before the target so we
# can conditionally add the dependency + compile definition.
# ---------------------------------------------------------------------------------------
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../External ${CMAKE_CURRENT_BINARY_DIR}/CrossEngineExternal)

# ---------------------------------------------------------------------------------------
# CrossEngineEditor application
#   Strategy B: built directly on AzToolsFramework (Outliner/Inspector/Prefab/AssetBrowser/
#   Gizmo/Undo) with zero engine-runtime dependency. Engines plug in via the backend
#   contract (BackendAPI). See O3DE_CrossEngine_Editor_Executable_Plan.md.
# ---------------------------------------------------------------------------------------

ly_add_target(
    NAME CrossEngineEditor APPLICATION
    NAMESPACE Gem
    AUTOMOC
    AUTOUIC
    AUTORCC
    FILES_CMAKE
        crossengineeditor_files.cmake
    INCLUDE_DIRECTORIES
        PRIVATE
            .
            Source
    BUILD_DEPENDENCIES
        PRIVATE
            3rdParty::Qt::Core
            3rdParty::Qt::Gui
            3rdParty::Qt::OpenGL
            3rdParty::Qt::Widgets
            AZ::AzCore
            AZ::AzFramework
            AZ::AzQtComponents
            AZ::AzToolsFramework
)

# HARD CONSTRAINT (see Plan §A0): all first-party code compiles as C++23.
set_target_properties(CrossEngineEditor PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# ---------------------------------------------------------------------------------------
# Wire the external submodules that are present. Each contributes a link dependency and a
# compile definition so the source can #if on availability without breaking a submodule-
# less checkout.
# ---------------------------------------------------------------------------------------
# Docking is ADS-only since the dock style review round (dock_style_review.md ch.3-1): the
# FancyDocking fallback branch was removed, so the link is unconditional and no
# CEE_HAVE_ADS compile definition is needed anymore (no source consumes it).
ly_target_link_libraries(CrossEngineEditor PRIVATE ads::qtadvanceddocking)

if(PAL_TRAIT_BUILD_EXTERNAL_CRASH_HANDLER_SUPPORTED)
    ly_target_link_libraries(CrossEngineEditor PRIVATE AZ::ToolsCrashHandler)
endif()
if(CEE_HAVE_DILIGENT)
    # Link Diligent with plain target_link_libraries (NOT ly_target_link_libraries): the latter
    # registers the dependency into O3DE's runtime-dependency walker, which recurses Diligent's
    # large static-lib graph and overflows CMake's 1000-deep recursion. Diligent is linked as
    # STATIC libs, so there are no DLLs to copy and nothing for that walker to do anyway.
    target_link_libraries(CrossEngineEditor PRIVATE CrossEngine::Diligent)
    target_compile_definitions(CrossEngineEditor PRIVATE CEE_HAVE_DILIGENT=1)
    # The Diligent backend + debug renderer include Diligent headers, so they are only compiled
    # when the submodule/target is available. Kept out of unity builds because Diligent headers
    # define a lot of macros/enums that clash across translation units when merged.
    target_sources(CrossEngineEditor PRIVATE
        Source/Backends/DiligentBackend.h
        Source/Backends/DiligentBackend.cpp
        Source/Viewport/DiligentDebugRenderer.h
        Source/Viewport/DiligentDebugRenderer.cpp
    )
    set_source_files_properties(
        Source/Backends/DiligentBackend.cpp
        Source/Viewport/DiligentDebugRenderer.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
    )
endif()

if(CEE_HAVE_RBFX)
    # Link rbfx via the External facade with plain target_link_libraries (same rationale as
    # Diligent: keep O3DE's runtime-dependency walker out of the static-lib graph).
    target_link_libraries(CrossEngineEditor PRIVATE CrossEngine::Rbfx)
    target_compile_definitions(CrossEngineEditor PRIVATE CEE_HAVE_RBFX=1)
    # The rbfx backend includes Urho3D/EASTL headers, which define many macros/containers that
    # clash across translation units when merged, so keep it out of unity builds.
    target_sources(CrossEngineEditor PRIVATE
        Source/Backends/RbfxBackend.h
        Source/Backends/RbfxBackend.cpp
        Source/MaterialEditor/RbfxMaterialSource.h
        Source/MaterialEditor/RbfxMaterialSource.cpp
    )
    set_source_files_properties(
        Source/Backends/RbfxBackend.cpp
        Source/MaterialEditor/RbfxMaterialSource.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
    )
    # rbfx public headers pull in rapidjson, which trips C5054 (operator '|' between different
    # enum types). O3DE promotes this to an error via /we5054, and that /weNNNN promotion cannot
    # be undone by an in-header #pragma warning(disable) once the force-included VSCompat re-asserts
    # it per-TU. Demote it back to a (silenced) warning for the whole editor target instead.
    if(MSVC)
        target_compile_options(CrossEngineEditor PRIVATE /wd5054)
    endif()
endif()

if(CEE_HAVE_FILAMENT)
    target_link_libraries(CrossEngineEditor PRIVATE CrossEngine::Filament)
    target_compile_definitions(CrossEngineEditor PRIVATE CEE_HAVE_FILAMENT=1)
    # Filament's headers self-include as <math/...>, <utils/...>, <stb_image.h> - bare names
    # that rbfx's INTERFACE include paths ALSO satisfy (embree's math/, rbfx's vendored
    # stb), and rbfx is linked first so it wins the search order. Force Filament's roots to
    # the FRONT for this target so its headers (and the stb ABI its libs were built against)
    # resolve to the Filament checkout. Keep in sync with the facade list in
    # External/CMakeLists.txt (CrossEngine_Filament target_include_directories).
    target_include_directories(CrossEngineEditor BEFORE PRIVATE
        ${CEE_FILAMENT_ROOT}/filament/include
        ${CEE_FILAMENT_ROOT}/filament/backend/include
        ${CEE_FILAMENT_ROOT}/libs/gltfio/include
        ${CEE_FILAMENT_ROOT}/libs/filabridge/include
        ${CEE_FILAMENT_ROOT}/libs/math/include
        ${CEE_FILAMENT_ROOT}/libs/utils/include
        ${CEE_FILAMENT_PREBUILT}/libs/gltfio
        ${CEE_FILAMENT_ROOT}/third_party/cgltf
        ${CEE_FILAMENT_ROOT}/third_party/robin-map/include
        ${CEE_FILAMENT_ROOT}/third_party/stb)
    # Filament headers define many macros/enums that clash when merged into unity chunks.
    target_sources(CrossEngineEditor PRIVATE
        Source/Backends/FilamentBackend.h
        Source/Backends/FilamentBackend.cpp
        Source/MaterialEditor/FilamentMaterialSource.h
        Source/MaterialEditor/FilamentMaterialSource.cpp
    )
    set_source_files_properties(
        Source/Backends/FilamentBackend.cpp
        Source/MaterialEditor/FilamentMaterialSource.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
    )
endif()

if(CEE_HAVE_GODOT)
    # Godot is embedded via the libgodot DLL loaded at RUNTIME (LoadLibrary), so there is no
    # build-time link dependency — only the header-only facade exposing Godot's include roots.
    target_link_libraries(CrossEngineEditor PRIVATE CrossEngine::Godot)
    target_compile_definitions(CrossEngineEditor PRIVATE CEE_HAVE_GODOT=1)
    # Keep out of unity builds: the Godot GDExtension headers define many macros/enums that
    # clash across translation units when merged (same rationale as rbfx/Diligent).
    target_sources(CrossEngineEditor PRIVATE
        Source/Backends/GodotApi.h
        Source/Backends/GodotBackend.h
        Source/Backends/GodotBackend.cpp
        Source/Backends/GodotMaterialSource.h
        Source/Backends/GodotMaterialSource.cpp
    )
    set_source_files_properties(
        Source/Backends/GodotBackend.cpp
        Source/Backends/GodotMaterialSource.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
    )

    # Copy the runtime libgodot DLL next to the editor executable so `--backend godot` loads it
    # via LoadLibrary without needing it on PATH. Done at BUILD time (cmake -P script) so a DLL
    # produced by scons AFTER configure is still picked up — a configure-time file(GLOB) would
    # miss it. Silently skipped when no DLL exists yet so a header-only configure still builds.
    # Point at your build with -DCEE_GODOT_ROOT=<path>.
    add_custom_command(TARGET CrossEngineEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DCEE_GODOT_ROOT=${CEE_GODOT_ROOT}"
            "-DCEE_GODOT_DLL_DEST=$<TARGET_FILE_DIR:CrossEngineEditor>"
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/CopyLibGodotDll.cmake
        COMMENT "Copying libgodot runtime DLL next to CrossEngineEditor")
endif()

# ---------------------------------------------------------------------------------------
# Tracy profiler client. Linked with plain target_link_libraries so its PUBLIC TRACY_ENABLE
# define + public/ include dir propagate to every CrossEngineEditor TU that #includes
# <tracy/Tracy.hpp> (via Source/Profiling/CrossEngineProfiler.h). CEE_HAVE_TRACY lets the
# profiler wrapper header no-op cleanly when the submodule is absent / disabled.
# ---------------------------------------------------------------------------------------
if(CEE_HAVE_TRACY)
    target_link_libraries(CrossEngineEditor PRIVATE CrossEngine::Tracy)
    target_compile_definitions(CrossEngineEditor PRIVATE CEE_HAVE_TRACY=1)
    # rbfx vendors its OWN (older) copy of Tracy under Source/ThirdParty/tracy/public and adds it
    # to our include path transitively. If <tracy/Tracy.hpp> resolves to THAT copy the client and
    # the instrumentation disagree on the config-mangled symbol names (GetProfiler_CFG_... vs the
    # plain GetProfiler), and the editor fails to link. Force OUR v0.14.1 public/ dir to the FRONT
    # of the search order (BEFORE) so <tracy/Tracy.hpp> always resolves to the same headers that
    # built TracyClient.cpp. Use CEE_TRACY_ROOT (not a relative path) so a -DCEE_TRACY_ROOT
    # override moves both the client TU and this include together.
    target_include_directories(CrossEngineEditor BEFORE PRIVATE
        ${CEE_TRACY_ROOT}/public)
    # Tracy's inline client code (ZoneScoped/plots expanded at every call site) trips a handful of
    # MSVC warnings that O3DE promotes to errors under /WX. They fire in OUR TUs at the macro
    # expansion point, so an in-header #pragma push/pop cannot reliably cover every instantiation;
    # demote them to silenced warnings for the whole editor target (same pattern as rbfx's /wd5054).
    if(MSVC)
        target_compile_options(CrossEngineEditor PRIVATE /wd4366 /wd4267 /wd4244 /wd4324 /wd4245)
    endif()
endif()

ly_set_gem_variant_to_load(TARGETS CrossEngineEditor VARIANTS Tools)

# Inject LY_CMAKE_TARGET so the application can locate its generated dependency .setreg.
set_source_files_properties(
    Source/Application/CrossEngineEditorApplication.cpp
    PROPERTIES
        COMPILE_DEFINITIONS
            LY_CMAKE_TARGET="CrossEngineEditor"
)
