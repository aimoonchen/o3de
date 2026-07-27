/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/std/smart_ptr/unique_ptr.h>

#include <AzFramework/Viewport/ViewportId.h>

#include <AzQtComponents/Components/DockMainWindow.h>
#endif

class QDockWidget;

namespace AzQtComponents
{
    class FancyDocking;
}

namespace CrossEngineEditor
{
    //! Single editor viewport id shared by the viewport widget and the interaction system.
    inline constexpr AzFramework::ViewportId k_viewportId = 0;

    //! Thin docking main window for the cross-engine editor (plan §4 EditorShell/Window).
    //!
    //! Phase 0: stands up the FancyDocking scaffold, menu bar and placeholder panels.
    //! Phase 1 swaps the placeholders for the reused AzToolsFramework widgets
    //! (EntityOutliner / EntityPropertyEditor / AssetBrowser) and the viewport backend.
    class EditorMainWindow : public AzQtComponents::DockMainWindow
    {
        Q_OBJECT
    public:
        explicit EditorMainWindow(QWidget* parent = nullptr);
        ~EditorMainWindow() override;

    private Q_SLOTS:
        //! Create a new entity under the focused/root prefab and select it (plan M1).
        void OnCreateEntity();
        void OnDeleteSelection();

        // Level (root prefab) workflow (plan §6 阶段3.1).
        void OnNewLevel();
        void OnOpenLevel();
        void OnSaveLevel();

        // UX enhancements (plan §5.1/§5.2 / 阶段3.3).
        void OnShowCommandPalette();
        void OnSaveWorkspace();
        void OnRestoreWorkspace();

        // Transform gizmo mode switching (阶段3, engine-native EditorTransformComponentSelection).
        void OnTransformModeMove();
        void OnTransformModeRotate();
        void OnTransformModeScale();

    private:
        void BuildMenuBar();
        void BuildToolBar();
        void BuildDockPanels();

        //! Persist / restore the current dock layout to QSettings under a workspace name.
        void SaveWorkspaceLayout(const QString& name);
        bool RestoreWorkspaceLayout(const QString& name);

        //! Register a dock widget with the fancy docking system in the given area.
        QDockWidget* AddPanel(const QString& objectName, const QString& title, QWidget* content, Qt::DockWidgetArea area);

        //! Collect every QAction reachable from the menus (data source for the command palette).
        QList<QAction*> CollectCommands() const;

        AZStd::unique_ptr<AzQtComponents::FancyDocking> m_fancyDocking;
    };
} // namespace CrossEngineEditor
