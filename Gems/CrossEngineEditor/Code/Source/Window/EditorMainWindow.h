/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/Component/EntityId.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>

#include <AzFramework/Viewport/ViewportId.h>

#include <AzQtComponents/Components/DockMainWindow.h>
#endif

class QCloseEvent;
class QDockWidget;
class QMenu;

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
    class EntityMirrorBridge;

    //! Single editor viewport id shared by the viewport widget and the interaction system.
    inline constexpr AzFramework::ViewportId k_viewportId = 0;

    //! Thin docking main window for the cross-engine editor (Plan §B1).
    //!
    //! Hosts the reused AzToolsFramework widgets (EntityOutliner / EntityPropertyEditor /
    //! AssetBrowser), the engine viewport, the command palette and the workspace save/restore
    //! (Plan §B5). Docking is dual-path: ADS when CEE_HAVE_ADS is defined, FancyDocking otherwise.
    class EditorMainWindow : public AzQtComponents::DockMainWindow
    {
        Q_OBJECT
    public:
        //! The mirror bridge is the shell's engine <-> editor object sync (used by the
        //! create/delete UI to trigger full re-mirroring after engine-side object changes).
        explicit EditorMainWindow(EntityMirrorBridge* mirrorBridge, QWidget* parent = nullptr);
        ~EditorMainWindow() override;

        //! Bring the Asset Browser dock to the front (BrowseForAssets wiring,
        //! rbfx_migration.md §2.8).
        void ShowAssetBrowser();

    private Q_SLOTS:
        //! Create an empty engine node through the backend mirror (migration P0; replaces the
        //! O3DE prefab CreateEntity path - the engine scene is the single source of truth).
        void OnCreateEntity();
        //! Create a typed engine object chosen from the dynamic Create menu (migration P0,
        //! rbfx_migration.md §3.1 Create-menu data source).
        void OnCreateObjectType();
        void OnDeleteSelection();

        //! Node clipboard (migration P0, §B9): engine-native byte stream via
        //! SerializeNodes/PasteNodes. Cut = copy + delete; duplicate never touches the
        //! clipboard (Blender-style, same engine ops).
        void OnCutSelection();
        void OnCopySelection();
        void OnPasteSelection();
        void OnDuplicateSelection();

        // Level (root prefab) workflow (Plan §B4).
        void OnNewLevel();
        void OnOpenLevel();
        bool OnSaveLevel(); //!< Returns false only when the user cancelled the save dialog.

        //! Export the selection as an engine-native prefab file (migration P0, §3.1 简: the
        //! first selected node's subtree via CreatePrefabFromNodes).
        void OnExportPrefab();

        // UX enhancements (Plan §B5).
        void OnShowCommandPalette();
        void OnSaveWorkspace();
        void OnRestoreWorkspace();

        // Transform gizmo mode switching (Plan §B7, route B: self-drawn GizmoManager via
        // GizmoControlRequestBus, not the engine-native EditorTransformComponentSelection).
        void OnTransformModeSelect();
        void OnTransformModeMove();
        void OnTransformModeRotate();
        void OnTransformModeScale();
        void OnTransformModeCombined();

    protected:
        // QWidget - standard exit flow (rbfx_migration.md §3.4): confirm before leaving,
        // offer a level save. v1 has no dirty tracking, so Save is always offered.
        void closeEvent(QCloseEvent* event) override;

    private:
        void BuildMenuBar();
        void BuildToolBar();
        void BuildDockPanels();

        //! Fill the dynamic Create menu with the backend's EnumerateObjectTypes entries,
        //! grouped by category into submenus. Called once from the menu's aboutToShow (the
        //! backend builds its scene lazily on first surface, so this must not run at menu
        //! construction time; categories are static per engine session).
        void PopulateCreateMenu(QMenu* createMenu);

        //! Serialize the selection to the node clipboard (no-op when nothing selected or the
        //! backend returns an empty stream).
        void CopySelectionToClipboard();

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

        //! Engine <-> editor object sync (not owned; owned by the application, which outlives
        //! the window). Create/delete UI calls RefreshFromEngine through it.
        EntityMirrorBridge* m_mirrorBridge = nullptr;

        //! True once the dynamic Create menu has been filled from EnumerateObjectTypes.
        bool m_createMenuPopulated = false;
    };
} // namespace CrossEngineEditor
