/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/EditorMainWindow.h>
#include <Window/CeeActionIds.h>
#include <Window/CeeAssetBrowserPanel.h>
#include <Window/CeePreferences.h>
#include <Window/CeePreferencesDialog.h>
#include <Window/CommandPalette.h>
#include <Window/ResourcePropertiesPanel.h>
#include <Window/ViewPaneRegistry.h>
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
#include <AzCore/Math/Crc.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/functional.h>

#include <AzToolsFramework/ActionManager/Action/ActionManagerInternalInterface.h>
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>
#include <AzToolsFramework/ActionManager/HotKey/HotKeyManagerInterface.h>
#include <AzToolsFramework/ActionManager/Menu/MenuManagerInterface.h>
#include <AzToolsFramework/ActionManager/Menu/MenuManagerInternalInterface.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Editor/ActionManagerIdentifiers/EditorContextIdentifiers.h>
#include <AzToolsFramework/Editor/ActionManagerIdentifiers/EditorMenuIdentifiers.h>
#include <AzToolsFramework/Entity/PrefabEditorEntityOwnershipInterface.h>
#include <AzToolsFramework/UI/UICore/QTreeViewStateSaver.hxx>
#include <AzToolsFramework/UI/Logging/TracePrintFLogPanel.h>
#include <AzToolsFramework/UI/Notifications/ToastBus.h>
#include <AzToolsFramework/UI/Notifications/ToastNotificationsView.h>
#include <AzToolsFramework/UI/Outliner/EntityOutlinerWidget.hxx>
#include <AzToolsFramework/UI/PropertyEditor/EntityPropertyEditor.hxx>
#include <AzToolsFramework/Viewport/ViewportSettings.h>

#include <AzQtComponents/Components/ToastNotificationConfiguration.h>
#include <AzQtComponents/Components/Widgets/FileDialog.h>
#include <AzQtComponents/Components/Widgets/SegmentControl.h>
#include <AzQtComponents/Components/Widgets/ToolBar.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <DockManager.h>
#include <DockWidget.h>
#include <DockAreaWidget.h>
AZ_POP_DISABLE_WARNING

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QAction>
#include <QActionGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QResizeEvent>
#include <QSet>
#include <QSettings>
#include <QShowEvent>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>
AZ_POP_DISABLE_WARNING

namespace
{
    // Node-clipboard mime (migration P0, §B9). Deliberately NOT an O3DE slice/prefab mime:
    // the payload is the engine's own serialization (rbfx Node::SaveXML bytes), and other O3DE
    // tooling must not mistake it for an entity prefab.
    const QString k_nodeClipboardMimeType = QStringLiteral("application/x-crossengineditor-nodes");

    // Toast bus id for this window's notification host (editor_polish.md P2).
    constexpr AZ::u32 k_toastBusId = AZ_CRC_CE("CEE::ToastNotifications");

    constexpr int k_maxRecentFiles = 10;

    //! Stable dock names (the ADS lookup keys; the ViewPane registry entries reuse them).
    constexpr const char* k_dockOutliner = "OutlinerDock";
    constexpr const char* k_dockInspector = "InspectorDock";
    constexpr const char* k_dockResourceInspector = "ResourceInspectorDock";
    constexpr const char* k_dockAssetBrowser = "AssetBrowserDock";
    constexpr const char* k_dockConsole = "ConsoleDock";
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

        // Qt-Advanced-Docking-System. The manager registers itself as the central widget of
        // this QMainWindow, so it must be created before any dock panels are added. It owns
        // all CDockWidgets and provides saveState/restoreState for workspaces.
        ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
        ads::CDockManager::setConfigFlag(ads::CDockManager::XmlCompressionEnabled, false);
        ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
        ads::CDockManager::setAutoHideConfigFlags(ads::CDockManager::DefaultAutoHideConfig);
        m_dockManager = new ads::CDockManager(this);

        // Preferences first: the autosave timer / helper visibility / default gizmo style all
        // read from it during panel construction (P2 / D9: persistence before UI).
        m_preferences = AZStd::make_unique<CeePreferences>();
        m_preferences->Load();
        AzToolsFramework::SetHelpersVisible(m_preferences->m_showViewportHelpers);

        BuildDockPanels();
        BuildStatusBar();

        // Toast host (P2): floats over this window; repositioned on show/resize.
        m_toastView = new AzToolsFramework::ToastNotificationsView(this, k_toastBusId);
        m_toastView->setObjectName(QStringLiteral("CeeToastView"));

        // Autosave (P2): periodic save of the engine scene while dirty.
        m_autosaveTimer = new QTimer(this);
        connect(m_autosaveTimer, &QTimer::timeout, this, &EditorMainWindow::OnAutosaveTimeout);
        ApplyAutosaveSettings();

        // Session-slot auto-restore (editor_polish.md P0-10 / A5): bring back the layout the
        // user closed with, before the window is shown. Runs after every panel exists so the
        // docking system can resolve them all.
        RestoreSessionLayout();

