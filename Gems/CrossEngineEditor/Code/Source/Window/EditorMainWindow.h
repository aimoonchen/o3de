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

#if defined(CEE_HAVE_ADS)
namespace ads
{
    class CDockManager;
    class CDockWidget;
} // namespace ads
#endif

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
        void OnTransformModeSelect();
        void OnTransformModeMove();
        void OnTransformModeRotate();
        void OnTransformModeScale();
        void OnTransformModeCombined();

    private:
        void BuildMenuBar();
        void BuildToolBar();
        void BuildDockPanels();

        //! Persist / restore the current dock layout to QSettings under a workspace name.
        void SaveWorkspaceLayout(const QString& name);
        bool RestoreWorkspaceLayout(const QString& name);

        //! Register a dock panel in the given area. With ADS it wraps the content in an
        //! ads::CDockWidget and adds it to the dock manager; otherwise it uses a QDockWidget
        //! with the FancyDocking scaffold. Returns nullptr on the ADS path (caller ignores it).
        QDockWidget* AddPanel(const QString& objectName, const QString& title, QWidget* content, Qt::DockWidgetArea area);

        //! Collect every QAction reachable from the menus (data source for the command palette).
        QList<QAction*> CollectCommands() const;

#if defined(CEE_HAVE_ADS)
        //! Qt-Advanced-Docking-System manager. Owns all dock widgets and the layout state.
        //! Parented to this QMainWindow, so Qt destroys it - do not wrap in unique_ptr.
        ads::CDockManager* m_dockManager = nullptr;
#else
        AZStd::unique_ptr<AzQtComponents::FancyDocking> m_fancyDocking;
#endif
    };
} // namespace CrossEngineEditor
