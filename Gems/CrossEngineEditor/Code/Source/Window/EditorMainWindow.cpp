/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/EditorMainWindow.h>
#include <Window/CeeAssetBrowserPanel.h>
#include <Window/CommandPalette.h>
#include <Window/ResourcePropertiesPanel.h>
#include <Viewport/GizmoManager.h>
#include <Viewport/EditorViewportWidget.h>
#include <Application/EntityMirrorBridge.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>
#include <Framework/EngineNodeComponent.h>

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/IO/FileIO.h>
#include <AzCore/IO/GenericStreams.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/functional.h>

#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Entity/PrefabEditorEntityOwnershipInterface.h>
#include <AzToolsFramework/Prefab/PrefabPublicInterface.h>
#include <AzToolsFramework/UI/Logging/TracePrintFLogPanel.h>
#include <AzToolsFramework/UI/Outliner/EntityOutlinerWidget.hxx>
#include <AzToolsFramework/UI/PropertyEditor/EntityPropertyEditor.hxx>

#include <AzQtComponents/Components/FancyDocking.h>

#if defined(CEE_HAVE_ADS)
AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <DockManager.h>
#include <DockWidget.h>
#include <DockAreaWidget.h>
AZ_POP_DISABLE_WARNING
#endif

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QAction>
#include <QActionGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QToolBar>
AZ_POP_DISABLE_WARNING

namespace
{
    // Node-clipboard mime (migration P0, §B9). Deliberately NOT an O3DE slice/prefab mime:
    // the payload is the engine's own serialization (rbfx Node::SaveXML bytes), and other O3DE
    // tooling must not mistake it for an entity prefab.
    const QString k_nodeClipboardMimeType = QStringLiteral("application/x-crossengineditor-nodes");
} // namespace

namespace CrossEngineEditor
{
    EditorMainWindow::EditorMainWindow(EntityMirrorBridge* mirrorBridge, QWidget* parent)
        : AzQtComponents::DockMainWindow(parent)
        , m_mirrorBridge(mirrorBridge)
    {
        setWindowTitle(QStringLiteral("Cross-Engine Editor"));
        setObjectName(QStringLiteral("CrossEngineEditorMainWindow"));
        resize(1600, 900);

#if defined(CEE_HAVE_ADS)
        // Qt-Advanced-Docking-System. The manager registers itself as the central widget of
        // this QMainWindow, so it must be created before any dock panels are added. It owns
        // all CDockWidgets and provides saveState/restoreState for workspaces.
        ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
        ads::CDockManager::setConfigFlag(ads::CDockManager::XmlCompressionEnabled, false);
        ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
        ads::CDockManager::setAutoHideConfigFlags(ads::CDockManager::DefaultAutoHideConfig);
        m_dockManager = new ads::CDockManager(this);
#else
        // Visual Studio style docking. saveState/restoreState must go through this
        // instance (see Workspaces, Plan §B5).
        m_fancyDocking = AZStd::make_unique<AzQtComponents::FancyDocking>(this);
#endif

        BuildMenuBar();
        BuildToolBar();
        BuildDockPanels();
    }

    EditorMainWindow::~EditorMainWindow() = default;