        // Engine edits (property writes, gizmo drags) arrive through the mirror bridge; let it
        // mark the scene dirty here (P2 dirty tracking).
        if (m_mirrorBridge)
        {
            m_mirrorBridge->SetDirtyCallback([this]
            {
                MarkSceneDirty();
            });
        }
    }

    EditorMainWindow::~EditorMainWindow()
    {
        // The mirror bridge (application-owned) OUTLIVES this window; drop the dirty callback
        // it holds into us before the window memory goes away (teardown-time mirror events
        // would otherwise call a dead window - review round 1, R3).
        if (m_mirrorBridge)
        {
            m_mirrorBridge->SetDirtyCallback({});
        }
    }

    // ------------------------------------------------------------------ panels

    void EditorMainWindow::BuildDockPanels()
    {
        // Engine-agnostic viewport surface (Plan §B2). Since P1-18 the gizmo controls live in a
        // header bar strip attached to the viewport instead of a main-window toolbar.
        auto* viewport = new EditorViewportWidget(k_viewportId, m_mirrorBridge, this);
        m_viewport = viewport;
        // Shortcut upstream bridge (editor_polish.md P0-2): the viewport is a bare QWindow, so
        // while it has focus no registered shortcut fires; key presses are re-dispatched here.
        viewport->SetShortcutBridgeTarget(this);
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (backend)
        {
            viewport->SetSceneRenderer(&backend->GetSceneRenderer());
        }

        // Console (rbfx_migration.md §3.4): the official trace log panel. A fresh panel has no
        // tabs (editor_polish.md F1) - seed it like the official host does (AssetProcessor
        // MainWindow.cpp:518,1851): stable settings id, load the persisted layout, else two
        // default tabs. BaseLogPanel's ctor requires a parent WITH a layout
        // (pParent->layout(), LogPanel_Panel.cpp:105) - a parentless construction crashes, so
        // the panel sits in a margin-less host widget the dock takes.
        auto consoleFactory = [] -> QWidget*
        {
            auto* host = new QWidget();
            auto* layout = new QVBoxLayout(host);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(0);
            auto* console = new AzToolsFramework::LogPanel::TracePrintFLogPanel(host);
            console->SetStorageID(AZ_CRC_CE("CEE::Console"));
            if (!console->LoadState())
            {
                using AzToolsFramework::LogPanel::TabSettings;
                console->AddLogTab(TabSettings("All output", "", "", true, true, true, false));
                console->AddLogTab(TabSettings("Warnings+Errors", "", "", false, true, true, false));
            }
            return host;
        };

        // ViewPane registry (editor_polish.md P1-14 / D6): every tool panel registers name (the
        // ADS restore key), category and factory here; Tools-menu actions and View toggles
        // enumerate from it. The viewport stays the central dock, so it is not a pane.
        m_paneRegistry.RegisterPane({ QString::fromLatin1(k_dockOutliner), QStringLiteral("Entity Outliner"),
                                      QStringLiteral("Core"), Qt::LeftDockWidgetArea, 100,
                                      [this]
                                      {
                                          return new AzToolsFramework::EntityOutlinerWidget(this);
                                      } });
        m_paneRegistry.RegisterPane({ QString::fromLatin1(k_dockInspector), QStringLiteral("Inspector"),
                                      QStringLiteral("Core"), Qt::RightDockWidgetArea, 200,
                                      [this]
                                      {
                                          return new AzToolsFramework::EntityPropertyEditor(this);
                                      } });
        m_paneRegistry.RegisterPane({ QString::fromLatin1(k_dockResourceInspector), QStringLiteral("Resource Inspector"),
                                      QStringLiteral("Core"), Qt::RightDockWidgetArea, 300,
                                      [this]
                                      {
                                          return new ResourcePropertiesPanel(m_mirrorBridge, this);
                                      } });
        m_paneRegistry.RegisterPane({ QString::fromLatin1(k_dockAssetBrowser), QStringLiteral("Asset Browser"),
                                      QStringLiteral("Core"), Qt::BottomDockWidgetArea, 400,
                                      [this, backend]
                                      {
                                          // backend is never null here: StartCommon registers it
                                          // before this window is constructed (no fallback path
                                          // to miscast - review round 1, R4).
                                          return new CeeAssetBrowserPanel(&backend->GetAssetSource(), m_mirrorBridge, this);
                                      } });
        m_paneRegistry.RegisterPane({ QString::fromLatin1(k_dockConsole), QStringLiteral("Console"),
                                      QStringLiteral("Core"), Qt::BottomDockWidgetArea, 500,
                                      consoleFactory });

        // Viewport header bar + surface stacked vertically in the central dock.
        auto* viewportColumn = new QWidget(this);
        auto* viewportLayout = new QVBoxLayout(viewportColumn);
        viewportLayout->setContentsMargins(0, 0, 0, 0);
        viewportLayout->setSpacing(0);
        viewportLayout->addWidget(BuildViewportHeaderBar());
        viewportLayout->addWidget(viewport);

        // The viewport is the non-closable central anchor dock; tool panels dock around it.
        auto* viewportDock = new ads::CDockWidget(m_dockManager, QStringLiteral("Viewport"), this);
        viewportDock->setObjectName(QStringLiteral("ViewportDock"));
        viewportDock->setWidget(viewportColumn);
        viewportDock->setFeature(ads::CDockWidget::DockWidgetClosable, false);
        viewportDock->setFeature(ads::CDockWidget::DockWidgetMovable, false);
        viewportDock->setFeature(ads::CDockWidget::DockWidgetFloatable, false);
        ads::CDockAreaWidget* centralArea = m_dockManager->setCentralWidget(viewportDock);
        (void)centralArea;

        CeeAssetBrowserPanel* assetBrowser = nullptr;
        ResourcePropertiesPanel* resourcePanel = nullptr;
        for (const ViewPaneEntry& pane : m_paneRegistry.Entries())
        {
            QWidget* content = pane.m_factory();
            if (pane.m_name == QLatin1String(k_dockAssetBrowser))
            {
                assetBrowser = static_cast<CeeAssetBrowserPanel*>(content);
            }
            else if (pane.m_name == QLatin1String(k_dockResourceInspector))
            {
                resourcePanel = static_cast<ResourcePropertiesPanel*>(content);
            }
            AddPanel(pane.m_name, pane.m_title, content, pane.m_area);
        }

        // Double-click wiring between the Asset Browser and the Resource Inspector panel
        // (rbfx_migration.md §3.4).
        if (assetBrowser && resourcePanel)
        {
            assetBrowser->SetResourcePropertiesPanel(resourcePanel);
        }
    }

    QWidget* EditorMainWindow::BuildViewportHeaderBar()
    {
        // Gizmo control cluster (editor_polish.md P1-18 / S1, replacing the main-window
        // Transform toolbar): a toolbar strip that is a SIBLING of the render surface (never an
        // overlay on it), so the native-surface constraints of Plan §B8 B3 cannot bite and the
        // engine input never competes with a transparent widget. Hot-keys follow the industry
        // standard Q/W/E/R (Move/Rotate/Scale) + Q=Select and T=Combined; 1/2/3/4 alternates.
        auto* bar = new QToolBar(QStringLiteral("Viewport Header"), this);
        bar->setObjectName(QStringLiteral("ViewportHeaderBar"));
        bar->setMovable(false);
        bar->setFloatable(false);

        auto addModeAction = [this, bar](
                                  const QString& text, const QString& primaryKey, const QString& altKey,
                                  void (EditorMainWindow::*slot)())
        {
            QAction* action = bar->addAction(text);
            action->setCheckable(true);
            // Primary industry-standard key plus the legacy numeric key as an alternate.
            action->setShortcuts(QList<QKeySequence>{ QKeySequence(primaryKey), QKeySequence(altKey) });
            // Same context as the Edit actions (O3DE ActionManager alignment), and the action
            // is registered with THIS window so the ActionContextWidgetWatcher matches it while
            // the viewport QWindow has focus (the bridge re-dispatches the key here, P0-2).
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            addAction(action);
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

        bar->addSeparator();

        // Gizmo style switcher (Plan §B7; P2 A10 control upgrade): a SegmentControl is exactly
        // the idiom for a two-way exclusive choice. The stacked pages are unused placeholders -
        // the control is the segmented bar itself; currentChanged drives GizmoControlRequestBus.
        auto* styleSwitch = new AzQtComponents::SegmentControl(AzQtComponents::SegmentControl::TabPosition::North, bar);
        styleSwitch->setObjectName(QStringLiteral("GizmoStyleSwitch"));
        QWidget* blenderPage = new QWidget(styleSwitch);
        QWidget* unrealPage = new QWidget(styleSwitch);
        styleSwitch->addTab(blenderPage, QStringLiteral("Blender"));
        styleSwitch->addTab(unrealPage, QStringLiteral("Unreal"));
        styleSwitch->setCurrentIndex(
            m_preferences ? static_cast<int>(m_preferences->m_defaultGizmoStyle) : 0);
        connect(styleSwitch, &AzQtComponents::SegmentControl::currentChanged, this, [](int index)
        {
            GizmoControlRequestBus::Broadcast(
                &GizmoControlRequests::SetGizmoStyle, index == 1 ? GizmoStyle::Unreal : GizmoStyle::Blender);
        });
        bar->addWidget(styleSwitch);

        bar->addSeparator();

        // Coordinate space switcher (World / Local, Plan §B5). World = axis-aligned handles;
        // Local aligns them to the entity's orientation.
        auto* spaceGroup = new QActionGroup(this);
        spaceGroup->setExclusive(true);
        auto addSpaceAction = [this, bar, spaceGroup](const QString& text, GizmoSpace space)
        {
            QAction* action = bar->addAction(text);
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

        bar->addSeparator();

        // Snap toggle (Plan §B5). One checkbox flips grid + angle + scale snap on/off, preserving
        // the current style's step increments; per-increment tuning lives in Preferences.
        QAction* snap = bar->addAction(QStringLiteral("Snap"));
        snap->setCheckable(true);
        connect(snap, &QAction::toggled, this, [](bool enabled)
        {
            GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetSnapEnabled, enabled);
        });

        return bar;
    }

    void EditorMainWindow::BuildStatusBar()
    {
        // Two-cell status bar (editor_polish.md P1-17 / D8): project path / backend name. The
        // AzQtComponents StatusBar style applies to the standard QStatusBar automatically. FPS
        // and memory cells were deliberately cut - the engines expose no cheap statistics
        // interface, and polling one in would violate KISS.
        QStatusBar* status = statusBar();
        status->setObjectName(QStringLiteral("CeeStatusBar"));
        m_statusProject = new QLabel(QStringLiteral("-"), status);
        m_statusProject->setMinimumWidth(200);
        m_statusBackend = new QLabel(QStringLiteral("-"), status);
        status->addWidget(m_statusProject);
        status->addPermanentWidget(m_statusBackend);
    }

    void EditorMainWindow::OpenViewPane(const QString& paneName)
    {
        // Registry key == dock object name, so "open" is a docking-system lookup + raise.
        const ViewPaneEntry* entry = m_paneRegistry.Find(paneName);
        const QString dockName = entry ? entry->m_name : paneName;
        if (ads::CDockWidget* dock = m_dockManager->findDockWidget(dockName))
        {
            dock->toggleView(true);
            dock->setAsCurrentTab();
        }
    }

    void EditorMainWindow::ShowAssetBrowser()
    {
        OpenViewPane(QString::fromLatin1(k_dockAssetBrowser));
    }

    bool EditorMainWindow::IsPanelOpen(const QString& dockName) const
    {
        const ads::CDockWidget* dock = m_dockManager ? m_dockManager->findDockWidget(dockName) : nullptr;
        return dock && !dock->isClosed(); // floating counts as open
    }

    void EditorMainWindow::SetPanelOpen(const QString& dockName, bool open)
    {
        if (ads::CDockWidget* dock = m_dockManager ? m_dockManager->findDockWidget(dockName) : nullptr)
        {
            dock->toggleView(open);
        }
    }

    QWidget* EditorMainWindow::DockContent(const QString& dockName) const
    {
        const ads::CDockWidget* dock = m_dockManager ? m_dockManager->findDockWidget(dockName) : nullptr;
        return dock ? dock->widget() : nullptr;
    }

    // ------------------------------------------------------- entity operations

    AZ::EntityId EditorMainWindow::CreateEngineEntity()
    {
        // Migration P0: with an engine backend the engine's native scene is the single source
        // of truth, so creation goes through the mirror contract (rbfx: a bare scene Node)
        // instead of the O3DE prefab path. The rbfx mirror deliberately returns an invalid id -
        // the mirror entity is built on the next full re-sync, which RefreshFromEngine runs.
        AZ::EntityId createdId;
        if (IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get())
        {
            createdId = backend->GetEntityMirror().CreateObject(ObjectSpec{});
            if (m_mirrorBridge)
            {
                RefreshFromEngineKeepingState();
            }
        }
        MarkSceneDirty();
        return createdId;
    }

    void EditorMainWindow::DeleteSelection()
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
            RefreshFromEngineKeepingState();
        }
        MarkSceneDirty();
    }

    void EditorMainWindow::CutSelection()
    {
        // Cut = copy to the node clipboard, then the normal delete flow (engine nodes destroyed
        // first, editor-side removal, re-mirror).
        CopySelection();
        DeleteSelection();
    }

    void EditorMainWindow::CopySelection()
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

    bool EditorMainWindow::HasNodeClipboard()
    {
        const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
        return mime && mime->hasFormat(k_nodeClipboardMimeType);
    }

    void EditorMainWindow::PasteSelection()
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
            RefreshFromEngineKeepingState();
        }
        MarkSceneDirty();
    }

    void EditorMainWindow::DuplicateSelection()
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
            RefreshFromEngineKeepingState();
        }
        MarkSceneDirty();
    }

    void EditorMainWindow::FocusSelection()
    {
        // "Focus on selection" (editor_polish.md P0-3): the industry-standard F-key action.
        // Frame the camera on the union of the selection bounds so lights / cameras / empty
        // nodes (wireframe gizmos) frame just as meshes do.
        if (m_viewport)
        {
            m_viewport->FrameSelection();
        }
    }

    void EditorMainWindow::OpenPinnedInspector(const AzToolsFramework::EntityIdSet& entities)
    {
        // editor_polish.md P0-3 / A3: the Inspector's pin button asks the host app to open a
        // second, locked copy of the grid. 100% reuse: another EntityPropertyEditor pinned to
        // explicit ids (SetOverrideEntityIds - the official locked-selection mechanism, same as
        // ResourcePropertiesPanel). Hosted in a small tool window parented to this main window.
        if (entities.empty() || m_pinnedInspectorWindows.size() >= 4)
        {
            return; // nothing to pin, or politely cap runaway pinning at four windows.
        }

        auto* editor = new AzToolsFramework::EntityPropertyEditor();
        editor->SetOverrideEntityIds(entities);

        auto* window = new QWidget(this, Qt::Tool);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->setWindowTitle(QStringLiteral("Pinned Inspector"));
        auto* layout = new QVBoxLayout(window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(editor);
        window->resize(360, 480);

        // Remember the window so ClosePinnedInspector can find (and close) the host of a given
        // editor instance, and forget it again when the user closes the window directly.
        m_pinnedInspectorWindows.push_back(window);
        connect(window, &QObject::destroyed, this, [this, window]
        {
            AZStd::erase(m_pinnedInspectorWindows, window);
        });

        window->show();
        window->raise();
    }

    void EditorMainWindow::ClosePinnedInspector(AzToolsFramework::EntityPropertyEditor* editor)
    {
        // The pinned editor asks the host to close its window (EntityPropertyEditor::
        // CloseInspectorWindow, e.g. when its entities are about to be context-reset).
        for (QWidget* window : m_pinnedInspectorWindows)
        {
            if (window->findChild<AzToolsFramework::EntityPropertyEditor*>() == editor)
            {
                window->close(); // WA_DeleteOnClose disposes of the editor too.
                return;
            }
        }
    }

    // ------------------------------------------------------- level workflow

    void EditorMainWindow::NewLevel()
    {
        // Recreate the level as a fresh in-memory root prefab (Plan §B4).
        if (auto* ownership = AZ::Interface<AzToolsFramework::PrefabEditorEntityOwnershipInterface>::Get())
        {
            ownership->CreateNewLevelPrefab("NewLevel.prefab", "");
        }
    }

    void EditorMainWindow::OpenLevel()
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
            return;
        }
        RecordRecentFile(QDir::toNativeSeparators(path));
        SetSceneDisplayName(QFileInfo(path).fileName());
        ClearSceneDirty();
    }

    bool EditorMainWindow::SaveLevel()
    {
        // Plan §B4: with an engine backend the engine's native scene is the single source of
        // truth, so route Save to the backend (Godot PackedScene+ResourceSaver / rbfx SaveXML).
        // Passing an empty path lets the backend save to the scene's current file. If the backend
        // can't save (null/diligent, or no scene), fall back to the O3DE prefab path below.
        // Returns false only when the user cancelled the fallback save dialog, so closeEvent
        // can keep the editor open; write failures warn on the Console and still return true.
        if (IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get())
        {
            if (backend->GetEntityMirror().SaveScene(AZStd::string{}))
            {
                ClearSceneDirty();
                return true;
            }
        }

        // Themed save dialog (editor_polish.md P3): AzQtComponents::FileDialog adds invalid-
        // character validation over the stock QFileDialog.
        const QString path = AzQtComponents::FileDialog::GetSaveFileName(
            this, QStringLiteral("Save Level"), QStringLiteral("NewLevel.prefab"), QStringLiteral("Prefab (*.prefab)"));
        if (path.isEmpty())
        {
            return false;
        }

        auto* ownership = AZ::Interface<AzToolsFramework::PrefabEditorEntityOwnershipInterface>::Get();
        if (!ownership)
        {
            // The contract says write failures warn and still return true, but silently doing
            // NOTHING would discard a dirty scene in closeEvent - warn so it is visible
            // (review round 1, R11).
            AZ_Warning("CrossEngineEditor", false, "SaveLevel: PrefabEditorEntityOwnershipInterface unavailable.");
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
        RecordRecentFile(QDir::toNativeSeparators(path));
        SetSceneDisplayName(QFileInfo(path).fileName());
        ClearSceneDirty();
        return true;
    }

    void EditorMainWindow::ShowCommandPalette()
    {
        CommandPalette::Show(this, CollectCommands());
    }

    void EditorMainWindow::ShowPreferences()
    {
        // Preferences dialog (editor_polish.md P2 / D9 + hotkey rebinding E1). Modal; edits the
        // shared CeePreferences object and applies/persists on change. The shortcuts page gets
        // the live command set (registered actions carry their ids as objectName).
        CeePreferencesDialog dialog(this, m_preferences.get(), CollectCommands());
        connect(&dialog, &CeePreferencesDialog::PreferencesChanged, this, [this]
        {
            m_preferences->Save();
            ApplyAutosaveSettings();
            AzToolsFramework::SetHelpersVisible(m_preferences->m_showViewportHelpers);
        });
        dialog.exec();
    }

    void EditorMainWindow::SaveWorkspace()
    {
        SaveWorkspaceLayout(QStringLiteral("Default"));
    }

    void EditorMainWindow::RestoreWorkspace()
    {
        RestoreWorkspaceLayout(QStringLiteral("Default"));
    }

    // ------------------------------------------------------- status / dirty / toast

    void EditorMainWindow::SetStatusInfo(const QString& projectPath, const QString& backendName)
    {
        if (m_statusProject)
        {
            m_statusProject->setText(projectPath.isEmpty() ? QStringLiteral("-") : projectPath);
            m_statusProject->setToolTip(projectPath);
        }
        if (m_statusBackend)
        {
            m_statusBackend->setText(backendName.isEmpty() ? QStringLiteral("-") : backendName);
        }
    }

    void EditorMainWindow::SetSceneDisplayName(const QString& sceneName)
    {
        m_sceneDisplayName = sceneName;
        UpdateWindowTitle();
    }

    void EditorMainWindow::MarkSceneDirty()
    {
        if (!m_sceneDirty)
        {
            m_sceneDirty = true;
            UpdateWindowTitle();
        }
    }

    void EditorMainWindow::ClearSceneDirty()
    {
        if (m_sceneDirty)
        {
            m_sceneDirty = false;
            UpdateWindowTitle();
        }
    }

    void EditorMainWindow::UpdateWindowTitle()
    {
        QString title = QStringLiteral("Cross-Engine Editor");
        if (!m_sceneDisplayName.isEmpty())
        {
            title += QStringLiteral(" - ") + m_sceneDisplayName;
        }
        if (m_sceneDirty)
        {
            title += QStringLiteral(" [*]");
        }
        setWindowTitle(title);
    }

    void EditorMainWindow::ShowToast(AzQtComponents::ToastType type, const QString& title, const QString& description)
    {
        if (!m_toastView)
        {
            return;
        }
        AzQtComponents::ToastConfiguration config(type, title, description);
        AzToolsFramework::ToastRequestBus::Event(k_toastBusId, &AzToolsFramework::ToastRequests::ShowToastNotification, config);
    }

    void EditorMainWindow::showEvent(QShowEvent* event)
    {
        AzQtComponents::DockMainWindow::showEvent(event);
        if (m_toastView)
        {
            m_toastView->OnShow();
        }
    }

    void EditorMainWindow::resizeEvent(QResizeEvent* event)
    {
        AzQtComponents::DockMainWindow::resizeEvent(event);
        if (m_toastView)
        {
            m_toastView->UpdateToastPosition();
        }
    }

    // ------------------------------------------------------- ActionManager follow-ups

    void EditorMainWindow::OnActionManagerReady()
    {
        // editor_polish.md P1-12: the menu bar is fully generated by the ActionManager. The
        // shell is left with the main toolbar (P1-16), the dynamic menu content (S3
        // exceptions) and enable-state refresh wiring.
        auto* actionManagerInternal = AZ::Interface<AzToolsFramework::ActionManagerInternalInterface>::Get();
        auto* actionManager = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get();
        auto* menuManagerInternal = AZ::Interface<AzToolsFramework::MenuManagerInternalInterface>::Get();
        if (!actionManagerInternal || !actionManager || !menuManagerInternal)
        {
            return;
        }

        // --- Main toolbar (P1-16): level workflow + undo/redo, AzQtComponents main-toolbar
        // style. Icons are the style's standard pixmaps (the native editor's icon set lives in
        // the Atom-bound EditorLib - stock pixmaps keep this dependency-free).
        m_mainToolBar = addToolBar(QStringLiteral("Main"));
        m_mainToolBar->setObjectName(QStringLiteral("MainToolBar"));
        m_mainToolBar->setMovable(false);
        AzQtComponents::ToolBar::addMainToolBarStyle(m_mainToolBar);

        QStyle* currentStyle = style();
        auto addToolAction = [this, actionManagerInternal, currentStyle](AZStd::string_view actionId, QStyle::StandardPixmap icon)
        {
            if (QAction* action = actionManagerInternal->GetAction(AZStd::string(actionId)))
            {
                action->setIcon(currentStyle->standardIcon(icon));
                m_mainToolBar->addAction(action);
            }
        };
        addToolAction(CeeActions::FileNew, QStyle::SP_FileDialogNewFolder);
        addToolAction(CeeActions::FileOpen, QStyle::SP_DirOpenIcon);
        addToolAction(CeeActions::FileSave, QStyle::SP_DialogSaveButton);
        m_mainToolBar->addSeparator();
        addToolAction(CeeActions::EditUndo, QStyle::SP_ArrowBack);
        addToolAction(CeeActions::EditRedo, QStyle::SP_ArrowForward);

        // --- Dynamic menu content (S3 exceptions; OnActionManagerReady runs once - A7).
        auto hookMenu = [menuManagerInternal, this](AZStd::string_view menuId, auto&& slot)
        {
            if (QMenu* menu = menuManagerInternal->GetMenu(AZStd::string(menuId)))
            {
                connect(menu, &QMenu::aboutToShow, this, slot);
            }
        };

        // Edit menu: fill the Create submenu once the backend can enumerate types.
        hookMenu(EditorIdentifiers::EditMenuIdentifier, [this]
        {
            if (!m_createMenuPopulated)
            {
                PopulateCreateMenu();
            }
            // Refresh selection-sensitive enable states while the menu is opening.
            AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get()->TriggerActionUpdater(AZStd::string(CeeActions::SelectionUpdater));
        });
        // File menu: refresh the Open Recent entries right before showing.
        hookMenu(EditorIdentifiers::FileMenuIdentifier, [this]
        {
            SyncRecentActions();
            AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get()->TriggerActionUpdater(AZStd::string(CeeActions::RecentUpdater));
        });
        // Selection-sensitive enable states also refresh on the context menus.
        for (const AZStd::string_view menuId :
             { EditorIdentifiers::EntityOutlinerContextMenuIdentifier, EditorIdentifiers::ViewportContextMenuIdentifier,
               EditorIdentifiers::InspectorEntityComponentContextMenuIdentifier })
        {
            hookMenu(menuId, []
            {
                AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get()->TriggerActionUpdater(
                    AZStd::string(CeeActions::SelectionUpdater));
            });
        }

        // Panel-toggle check states follow the docking system: every dock visibility change
        // fires the panels updater (A1: the docking system stays the source of truth).
        const auto triggerPanelsUpdater = [actionManager]
        {
            actionManager->TriggerActionUpdater(AZStd::string(CeeActions::PanelsUpdater));
        };
        const auto docks = m_dockManager->dockWidgetsMap();
        for (auto it = docks.cbegin(); it != docks.cend(); ++it)
        {
            connect(it.value(), &ads::CDockWidget::viewToggled, this, triggerPanelsUpdater);
        }

        // Re-apply persisted hotkey bindings (E1/F4: upstream has no persistence, CEE keeps
        // them in QSettings).
        if (auto* hotKeyManager = AZ::Interface<AzToolsFramework::HotKeyManagerInterface>::Get())
        {
            QSettings settings;
            settings.beginGroup(QStringLiteral("HotKeys"));
            for (const QString& id : settings.allKeys())
            {
                const QString binding = settings.value(id).toString();
                if (!binding.isEmpty() && actionManagerInternal->GetAction(AZStd::string(id.toUtf8().constData())) != nullptr)
                {
                    hotKeyManager->SetActionHotKey(
                        AZStd::string(id.toUtf8().constData()), AZStd::string(binding.toUtf8().constData()));
                }
            }
            settings.endGroup();
        }
    }

    void EditorMainWindow::PopulateCreateMenu()
    {
        // P1-12 / S3: engine types are DATA, not shell code - each becomes a real registered
        // action the first time the Edit menu opens after the backend can enumerate (latched;
        // the backend builds its scene lazily on first surface, so a failed enumeration simply
        // retries on the next open). Registering binds them into the registered Create submenu;
        // the MenuManager refreshes the Edit menu by itself. NO empty placeholder slots exist at
        // any point (the native Recent-Files 10-slot pattern is the rejected compromise).
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        auto* actionManager = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get();
        auto* menuManager = AZ::Interface<AzToolsFramework::MenuManagerInterface>::Get();
        if (!backend || !actionManager || !menuManager)
        {
            return;
        }

        const AZStd::string createMenuId(EditorIdentifiers::EntityCreationMenuIdentifier);
        if (!menuManager->IsMenuRegistered(createMenuId))
        {
            AzToolsFramework::MenuProperties menuProperties;
            menuProperties.m_name = "Create";
            menuManager->RegisterMenu(createMenuId, menuProperties);
            menuManager->AddSubMenuToMenu(AZStd::string(EditorIdentifiers::EditMenuIdentifier), createMenuId, 200);
        }

        AZStd::vector<ObjectTypeInfo> types;
        backend->GetEntityMirror().EnumerateObjectTypes(types);
        if (types.empty())
        {
            return; // latch stays unset: retry on the next open.
        }
        m_createMenuPopulated = true;

        int sortKey = 200; // 100 = "Empty Node" (registered by CeeActionsHandler at startup).
        for (const ObjectTypeInfo& type : types)
        {
            const AZStd::string actionId = AZStd::string::format("cee.action.entity.create.%s", type.m_typeId.c_str());
            if (actionManager->IsActionRegistered(actionId))
            {
                continue;
            }

            AzToolsFramework::ActionProperties properties;
            properties.m_name = type.m_displayName;
            properties.m_description = "Create a " + type.m_displayName + " engine object";
            properties.m_category = "Entity";
            properties.m_menuVisibility = AzToolsFramework::ActionVisibility::AlwaysShow;

            actionManager->RegisterAction(
                AZStd::string(EditorIdentifiers::MainWindowActionContextIdentifier), actionId, properties,
                [this, backend, typeId = type.m_typeId]
                {
                    // Same path as "Empty Node": create through the mirror contract, then a
                    // full re-mirror (state-keeping) + dirty mark.
                    backend->GetEntityMirror().CreateObject(
                        ObjectSpec{ typeId, "", "", AZ::Transform::CreateIdentity() });
                    RefreshFromEngineKeepingState();
                    MarkSceneDirty();
                });
            menuManager->AddActionToMenu(createMenuId, actionId, sortKey);
            sortKey += 10;
        }

        // Menu refreshes normally drain on the next system tick; we are inside the Edit menu's
        // aboutToShow, so drain NOW or the freshly registered types appear one tick late on the
        // very first open (review round 1, R9).
        if (auto* menuManagerInternal = AZ::Interface<AzToolsFramework::MenuManagerInternalInterface>::Get())
        {
            menuManagerInternal->RefreshMenus();
        }
    }

    void EditorMainWindow::RecordRecentFile(const QString& path)
    {
        // QSettings-backed MRU (editor_polish.md P2): only editor-openable documents (.prefab
        // levels) are tracked; engine scenes are launch arguments (--scene), not documents.
        QSettings settings;
        settings.beginGroup(QStringLiteral("Recent"));
        QStringList files = settings.value(QStringLiteral("files")).toStringList();
        files.removeAll(path);
        files.prepend(path);
        while (files.size() > k_maxRecentFiles)
        {
            files.removeLast();
        }
        settings.setValue(QStringLiteral("files"), files);
        settings.endGroup();

        SyncRecentActions();
    }

    void EditorMainWindow::OpenRecentFile(const QString& path)
    {
        // Same load path as OpenLevel, addressed by the remembered path. A stale entry (file
        // moved or deleted) warns on the console and is dropped from the list.
        auto* ownership = AZ::Interface<AzToolsFramework::PrefabEditorEntityOwnershipInterface>::Get();
        if (!ownership)
        {
            return;
        }

        AZ::IO::FileIOStream stream(path.toUtf8().constData(), AZ::IO::OpenMode::ModeRead);
        if (!stream.IsOpen())
        {
            AZ_Warning("CrossEngineEditor", false, "Recent file is no longer readable: %s", path.toUtf8().constData());
            QSettings settings;
            settings.beginGroup(QStringLiteral("Recent"));
            QStringList files = settings.value(QStringLiteral("files")).toStringList();
            files.removeAll(path);
            settings.setValue(QStringLiteral("files"), files);
            settings.endGroup();
            SyncRecentActions();
            return;
        }
        if (ownership->LoadFromStream(stream, path.toUtf8().constData()))
        {
            SetSceneDisplayName(QFileInfo(path).fileName());
            ClearSceneDirty();
        }
    }

    void EditorMainWindow::SyncRecentActions()
    {
        // Materialize the MRU as lazily registered actions bound into the Open Recent submenu.
        // Slots are created on demand and reused (renamed) as the list changes; a slot beyond
        // the current list length disables itself, and disabled entries hide via
        // HideWhenDisabled - so the submenu only ever shows real files (S3).
        auto* actionManager = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get();
        auto* menuManager = AZ::Interface<AzToolsFramework::MenuManagerInterface>::Get();
        auto* menuManagerInternal = AZ::Interface<AzToolsFramework::MenuManagerInternalInterface>::Get();
        if (!actionManager || !menuManager || !menuManagerInternal)
        {
            return;
        }

        QSettings settings;
        settings.beginGroup(QStringLiteral("Recent"));
        const QStringList files = settings.value(QStringLiteral("files")).toStringList();
        settings.endGroup();

        const AZStd::string recentMenuId(EditorIdentifiers::RecentFilesMenuIdentifier);
        if (!menuManager->IsMenuRegistered(recentMenuId))
        {
            AzToolsFramework::MenuProperties menuProperties;
            menuProperties.m_name = "Open Recent";
            menuManager->RegisterMenu(recentMenuId, menuProperties);
            menuManager->AddSubMenuToMenu(
                AZStd::string(EditorIdentifiers::FileMenuIdentifier), recentMenuId, 300);
        }

        for (int i = 0; i < k_maxRecentFiles; ++i)
        {
            const AZStd::string actionId = AZStd::string::format("cee.action.file.recent.%d", i);
            if (i >= files.size())
            {
                if (actionManager->IsActionRegistered(actionId))
                {
                    actionManager->UpdateAction(actionId); // enabled callback turns it off
                }
                continue;
            }

            const QString path = files.at(i);
            if (!actionManager->IsActionRegistered(actionId))
            {
                AzToolsFramework::ActionProperties properties;
                properties.m_name = path.toUtf8().constData();
                properties.m_category = "Level";
                properties.m_menuVisibility = AzToolsFramework::ActionVisibility::HideWhenDisabled;

                actionManager->RegisterAction(
                    AZStd::string(EditorIdentifiers::MainWindowActionContextIdentifier), actionId, properties,
                    [this, i]
                    {
                        QSettings s;
                        s.beginGroup(QStringLiteral("Recent"));
                        const QStringList list = s.value(QStringLiteral("files")).toStringList();
                        s.endGroup();
                        if (i < list.size())
                        {
                            OpenRecentFile(list.at(i));
                        }
                    });
                actionManager->InstallEnabledStateCallback(actionId, [i]
                {
                    QSettings s;
                    s.beginGroup(QStringLiteral("Recent"));
                    const int count = s.value(QStringLiteral("files")).toStringList().size();
                    s.endGroup();
                    return i < count;
                });
                actionManager->AddActionToUpdater(AZStd::string(CeeActions::RecentUpdater), actionId);
                menuManager->AddActionToMenu(recentMenuId, actionId, 100 + i);
            }
            else
            {
                actionManager->SetActionName(actionId, path.toUtf8().constData());
            }
        }

        // Name/enabled changes need a menu refresh to show (SetActionName does not queue one).
        menuManagerInternal->QueueRefreshForMenu(recentMenuId);
        menuManagerInternal->QueueRefreshForMenu(AZStd::string(EditorIdentifiers::FileMenuIdentifier));
        menuManagerInternal->RefreshMenus();
    }

    void EditorMainWindow::OnAutosaveTimeout()
    {
        // Autosave (editor_polish.md P2 / M2): while the scene is dirty, periodically write it
        // to <exe>/autosave/<scene> through the backend's SaveScene(path). A separate file, so
        // the .bak chain of a manual save stays untouched; recovery is manual (open the file
        // via --scene) because v1 has no runtime scene-loading contract.
        if (!m_sceneDirty)
        {
            return;
        }
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            return;
        }

        QDir autosaveDir(QCoreApplication::applicationDirPath() + QStringLiteral("/autosave"));
        const QString fileName = (m_sceneDisplayName.isEmpty() ? QStringLiteral("Untitled") : m_sceneDisplayName)
                                     .replace(QLatin1Char('/'), QLatin1Char('_'))
                                     .replace(QLatin1Char('\\'), QLatin1Char('_'));
        const QString autosavePath = autosaveDir.filePath(fileName);
        if (!autosaveDir.exists() && !autosaveDir.mkpath(QStringLiteral(".")))
        {
            AZ_Warning("CrossEngineEditor", false, "Autosave: could not create %s.",
                autosaveDir.absolutePath().toUtf8().constData());
            return;
        }

        if (backend->GetEntityMirror().SaveScene(AZStd::string(autosavePath.toUtf8().constData())))
        {
            ShowToast(AzQtComponents::ToastType::Information, QStringLiteral("Autosaved"),
                QStringLiteral("Scene saved to ") + QDir::toNativeSeparators(autosavePath));
        }
    }

    void EditorMainWindow::ApplyAutosaveSettings()
    {
        if (!m_autosaveTimer)
        {
            return;
        }
        if (m_preferences && m_preferences->m_autosaveEnabled)
        {
            m_autosaveTimer->start(
                AZStd::max(1, m_preferences->m_autosaveIntervalMinutes) * 60 * 1000);
        }
        else
        {
            m_autosaveTimer->stop();
        }
    }

    void EditorMainWindow::RefreshFromEngineKeepingState()
    {
        // Full re-mirror with the Outliner's expansion/scroll state carried across
        // (editor_polish.md P2). The stock TreeViewState helper works on any QTreeView - the
        // Outliner is a framework widget we cannot rebase onto QTreeViewWithStateSaving.
        QTreeView* outlinerTree = nullptr;
        if (QWidget* outlinerDockContent = DockContent(QString::fromLatin1(k_dockOutliner)))
        {
            outlinerTree = outlinerDockContent->findChild<QTreeView*>();
        }
        if (outlinerTree)
        {
            if (!m_outlinerTreeState)
            {
                m_outlinerTreeState = AzToolsFramework::TreeViewState::CreateTreeViewState();
            }
            m_outlinerTreeState->CaptureSnapshot(outlinerTree);
        }

        if (m_mirrorBridge)
        {
            m_mirrorBridge->RefreshFromEngine();
        }

        if (outlinerTree && m_outlinerTreeState)
        {
            m_outlinerTreeState->ApplySnapshot(outlinerTree);
        }
    }

    // ------------------------------------------------------- close / layout persistence

    void EditorMainWindow::closeEvent(QCloseEvent* event)
    {
        // Migration L1 (rbfx_migration.md §3.4) + dirty tracking (editor_polish.md P2 / A6):
        // only ask about saving when there ARE unsaved changes - the old "always ask" flow was
        // the v1 stand-in for a dirty flag, not a feature. Closing the box (X/Escape) resolves
        // to Cancel, which keeps the editor open.
        if (m_sceneDirty)
        {
            const QMessageBox::StandardButton answer = QMessageBox::question(
                this, QStringLiteral("Exit Editor"), QStringLiteral("Save changes before exiting?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
            if (answer == QMessageBox::Cancel)
            {
                event->ignore();
                return;
            }
            if (answer == QMessageBox::Save && !SaveLevel())
            {
                event->ignore(); // User cancelled the save dialog - keep the editor open.
                return;
            }
        }

        // Session-slot auto-save (editor_polish.md P0-10): persist the layout the user is
        // leaving with, regardless of the save/discard choice - layout is UI state, not scene
        // content. Restored by the constructor on the next launch.
        SaveSessionLayout();
        event->accept();
    }

    void EditorMainWindow::SaveSessionLayout()
    {
        // Session slot (editor_polish.md P0-10 / A5): same saveState pair as the named
        // Workspaces, but in its own QSettings group so automatic persistence never overwrites a
        // user-named workspace ("Default" included).
        QSettings settings;
        settings.beginGroup(QStringLiteral("Session"));
        settings.setValue(QStringLiteral("geometry"), saveGeometry());
        settings.setValue(QStringLiteral("state"), m_dockManager->saveState());
        settings.endGroup();
    }

    void EditorMainWindow::RestoreSessionLayout()
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("Session"));
        const QVariant geometry = settings.value(QStringLiteral("geometry"));
        const QVariant state = settings.value(QStringLiteral("state"));
        settings.endGroup();

        if (!geometry.isValid() || !state.isValid())
        {
            return; // first launch - keep the default layout.
        }
        restoreGeometry(geometry.toByteArray());
        m_dockManager->restoreState(state.toByteArray());
    }

    void EditorMainWindow::SaveWorkspaceLayout(const QString& name)
    {
        // saveState must be routed through the docking system so floating/docked windows
        // are captured correctly (Plan §B5).
        QSettings settings;
        settings.beginGroup(QStringLiteral("Workspaces"));
        settings.setValue(name + QStringLiteral("/geometry"), saveGeometry());
        settings.setValue(name + QStringLiteral("/state"), m_dockManager->saveState());
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
        return m_dockManager->restoreState(state.toByteArray());
    }

    void EditorMainWindow::AddPanel(
        const QString& objectName, const QString& title, QWidget* content, Qt::DockWidgetArea area)
    {
        // Wrap the content in an ADS dock widget and add it to the manager (ADS owns the
        // widget). ADS uses its own DockWidgetArea enum; map the classic Qt areas to it.
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
    }

    QList<QAction*> EditorMainWindow::CollectCommands() const
    {
        // Command palette data source (editor_polish.md P1-15): every QAction reachable from
        // the GENERATED menu bar (which the ActionManager fills - all registered top-level
        // commands) plus the three registered context menus (fetched straight from the
        // MenuManager), so framework-registered actions that only live in context menus are
        // discoverable too. Pointer-level dedup: the entity pool is bound into several menus,
        // and one QAction* appearing four times in the palette would read as four commands.
        QList<QAction*> commands;
        QSet<QAction*> seen;
        AZStd::function<void(QMenu*)> walk = [&](QMenu* menu)
        {
            for (QAction* action : menu->actions())
            {
                if (action->menu())
                {
                    walk(action->menu());
                }
                else if (!action->isSeparator() && !seen.contains(action))
                {
                    seen.insert(action);
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

        if (auto* menuManagerInternal = AZ::Interface<AzToolsFramework::MenuManagerInternalInterface>::Get())
        {
            for (const AZStd::string_view menuId :
                 { EditorIdentifiers::EntityOutlinerContextMenuIdentifier,
                   EditorIdentifiers::ViewportContextMenuIdentifier,
                   EditorIdentifiers::InspectorEntityComponentContextMenuIdentifier })
            {
                if (QMenu* menu = menuManagerInternal->GetMenu(AZStd::string(menuId)))
                {
                    walk(menu);
                }
            }
        }
        return commands;
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
} // namespace CrossEngineEditor
