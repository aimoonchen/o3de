/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/EditorMainWindow.h>
#include <Window/AssetBrowserPanel.h>
#include <Window/CommandPalette.h>
#include <Viewport/GizmoManager.h>
#include <Viewport/EditorViewportWidget.h>
#include <BackendAPI/IEngineBackend.h>

#include <AzCore/Interface/Interface.h>
#include <AzCore/IO/FileIO.h>
#include <AzCore/IO/GenericStreams.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/functional.h>

#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Entity/PrefabEditorEntityOwnershipInterface.h>
#include <AzToolsFramework/Prefab/PrefabPublicInterface.h>
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
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QToolBar>
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    EditorMainWindow::EditorMainWindow(QWidget* parent)
        : AzQtComponents::DockMainWindow(parent)
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
        // instance (see Workspaces, plan §5.1).
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

        QMenu* editMenu = bar->addMenu(QStringLiteral("&Edit"));
        // Match the engine's EditorAction hot-key semantics (WidgetWithChildrenShortcut):
        // the action lives on the main window, so it fires whichever docked child holds
        // focus, without globally hijacking the shortcut. A full ActionManager-based
        // registration is deferred to the action/menu system phase.
        QAction* createEntity = editMenu->addAction(QStringLiteral("Create &Entity"));
        createEntity->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        createEntity->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
        addAction(createEntity);
        connect(createEntity, &QAction::triggered, this, &EditorMainWindow::OnCreateEntity);

        QAction* deleteSelection = editMenu->addAction(QStringLiteral("&Delete Selection"));
        deleteSelection->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        deleteSelection->setShortcut(QKeySequence::Delete);
        addAction(deleteSelection);
        connect(deleteSelection, &QAction::triggered, this, &EditorMainWindow::OnDeleteSelection);

        bar->addMenu(QStringLiteral("&View"));

        // Workspaces (Blender-style saved layouts, plan §5.1).
        QMenu* windowMenu = bar->addMenu(QStringLiteral("&Window"));
        QAction* saveWorkspace = windowMenu->addAction(QStringLiteral("Save &Workspace"));
        connect(saveWorkspace, &QAction::triggered, this, &EditorMainWindow::OnSaveWorkspace);
        QAction* restoreWorkspace = windowMenu->addAction(QStringLiteral("&Restore Workspace"));
        connect(restoreWorkspace, &QAction::triggered, this, &EditorMainWindow::OnRestoreWorkspace);

        // Command palette (VSCode/Blender-style, plan §5.2). A window-level shortcut so it
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
        // Hot-keys 1/2/3 mirror the common transform key bindings.
        auto* toolBar = addToolBar(QStringLiteral("Transform"));
        toolBar->setObjectName(QStringLiteral("TransformToolBar"));

        auto addModeAction = [this, toolBar](const QString& text, const QString& key, void (EditorMainWindow::*slot)())
        {
            QAction* action = toolBar->addAction(text);
            action->setCheckable(true);
            action->setShortcut(QKeySequence(key));
            action->setShortcutContext(Qt::WindowShortcut);
            connect(action, &QAction::triggered, this, slot);
            return action;
        };

        auto* group = new QActionGroup(this);
        group->setExclusive(true);
        QAction* move = addModeAction(QStringLiteral("Move"), QStringLiteral("1"), &EditorMainWindow::OnTransformModeMove);
        QAction* rotate = addModeAction(QStringLiteral("Rotate"), QStringLiteral("2"), &EditorMainWindow::OnTransformModeRotate);
        QAction* scale = addModeAction(QStringLiteral("Scale"), QStringLiteral("3"), &EditorMainWindow::OnTransformModeScale);
        group->addAction(move);
        group->addAction(rotate);
        group->addAction(scale);
        move->setChecked(true); // default mode is Move.

        // Gizmo theme switcher (Blender / Unreal, plan §9). Rebuilds the gizmo views on change.
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

        // Coordinate space switcher (World / Local, plan §5.4.2). World = axis-aligned handles;
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

        // Snap toggle (plan §5.5). One checkbox enables grid + angle + scale snap at the default
        // increments; per-increment tuning is deferred to a settings panel.
        toolBar->addSeparator();
        QAction* snap = toolBar->addAction(QStringLiteral("Snap"));
        snap->setCheckable(true);
        connect(snap, &QAction::toggled, this, [](bool enabled)
        {
            GizmoSnapSettings settings;
            settings.m_gridSnapEnabled = enabled;
            settings.m_angleSnapEnabled = enabled;
            settings.m_scaleSnapEnabled = enabled;
            GizmoControlRequestBus::Broadcast(&GizmoControlRequests::SetSnapSettings, settings);
        });
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

    void EditorMainWindow::BuildDockPanels()
    {
        // Engine-agnostic viewport surface. The manipulator/selection logic runs against
        // its camera state; the active backend renders overlay geometry into it (plan §6 阶段2).
        auto* viewport = new EditorViewportWidget(k_viewportId, this);
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (backend)
        {
            viewport->SetSceneRenderer(&backend->GetSceneRenderer());
        }

        // Reused AzToolsFramework widgets - they self-wire to the editor entity/selection
        // buses, so selection stays in sync across Outliner and Inspector for free.
        auto* outliner = new AzToolsFramework::EntityOutlinerWidget(this);
        auto* inspector = new AzToolsFramework::EntityPropertyEditor(this);

        // Real asset browser backed by the engine backend's asset source (plan §6 阶段4 / C8).
        QWidget* assetPanel = backend
            ? static_cast<QWidget*>(new AssetBrowserPanel(&backend->GetAssetSource(), this))
            : static_cast<QWidget*>(new QLabel(QStringLiteral("Asset Browser"), this));

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
        AddPanel(QStringLiteral("AssetBrowserDock"), QStringLiteral("Asset Browser"), assetPanel, Qt::BottomDockWidgetArea);
#else
        setCentralWidget(viewport);
        AddPanel(QStringLiteral("OutlinerDock"), QStringLiteral("Entity Outliner"), outliner, Qt::LeftDockWidgetArea);
        AddPanel(QStringLiteral("InspectorDock"), QStringLiteral("Inspector"), inspector, Qt::RightDockWidgetArea);
        AddPanel(QStringLiteral("AssetBrowserDock"), QStringLiteral("Asset Browser"), assetPanel, Qt::BottomDockWidgetArea);
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
        using namespace AzToolsFramework;

        AZ_Printf("CrossEngineEditor", "OnCreateEntity invoked.\n");

        auto* ownership = AZ::Interface<PrefabEditorEntityOwnershipInterface>::Get();
        auto* prefabPublic = AZ::Interface<Prefab::PrefabPublicInterface>::Get();
        if (!ownership || !prefabPublic)
        {
            AZ_Warning("CrossEngineEditor", false, "CreateEntity: ownership=%p prefabPublic=%p", ownership, prefabPublic);
            return;
        }

        Prefab::InstanceOptionalReference root = ownership->GetRootPrefabInstance();
        if (!root.has_value())
        {
            AZ_Warning("CrossEngineEditor", false, "CreateEntity: no root prefab instance.");
            return;
        }

        const AZ::EntityId parentId = root->get().GetContainerEntityId();

        // CreateEntity names, parents, adds required components and selects the entity,
        // all wrapped in a single undo batch (plan M1). Propagation runs on the next tick.
        auto result = prefabPublic->CreateEntity(parentId, AZ::Vector3::CreateZero());
        if (!result.IsSuccess())
        {
            AZ_Warning("CrossEngineEditor", false, "CreateEntity failed: %s", result.GetError().c_str());
            return;
        }
        AZ_Printf("CrossEngineEditor", "CreateEntity succeeded, id=%llu\n", static_cast<AZ::u64>(result.GetValue()));
    }

    void EditorMainWindow::OnDeleteSelection()
    {
        AzToolsFramework::ToolsApplicationRequestBus::Broadcast(
            &AzToolsFramework::ToolsApplicationRequests::DeleteSelected);
    }

    void EditorMainWindow::OnNewLevel()
    {
        // Recreate the level as a fresh in-memory root prefab (plan §6 阶段3.1).
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

    void EditorMainWindow::OnSaveLevel()
    {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Level"), QStringLiteral("NewLevel.prefab"), QStringLiteral("Prefab (*.prefab)"));
        if (path.isEmpty())
        {
            return;
        }

        auto* ownership = AZ::Interface<AzToolsFramework::PrefabEditorEntityOwnershipInterface>::Get();
        if (!ownership)
        {
            return;
        }

        AZ::IO::FileIOStream stream(path.toUtf8().constData(), AZ::IO::OpenMode::ModeWrite);
        if (!stream.IsOpen())
        {
            AZ_Warning("CrossEngineEditor", false, "SaveLevel: could not open %s for write", path.toUtf8().constData());
            return;
        }
        if (!ownership->SaveToStream(stream, path.toUtf8().constData()))
        {
            AZ_Warning("CrossEngineEditor", false, "SaveLevel: SaveToStream failed for %s", path.toUtf8().constData());
        }
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
        // are captured correctly (plan §5.1).
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