    void EditorMainWindow::BuildMenuBar()
    {
        QMenuBar* bar = menuBar();

        QMenu* fileMenu = bar->addMenu(QStringLiteral("&File"));
        QAction* newLevel = fileMenu->addAction(QStringLiteral("&New Level"));
        newLevel->setShortcut(QKeySequence::New);
        connect(newLevel, &QAction::triggered, this, &EditorMainWindow::OnNewLevel);
        QAction* openLevel = fileMenu->addAction(QStringLiteral("&Open Level..."));
        openLevel->setShortcut(QKeySequence::Open);
        connect(openLevel, &QAction::triggered, this, &EditorMainWindow::OnOpenLevel);
        QAction* saveLevel = fileMenu->addAction(QStringLiteral("&Save Level..."));
        saveLevel->setShortcut(QKeySequence::Save);
        connect(saveLevel, &QAction::triggered, this, &EditorMainWindow::OnSaveLevel);
        QAction* exportPrefab = fileMenu->addAction(QStringLiteral("Export Prefab..."));
        connect(exportPrefab, &QAction::triggered, this, &EditorMainWindow::OnExportPrefab);

        QMenu* editMenu = bar->addMenu(QStringLiteral("&Edit"));
        // Shortcut context deliberately matches O3DE's ActionManager (EditorAction.cpp sets every
        // editor action to Qt::WidgetWithChildrenShortcut). O3DE does NOT rely on Qt's native
        // QShortcutMap for cross-widget firing: it installs an ActionContextWidgetWatcher on the
        // main window that intercepts the bubbling QEvent::ShortcutOverride and triggers matching
        // actions manually. Keeping WidgetWithChildrenShortcut (NOT ApplicationShortcut) means these
        // actions carry over unchanged when the O3DE ActionManager is adopted.
        // RISK to verify: our viewport is a native QWindow behind createWindowContainer, unlike
        // O3DE's QWidget+winId viewport. ShortcutOverride must still bubble across the window-
        // container boundary up to this window; if it does not, a QWindow->main-window shortcut
        // bridge is required (see Plan §B8 B2).
        // Create submenu (migration P0): "Empty Node" (Ctrl+Shift+N) + the engine's creatable
        // object types, filled lazily on first open from EnumerateObjectTypes (the backend
        // builds its scene lazily on first surface, so enumeration must wait for the menu to
        // actually open). See PopulateCreateMenu.
        QMenu* createMenu = editMenu->addMenu(QStringLiteral("&Create"));
        QAction* createEmptyNode = createMenu->addAction(QStringLiteral("Empty Node"));
        createEmptyNode->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        createEmptyNode->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
        addAction(createEmptyNode);
        connect(createEmptyNode, &QAction::triggered, this, &EditorMainWindow::OnCreateEntity);
        connect(createMenu, &QMenu::aboutToShow, this, [this, createMenu]
        {
            if (!m_createMenuPopulated)
            {
                PopulateCreateMenu(createMenu);
            }
        });

        // Node clipboard (migration P0, §B9). Same shortcut context as the rest of the Edit
        // menu (see the WidgetWithChildrenShortcut comment above): focused text widgets still
        // consume Ctrl+C/V/X themselves, so Inspector editing is unaffected.
        QAction* cutSelection = editMenu->addAction(QStringLiteral("Cu&t"));
        cutSelection->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        cutSelection->setShortcut(QKeySequence::Cut);
        addAction(cutSelection);
        connect(cutSelection, &QAction::triggered, this, &EditorMainWindow::OnCutSelection);

        QAction* copySelection = editMenu->addAction(QStringLiteral("&Copy"));
        copySelection->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        copySelection->setShortcut(QKeySequence::Copy);
        addAction(copySelection);
        connect(copySelection, &QAction::triggered, this, &EditorMainWindow::OnCopySelection);

        QAction* pasteSelection = editMenu->addAction(QStringLiteral("&Paste"));
        pasteSelection->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        pasteSelection->setShortcut(QKeySequence::Paste);
        addAction(pasteSelection);
        connect(pasteSelection, &QAction::triggered, this, &EditorMainWindow::OnPasteSelection);

        QAction* duplicateSelection = editMenu->addAction(QStringLiteral("&Duplicate"));
        duplicateSelection->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        duplicateSelection->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
        addAction(duplicateSelection);
        connect(duplicateSelection, &QAction::triggered, this, &EditorMainWindow::OnDuplicateSelection);

        QAction* deleteSelection = editMenu->addAction(QStringLiteral("&Delete Selection"));
        deleteSelection->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        deleteSelection->setShortcut(QKeySequence::Delete);
        addAction(deleteSelection);
        connect(deleteSelection, &QAction::triggered, this, &EditorMainWindow::OnDeleteSelection);

        bar->addMenu(QStringLiteral("&View"));

        // Workspaces (Blender-style saved layouts, Plan §B5).
        QMenu* windowMenu = bar->addMenu(QStringLiteral("&Window"));
        QAction* saveWorkspace = windowMenu->addAction(QStringLiteral("Save &Workspace"));
        connect(saveWorkspace, &QAction::triggered, this, &EditorMainWindow::OnSaveWorkspace);
        QAction* restoreWorkspace = windowMenu->addAction(QStringLiteral("&Restore Workspace"));
        connect(restoreWorkspace, &QAction::triggered, this, &EditorMainWindow::OnRestoreWorkspace);

        // Command palette (VSCode/Blender-style, Plan §B5). A window-level shortcut so it
        // is reachable regardless of which docked panel holds focus.
        QAction* commandPalette = windowMenu->addAction(QStringLiteral("Command &Palette"));
        commandPalette->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
        commandPalette->setShortcutContext(Qt::ApplicationShortcut);
        addAction(commandPalette);
        connect(commandPalette, &QAction::triggered, this, &EditorMainWindow::OnShowCommandPalette);

        bar->addMenu(QStringLiteral("&Help"));
    }

