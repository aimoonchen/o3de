#
# Copyright (c) Cross-Engine Editor Project.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#

if(NOT PAL_TRAIT_BUILD_HOST_TOOLS)
    return()
endif()

# ---------------------------------------------------------------------------------------
# External git submodules (Qt-Advanced-Docking-System, DiligentEngine).
#   Sets CEE_HAVE_ADS / CEE_HAVE_DILIGENT and creates the ads::qtadvanceddocking /
#   CrossEngine::Diligent targets when the submodules are checked out. Included before
#   the target so we can conditionally add the dependency + compile definition.
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
target_compile_features(CrossEngineEditor PRIVATE cxx_std_23)

# ---------------------------------------------------------------------------------------
# Wire the external submodules that are present. Each contributes a link dependency and a
# compile definition so the source can #if on availability without breaking a submodule-
# less checkout.
# ---------------------------------------------------------------------------------------
if(CEE_HAVE_ADS)
    ly_target_link_libraries(CrossEngineEditor PRIVATE ads::qtadvanceddocking)
    target_include_directories(CrossEngineEditor PRIVATE ${CEE_ADS_INCLUDE_DIR})
    target_compile_definitions(CrossEngineEditor PRIVATE CEE_HAVE_ADS=1)
endif()

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
    )
    set_source_files_properties(
        Source/Backends/RbfxBackend.cpp
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

if(CEE_HAVE_GODOT)
    # Godot is embedded via the libgodot DLL loaded at RUNTIME (LoadLibrary), so there is no
    # build-time link dependency — only the header-only facade exposing Godot's include roots.
    target_link_libraries(CrossEngineEditor PRIVATE CrossEngine::Godot)
    target_compile_definitions(CrossEngineEditor PRIVATE CEE_HAVE_GODOT=1)
    # Keep out of unity builds: the Godot GDExtension headers define many macros/enums that
    # clash across translation units when merged (same rationale as rbfx/Diligent).
    target_sources(CrossEngineEditor PRIVATE
        Source/Backends/GodotBackend.h
        Source/Backends/GodotBackend.cpp
    )
    set_source_files_properties(
        Source/Backends/GodotBackend.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
    )

    # Copy the runtime libgodot DLL next to the editor executable so `--backend godot` loads it
    # via LoadLibrary without needing it on PATH. Only if a built DLL exists (scons produces
    # godot.windows.editor.*.dll); silently skipped otherwise so a header-only configure still
    # builds. Point at your build with -DCEE_GODOT_ROOT=<path>.
    file(GLOB _cee_godot_dll "${CEE_GODOT_ROOT}/bin/godot.windows.editor*.x86_64.dll")
    if(_cee_godot_dll)
        list(GET _cee_godot_dll 0 _cee_godot_dll0)
        add_custom_command(TARGET CrossEngineEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cee_godot_dll0}" "$<TARGET_FILE_DIR:CrossEngineEditor>"
            COMMENT "Copying libgodot runtime DLL next to CrossEngineEditor")
    endif()
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
    # plain GetProfiler), and the editor fails to link. Force OUR v0.14.0 public/ dir to the FRONT
    # of the search order (BEFORE) so <tracy/Tracy.hpp> always resolves to the same headers that
    # built TracyClient.cpp.
    target_include_directories(CrossEngineEditor BEFORE PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../External/tracy/public)
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
if(TARGET CrossEngineEditor)
    set_source_files_properties(
        Source/Application/CrossEngineEditorApplication.cpp
        PROPERTIES
            COMPILE_DEFINITIONS
                LY_CMAKE_TARGET="CrossEngineEditor"
    )
else()
    message(FATAL_ERROR "Cannot set LY_CMAKE_TARGET define to CrossEngineEditor as the target doesn't exist anymore. Perhaps it has been renamed")
endif()
