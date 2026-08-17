#
# Copyright (c) Cross-Engine Editor Project.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#

set(FILES
    Source/main.cpp
    Source/Profiling/CrossEngineProfiler.h
    Source/Application/CrossEngineEditorApplication.h
    Source/Application/CrossEngineEditorApplication.cpp
    Source/Application/EntityMirrorBridge.h
    Source/Application/EntityMirrorBridge.cpp
    Source/Framework/EngineTransformConverter.h
    Source/Framework/EngineProperty.h
    Source/Framework/EngineProperty.cpp
    Source/Framework/EngineNodeComponent.h
    Source/Framework/EngineNodeComponent.cpp
    Source/Window/EditorMainWindow.h
    Source/Window/EditorMainWindow.cpp
    Source/Window/CeeAssetBrowserPanel.h
    Source/Window/CeeAssetBrowserPanel.cpp
    Source/Window/ResourcePropertiesPanel.h
    Source/Window/ResourcePropertiesPanel.cpp
    Source/Window/CommandPalette.h
    Source/Window/CommandPalette.cpp
    Source/Viewport/EditorViewportWidget.h
    Source/Viewport/EditorViewportWidget.cpp
    Source/Viewport/EngineViewport.h
    Source/Viewport/EngineViewport.cpp
    Source/Viewport/EngineViewportWindow.h
    Source/Viewport/EngineViewportWindow.cpp
    Source/Viewport/GenericDebugDisplay.h
    Source/Viewport/GenericDebugDisplay.cpp
    Source/Viewport/EditorViewportCameraController.h
    Source/Viewport/EditorViewportCameraController.cpp
    Source/Viewport/EditorGrid.h
    Source/Viewport/EditorGrid.cpp
    Source/Viewport/GizmoTheme.h
    Source/Viewport/GizmoTheme.cpp
    Source/Viewport/GizmoViews.h
    Source/Viewport/GizmoViews.cpp
    Source/Viewport/GizmoManager.h
    Source/Viewport/GizmoManager.cpp
    Source/Viewport/CrossEngineViewportSelection.h
    Source/Viewport/CrossEngineViewportSelection.cpp
    Source/Backends/NullBackend.h
    Source/Backends/NullBackend.cpp
    Source/BackendAPI/BackendTypes.h
    Source/BackendAPI/IEngineBackend.h
    Source/BackendAPI/ISceneRenderer.h
    Source/BackendAPI/IEntityMirror.h
    Source/BackendAPI/IAssetSource.h
    Source/BackendAPI/IViewportTick.h
)

# QFileIconProvider/QFileInfo pull in the legacy <winsock.h>, which clashes with the
# <WinSock2.h> other translation units include when merged into a unity chunk. Compile
# the asset-facing files on their own so the socket headers never collide. The
# CeeAssetBrowserPanel is kept out of unity chunks as well: the moc-heavy
# AzToolsFramework AssetBrowser widget headers it includes are fragile there.
set(SKIP_UNITY_BUILD_INCLUSION_FILES
    Source/Backends/NullBackend.cpp
    Source/Window/CeeAssetBrowserPanel.cpp
)