    void EditorMainWindow::BuildToolBar()
    {
        // Gizmo mode switcher. Both the look AND the behaviour are ours: each button drives our
        // GizmoManager (route B, self-drawn gizmos), not the engine-native transform selection.
        // Hot-keys follow the industry standard Q/W/E/R (Unity/Unreal/Godot: Move=W, Rotate=E,
        // Scale=R); Q=Select (unobstructed picking) and T=Combined. The legacy 1/2/3/4 keys are
        // kept as alternates so existing muscle memory still works.
        auto* toolBar = addToolBar(QStringLiteral("Transform"));
        toolBar->setObjectName(QStringLiteral("TransformToolBar"));

        auto addModeAction = [this, toolBar](
            const QString& text, const QString& primaryKey, const QString& altKey, void (EditorMainWindow::*slot)())
        {
            QAction* action = toolBar->addAction(text);
            action->setCheckable(true);
            // Primary industry-standard key plus the legacy numeric key as an alternate.
            action->setShortcuts(QList<QKeySequence>{ QKeySequence(primaryKey), QKeySequence(altKey) });
            // Same context as the Edit actions above (O3DE ActionManager alignment): the toolbar
            // lives on the main window, so WidgetWithChildrenShortcut lets the key fire while any
            // docked child (incl. the viewport, pending the ShortcutOverride bubble verification)
            // holds focus, without an ApplicationShortcut global hijack.
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            connect(action, &QAction::triggered, this, slot);
            return action;
        };

        auto* group = new QActionGroup(this);
        group->setExclusive(true);
        QAction* select = addModeAction(
            QStringLiteral("Select"), QStringLiteral("Q"), QString(), &EditorMainWindow::OnTransformModeSelect);
        QAction* move = addModeAction(
            QStringLiteral("Move"), QStringLiteral("W"), QStringLiteral("1"), &EditorMainWindow::OnTransformModeMove);
        QAction* rotate = addModeAction(
            QStringLiteral("Rotate"), QStringLiteral("E"), QStringLiteral("2"), &EditorMainWindow::OnTransformModeRotate);
        QAction* scale = addModeAction(
            QStringLiteral("Scale"), QStringLiteral("R"), QStringLiteral("3"), &EditorMainWindow::OnTransformModeScale);
        QAction* combined = addModeAction(
            QStringLiteral("Combined"), QStringLiteral("T"), QStringLiteral("4"), &EditorMainWindow::OnTransformModeCombined);
        group->addAction(select);
        group->addAction(move);
        group->addAction(rotate);
        group->addAction(scale);
        group->addAction(combined);
        select->setChecked(true); // default mode is Select (industry norm: don't grab a gizmo on entry).

        // Gizmo theme switcher (Blender / Unreal, Plan §B7). Rebuilds the gizmo views on change.
        toolBar->addSeparator();
        auto* themeGroup = new QActionGroup(this);
        themeGroup->setExclusive(true);
        auto addThemeAction = [this, toolBar, themeGroup](const QString& text, GizmoStyle style)
        {
            QAction* action = toolBar->addAction(text);
            action->setCheckable(true);
            connect(action, &QAction::triggered, this, [style]()
            {
                GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetGizmoStyle, style);
            });
            themeGroup->addAction(action);
            return action;
        };
        QAction* blender = addThemeAction(QStringLiteral("Blender"), GizmoStyle::Blender);
        addThemeAction(QStringLiteral("Unreal"), GizmoStyle::Unreal);
        blender->setChecked(true);

