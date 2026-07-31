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

# HARD CONSTRAINT (see plan §0): all first-party code compiles as C++23.
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

if(PAL_TRAIT_BUILD_EXTERNAL_CRASH_HANDLER_SUPPORTED)
    ly_target_link_libraries(CrossEngineEditor PRIVATE AZ::ToolsCrashHandler)
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
