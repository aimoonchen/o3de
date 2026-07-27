#
# Copyright (c) Cross-Engine Editor Project.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#

if(NOT PAL_TRAIT_BUILD_HOST_TOOLS)
    return()
endif()

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