        // Coordinate space switcher (World / Local, Plan §B5). World = axis-aligned handles;
        // Local aligns them to the entity's orientation.
        toolBar->addSeparator();
        auto* spaceGroup = new QActionGroup(this);
        spaceGroup->setExclusive(true);
        auto addSpaceAction = [this, toolBar, spaceGroup](const QString& text, GizmoSpace space)
        {
            QAction* action = toolBar->addAction(text);
            action->setCheckable(true);
            connect(action, &QAction::triggered, this, [space]()
            {
                GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetGizmoSpace, space);
            });
            spaceGroup->addAction(action);
            return action;
        };
        QAction* worldSpace = addSpaceAction(QStringLiteral("World"), GizmoSpace::World);
        addSpaceAction(QStringLiteral("Local"), GizmoSpace::Local);
        worldSpace->setChecked(true); // default space is World.

        // Snap toggle (Plan §B5). One checkbox flips grid + angle + scale snap on/off, preserving
        // the current style's step increments; per-increment tuning is deferred to a settings panel.
        toolBar->addSeparator();
        QAction* snap = toolBar->addAction(QStringLiteral("Snap"));
        snap->setCheckable(true);
        connect(snap, &QAction::toggled, this, [](bool enabled)
        {
            GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetSnapEnabled, enabled);
        });
    }

    void EditorMainWindow::OnTransformModeSelect()
    {
        GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetGizmoMode, GizmoMode::Select);
    }

    void EditorMainWindow::OnTransformModeMove()
    {
        GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetGizmoMode, GizmoMode::Move);
    }

    void EditorMainWindow::OnTransformModeRotate()
    {
        GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetGizmoMode, GizmoMode::Rotate);
    }

    void EditorMainWindow::OnTransformModeScale()
    {
        GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetGizmoMode, GizmoMode::Scale);
    }

    void EditorMainWindow::OnTransformModeCombined()
    {
        GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetGizmoMode, GizmoMode::Combined);
    }

    void EditorMainWindow::BuildDockPanels()
    {
        // Engine-agnostic viewport surface. The manipulator/selection logic runs against
        // its camera state; the active backend renders overlay geometry into it (Plan §B2).
        auto* viewport = new EditorViewportWidget(k_viewportId, m_mirrorBridge, this);
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (backend)
        {
            viewport->SetSceneRenderer(&backend->GetSceneRenderer());
        }

        // Reused AzToolsFramework widgets - they self-wire to the editor entity/selection
        // buses, so selection stays in sync across Outliner and Inspector for free.
        auto* outliner = new AzToolsFramework::EntityOutlinerWidget(this);
        auto* inspector = new AzToolsFramework::EntityPropertyEditor(this);

        // Reused AzToolsFramework AssetBrowser (model/filter/tree) fed by the engine
        // backend's asset source (Plan §A6 C4 资产面, rbfx_migration.md §2).
        CeeAssetBrowserPanel* assetBrowser = backend
            ? new CeeAssetBrowserPanel(&backend->GetAssetSource(), m_mirrorBridge, this)
            : nullptr;
        QWidget* assetPanel = assetBrowser
            ? static_cast<QWidget*>(assetBrowser)
            : static_cast<QWidget*>(new QLabel(QStringLiteral("Asset Browser"), this));

        // Generic resource property panel (rbfx_migration.md §3.3): the O3DE generic
        // reflection grid absorbs the engine's per-type resource editors. Opened by
        // AssetBrowser double-click on .material/.mat/.ani via the handler below.
        auto* resourcePanel = new ResourcePropertiesPanel(m_mirrorBridge, this);
        if (assetBrowser)
        {
            assetBrowser->SetResourcePropertiesPanel(resourcePanel);
        }

        // Console (rbfx_migration.md §3.4): the official trace log panel - a 100% reused
        // AzToolsFramework control listening on the AZ trace bus, so AZ_Warning/AZ_Error
        // output from the backends and the shell lands here.
        auto* console = new AzToolsFramework::LogPanel::TracePrintFLogPanel();

#if defined(CEE_HAVE_ADS)
        // ADS models the viewport as the central dock widget (non-closable, non-floatable),
        // with the tool panels docked around it. This gives Blender/VS-style "dock anywhere"
        // while keeping the viewport as the stable anchor.
        auto* viewportDock = new ads::CDockWidget(m_dockManager, QStringLiteral("Viewport"), this);
        viewportDock->setObjectName(QStringLiteral("ViewportDock"));
        viewportDock->setWidget(viewport);
        viewportDock->setFeature(ads::CDockWidget::DockWidgetClosable, false);
        viewportDock->setFeature(ads::CDockWidget::DockWidgetMovable, false);
        viewportDock->setFeature(ads::CDockWidget::DockWidgetFloatable, false);
        ads::CDockAreaWidget* centralArea = m_dockManager->setCentralWidget(viewportDock);
        (void)centralArea;

        AddPanel(QStringLiteral("OutlinerDock"), QStringLiteral("Entity Outliner"), outliner, Qt::LeftDockWidgetArea);
        AddPanel(QStringLiteral("InspectorDock"), QStringLiteral("Inspector"), inspector, Qt::RightDockWidgetArea);
        AddPanel(QStringLiteral("ResourceInspectorDock"), QStringLiteral("Resource Inspector"), resourcePanel, Qt::RightDockWidgetArea);
        AddPanel(QStringLiteral("AssetBrowserDock"), QStringLiteral("Asset Browser"), assetPanel, Qt::BottomDockWidgetArea);
        AddPanel(QStringLiteral("ConsoleDock"), QStringLiteral("Console"), console, Qt::BottomDockWidgetArea);
#else
        setCentralWidget(viewport);
        AddPanel(QStringLiteral("OutlinerDock"), QStringLiteral("Entity Outliner"), outliner, Qt::LeftDockWidgetArea);
        AddPanel(QStringLiteral("InspectorDock"), QStringLiteral("Inspector"), inspector, Qt::RightDockWidgetArea);
        AddPanel(QStringLiteral("ResourceInspectorDock"), QStringLiteral("Resource Inspector"), resourcePanel, Qt::RightDockWidgetArea);
        AddPanel(QStringLiteral("AssetBrowserDock"), QStringLiteral("Asset Browser"), assetPanel, Qt::BottomDockWidgetArea);
        AddPanel(QStringLiteral("ConsoleDock"), QStringLiteral("Console"), console, Qt::BottomDockWidgetArea);
#endif
    }

    void EditorMainWindow::ShowAssetBrowser()
    {
#if defined(CEE_HAVE_ADS)
        // ADS: bring the Asset Browser tab forward and make it the current tab.
        if (ads::CDockWidget* dock = m_dockManager->findDockWidget(QStringLiteral("AssetBrowserDock")))
        {
            dock->toggleView(true);
            dock->setAsCurrentTab();
        }
#else
        if (QDockWidget* dock = findChild<QDockWidget*>(QStringLiteral("AssetBrowserDock")))
        {
            dock->show();
            dock->raise();
        }
#endif
    }

    QDockWidget* EditorMainWindow::AddPanel(
        const QString& objectName, const QString& title, QWidget* content, Qt::DockWidgetArea area)
    {
#if defined(CEE_HAVE_ADS)
        // Wrap the content in an ADS dock widget and add it to the manager. ADS uses its own
        // DockWidgetArea enum; map the classic Qt areas to it.
        auto* dock = new ads::CDockWidget(m_dockManager, title, this);
        dock->setObjectName(objectName);
        dock->setWidget(content);

        ads::DockWidgetArea adsArea = ads::CenterDockWidgetArea;
        switch (area)
        {
        case Qt::LeftDockWidgetArea:   adsArea = ads::LeftDockWidgetArea;   break;
        case Qt::RightDockWidgetArea:  adsArea = ads::RightDockWidgetArea;  break;
        case Qt::TopDockWidgetArea:    adsArea = ads::TopDockWidgetArea;    break;
        case Qt::BottomDockWidgetArea: adsArea = ads::BottomDockWidgetArea; break;
        default:                       adsArea = ads::CenterDockWidgetArea; break;
        }
        m_dockManager->addDockWidget(adsArea, dock);
        return nullptr; // ADS owns the widget; callers do not use the return value.
#else
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(objectName);
        dock->setWidget(content);
        addDockWidget(area, dock);
        return dock;
#endif
    }

    void EditorMainWindow::OnCreateEntity()
    {
        // Migration P0: with an engine backend the engine's native scene is the single source
        // of truth, so creation goes through the mirror contract (rbfx: a bare scene Node)
        // instead of the O3DE prefab path. The rbfx mirror deliberately returns an invalid id -
        // the mirror entity is built on the next full re-sync, which RefreshFromEngine runs.
        AZ_Printf("CrossEngineEditor", "OnCreateEntity invoked.\n");

        if (IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get())
        {
            backend->GetEntityMirror().CreateObject(ObjectSpec{});
            if (m_mirrorBridge)
            {
                m_mirrorBridge->RefreshFromEngine();
            }
        }
    }

    void EditorMainWindow::OnCreateObjectType()
    {
        // Create-menu entry: the action's data holds the engine typeId from
        // EnumerateObjectTypes; everything else matches OnCreateEntity.
        auto* action = qobject_cast<QAction*>(sender());
        if (!action)
        {
            return;
        }

        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            return;
        }

        ObjectSpec spec;
        spec.m_typeId = action->data().toString().toUtf8().constData();
        AZ_Printf("CrossEngineEditor", "OnCreateObjectType invoked for type '%s'.\n", spec.m_typeId.c_str());

        backend->GetEntityMirror().CreateObject(spec);
        if (m_mirrorBridge)
        {
            m_mirrorBridge->RefreshFromEngine();
        }
    }

    void EditorMainWindow::OnDeleteSelection()
    {
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            // No engine: plain editor-owned entity deletion.
            AzToolsFramework::ToolsApplicationRequestBus::Broadcast(
                &AzToolsFramework::ToolsApplicationRequests::DeleteSelected);
            return;
        }

        IEntityMirror& mirror = backend->GetEntityMirror();

        // Engine side first: destroy the engine nodes behind the selected mirror entities
        // WHILE they still resolve. OnEditorEntityDeleted fires only after editor-side
        // removal, at which point ResolveNode can no longer find the live entity, so this
        // must precede DeleteSelected.
        AzToolsFramework::EntityIdList selection;
        AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(
            selection, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
        for (const AZ::EntityId entityId : selection)
        {
            AZ::Entity* entity = nullptr;
            AZ::ComponentApplicationBus::BroadcastResult(
                entity, &AZ::ComponentApplicationBus::Events::FindEntity, entityId);
            if (entity && entity->FindComponent<EngineNodeComponent>())
            {
                mirror.DestroyObject(entityId);
            }
        }

        // Editor-side removal (undoable), then a full re-mirror so ghosts (e.g. unselected
        // subtree children whose engine nodes went away with their parent) disappear.
        AzToolsFramework::ToolsApplicationRequestBus::Broadcast(
            &AzToolsFramework::ToolsApplicationRequests::DeleteSelected);
        if (m_mirrorBridge)
        {
            m_mirrorBridge->RefreshFromEngine();
        }
    }

    void EditorMainWindow::OnCutSelection()
    {
        // Cut = copy to the node clipboard, then the normal delete flow (engine nodes destroyed
        // first, editor-side removal, re-mirror).
        CopySelectionToClipboard();
        OnDeleteSelection();
    }

    void EditorMainWindow::OnCopySelection()
    {
        CopySelectionToClipboard();
    }

    void EditorMainWindow::OnPasteSelection()
    {
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            return;
        }

        const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
        if (!mime || !mime->hasFormat(k_nodeClipboardMimeType))
        {
            return;
        }

        const QByteArray raw = mime->data(k_nodeClipboardMimeType);
        if (raw.isEmpty())
        {
            return;
        }
        const AZStd::vector<AZ::u8> bytes(raw.cbegin(), raw.cend());

        // v1: paste under the first selected mirror entity, else the scene root (PasteNodes
        // treats an invalid parent id as the root and bypasses ResolveNode warnings).
        backend->GetEntityMirror().PasteNodes(
            bytes, m_mirrorBridge ? m_mirrorBridge->FirstSelectedMirrorId() : AZ::EntityId());
        if (m_mirrorBridge)
        {
            m_mirrorBridge->RefreshFromEngine();
        }
    }

    void EditorMainWindow::OnDuplicateSelection()
    {
        // Blender-style duplicate: serialize + paste without touching the clipboard. v1 keeps
        // the same "under the first selected mirror" placement as paste (a duplicate of a node
        // pasted under itself lands as its child - documented v1 简 simplification).
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            return;
        }

        AzToolsFramework::EntityIdList selection;
        AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(
            selection, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
        const AZStd::vector<AZ::u8> bytes = backend->GetEntityMirror().SerializeNodes(selection);
        if (bytes.empty())
        {
            return;
        }

        backend->GetEntityMirror().PasteNodes(
            bytes, m_mirrorBridge ? m_mirrorBridge->FirstSelectedMirrorId() : AZ::EntityId());
        if (m_mirrorBridge)
        {
            m_mirrorBridge->RefreshFromEngine();
        }
    }

    void EditorMainWindow::CopySelectionToClipboard()
    {
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            return;
        }

        AzToolsFramework::EntityIdList selection;
        AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(
            selection, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
        const AZStd::vector<AZ::u8> bytes = backend->GetEntityMirror().SerializeNodes(selection);
        if (bytes.empty())
        {
            return;
        }

        auto* mime = new QMimeData;
        mime->setData(
            k_nodeClipboardMimeType,
            QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())));
        QGuiApplication::clipboard()->setMimeData(mime);
    }

    void EditorMainWindow::closeEvent(QCloseEvent* event)
    {
        // Migration L1 (rbfx_migration.md §3.4 关闭对话框): confirm before leaving, offer a level save. v1 has
        // no dirty tracking, so Save is always offered and always saves when chosen. Closing
        // the box (X/Escape) resolves to Cancel, which keeps the editor open.
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this,
            QStringLiteral("Exit Editor"),
            QStringLiteral("Save changes before exiting?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer == QMessageBox::Cancel)
        {
            event->ignore();
            return;
        }
        if (answer == QMessageBox::Save && !OnSaveLevel())
        {
            event->ignore(); // User cancelled the save dialog - keep the editor open.
            return;
        }
        event->accept();
    }

    void EditorMainWindow::PopulateCreateMenu(QMenu* createMenu)
    {
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            return;
        }

        AZStd::vector<ObjectTypeInfo> types;
        backend->GetEntityMirror().EnumerateObjectTypes(types);
        if (types.empty())
        {
            return; // Latch stays unset: the menu retries on the next open (the backend
                    // builds its scene lazily on first surface).
        }
        m_createMenuPopulated = true;

        // Backend guarantees category-then-typeId ordering, so equal consecutive categories
        // group into one submenu without a map.
        createMenu->addSeparator();
        QMenu* categoryMenu = nullptr;
        QString currentCategory;
        for (const ObjectTypeInfo& type : types)
        {
            const QString category = QString::fromUtf8(type.m_category.c_str());
            if (category != currentCategory)
            {
                currentCategory = category;
                categoryMenu = category.isEmpty() ? createMenu : createMenu->addMenu(category);
            }

            QAction* action = categoryMenu->addAction(QString::fromUtf8(type.m_displayName.c_str()));
            action->setData(QString::fromUtf8(type.m_typeId.c_str()));
            connect(action, &QAction::triggered, this, &EditorMainWindow::OnCreateObjectType);
        }
    }

    void EditorMainWindow::OnNewLevel()
    {
        // Recreate the level as a fresh in-memory root prefab (Plan §B4).
        if (auto* ownership = AZ::Interface<AzToolsFramework::PrefabEditorEntityOwnershipInterface>::Get())
        {
            ownership->CreateNewLevelPrefab("NewLevel.prefab", "");
        }
    }

    void EditorMainWindow::OnOpenLevel()
    {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Level"), QString(), QStringLiteral("Prefab (*.prefab)"));
        if (path.isEmpty())
        {
            return;
        }

        auto* ownership = AZ::Interface<AzToolsFramework::PrefabEditorEntityOwnershipInterface>::Get();
        if (!ownership)
        {
            return;
        }

        // Load the level root prefab through the ownership service (same path the engine uses).
        AZ::IO::FileIOStream stream(path.toUtf8().constData(), AZ::IO::OpenMode::ModeRead);
        if (!stream.IsOpen())
        {
            AZ_Warning("CrossEngineEditor", false, "OpenLevel: could not open %s", path.toUtf8().constData());
            return;
        }
        if (!ownership->LoadFromStream(stream, path.toUtf8().constData()))
        {
            AZ_Warning("CrossEngineEditor", false, "OpenLevel: LoadFromStream failed for %s", path.toUtf8().constData());
        }
    }

    bool EditorMainWindow::OnSaveLevel()
    {
        // Plan §B4: with an engine backend the engine's native scene is the single source of
        // truth, so route Save to the backend (Godot PackedScene+ResourceSaver / rbfx SaveFile).
        // Passing an empty path lets the backend save to the scene's current file. If the backend
        // can't save (null/diligent, or no scene), fall back to the O3DE prefab path below.
        // Returns false only when the user cancelled the fallback save dialog, so closeEvent
        // can keep the editor open; write failures warn on the Console and still return true.
        if (IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get())
        {
            if (backend->GetEntityMirror().SaveScene(AZStd::string{}))
            {
                return true;
            }
        }

        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Level"), QStringLiteral("NewLevel.prefab"), QStringLiteral("Prefab (*.prefab)"));
        if (path.isEmpty())
        {
            return false;
        }

        auto* ownership = AZ::Interface<AzToolsFramework::PrefabEditorEntityOwnershipInterface>::Get();
        if (!ownership)
        {
            return true;
        }

        AZ::IO::FileIOStream stream(path.toUtf8().constData(), AZ::IO::OpenMode::ModeWrite);
        if (!stream.IsOpen())
        {
            AZ_Warning("CrossEngineEditor", false, "SaveLevel: could not open %s for write", path.toUtf8().constData());
            return true;
        }
        if (!ownership->SaveToStream(stream, path.toUtf8().constData()))
        {
            AZ_Warning("CrossEngineEditor", false, "SaveLevel: SaveToStream failed for %s", path.toUtf8().constData());
        }
        return true;
    }

    void EditorMainWindow::OnExportPrefab()
    {
        // Migration P0 (§3.1 简): export the first selected mirror node's subtree to the
        // engine's own prefab format (rbfx: Node::SaveXML; a Model-loadable XML gets picked up
        // by the drop/spawn path).
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            return;
        }

        AzToolsFramework::EntityIdList selection;
        AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(
            selection, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
        if (selection.empty())
        {
            AZ_Warning("CrossEngineEditor", false, "ExportPrefab: nothing selected.");
            return;
        }

        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export Prefab"), QString(), QStringLiteral("Prefab XML (*.xml *.prefab)"));
        if (path.isEmpty())
        {
            return;
        }

        const bool ok = backend->GetEntityMirror().CreatePrefabFromNodes(
            selection, AZStd::string(path.toUtf8().constData()));
        AZ_Warning("CrossEngineEditor", ok, "ExportPrefab failed for %s.", path.toUtf8().constData());
    }

    QList<QAction*> EditorMainWindow::CollectCommands() const
    {
        // Flatten every menu action into a single command list for the palette. Submenus are
        // walked recursively; separators and section headers (no text) are skipped by the palette.
        QList<QAction*> commands;
        AZStd::function<void(QMenu*)> walk = [&](QMenu* menu)
        {
            for (QAction* action : menu->actions())
            {
                if (action->menu())
                {
                    walk(action->menu());
                }
                else if (!action->isSeparator())
                {
                    commands.append(action);
                }
            }
        };
        for (QAction* topAction : menuBar()->actions())
        {
            if (topAction->menu())
            {
                walk(topAction->menu());
            }
        }
        return commands;
    }

    void EditorMainWindow::OnShowCommandPalette()
    {
        CommandPalette::Show(this, CollectCommands());
    }

    void EditorMainWindow::SaveWorkspaceLayout(const QString& name)
    {
        // saveState must be routed through the docking system so floating/docked windows
        // are captured correctly (Plan §B5).
        QSettings settings;
        settings.beginGroup(QStringLiteral("Workspaces"));
        settings.setValue(name + QStringLiteral("/geometry"), saveGeometry());
#if defined(CEE_HAVE_ADS)
        settings.setValue(name + QStringLiteral("/state"), m_dockManager->saveState());
#else
        settings.setValue(name + QStringLiteral("/state"), m_fancyDocking->saveState());
#endif
        settings.endGroup();
    }

    bool EditorMainWindow::RestoreWorkspaceLayout(const QString& name)
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("Workspaces"));
        const QVariant geometry = settings.value(name + QStringLiteral("/geometry"));
        const QVariant state = settings.value(name + QStringLiteral("/state"));
        settings.endGroup();

        if (!geometry.isValid() || !state.isValid())
        {
            return false;
        }
        restoreGeometry(geometry.toByteArray());
#if defined(CEE_HAVE_ADS)
        return m_dockManager->restoreState(state.toByteArray());
#else
        return m_fancyDocking->restoreState(state.toByteArray());
#endif
    }

    void EditorMainWindow::OnSaveWorkspace()
    {
        SaveWorkspaceLayout(QStringLiteral("Default"));
    }

    void EditorMainWindow::OnRestoreWorkspace()
    {
        RestoreWorkspaceLayout(QStringLiteral("Default"));
    }
} // namespace CrossEngineEditor
