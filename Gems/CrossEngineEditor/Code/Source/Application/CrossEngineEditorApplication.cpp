/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Application/CrossEngineEditorApplication.h>
#include <Application/EntityMirrorBridge.h>
#include <Framework/EngineNodeComponent.h>
#include <Backends/NullBackend.h>
#if defined(CEE_HAVE_DILIGENT)
#include <Backends/DiligentBackend.h>
#endif
#if defined(CEE_HAVE_RBFX)
#include <Backends/RbfxBackend.h>
#endif
#if defined(CEE_HAVE_GODOT)
#include <Backends/GodotBackend.h>
#endif
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IMaterialSource.h>
#include <BackendAPI/IViewportTick.h>
#include <Viewport/CrossEngineViewportSelection.h>
#include <Window/EditorMainWindow.h>

#include <AzCore/Component/Entity.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>

#include <AzToolsFramework/ActionManager/ActionManagerSystemComponent.h>
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>
#include <AzToolsFramework/Editor/ActionManagerIdentifiers/EditorContextIdentifiers.h>
#include <Window/CeePreferences.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzToolsFramework/Entity/PrefabEditorEntityOwnershipInterface.h>
#include <AzToolsFramework/Prefab/PrefabFocusPublicInterface.h>
#include <AzToolsFramework/Viewport/ViewportMessages.h>
#include <AzToolsFramework/ViewportSelection/EditorInteractionSystemViewportSelectionRequestBus.h>

#include <Window/CeeActionsHandler.h>
#include <MaterialEditor/CeeMaterialDocument.h>
#include <MaterialEditor/CeeMaterialPreviewPanel.h>

// Vendor: AtomToolsFramework Core types for material editor stack
#include "MaterialEditor/Vendor/AtomToolsFramework/Document/AtomToolsDocumentSystem.h"
#include "MaterialEditor/Vendor/AtomToolsFramework/Inspector/InspectorWidget.h"
#include "MaterialEditor/Vendor/AtomToolsFramework/Inspector/PropertyWidgets/PropertyStringBrowseEditCtrl.h"

#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/string/conversions.h>

#include <AzQtComponents/Components/GlobalEventFilter.h>
#include <AzQtComponents/Components/StyleManager.h>
#include <AzQtComponents/Components/WindowDecorationWrapper.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QTimer>
AZ_POP_DISABLE_WARNING

#include <chrono>

#include <Profiling/CrossEngineProfiler.h>

namespace CrossEngineEditor
{
    namespace
    {
        //! Read the value following a "--key" token in the argument list, or an empty string.
        AZStd::string ReadArgValue(const QStringList& args, const char* key)
        {
            const int idx = args.indexOf(QString::fromUtf8(key));
            if (idx >= 0 && idx + 1 < args.size())
            {
                return AZStd::string(args.at(idx + 1).toUtf8().constData());
            }
            return {};
        }
    } // namespace

    CrossEngineEditorApplication::CrossEngineEditorApplication(int* argc, char*** argv)
        : AzQtComponents::AzQtApplication(*argc, *argv)
        , AzToolsFramework::ToolsApplication(argc, argv)
    {
        setOrganizationName(QStringLiteral("CrossEngineEditor"));
        setApplicationName(QStringLiteral("CrossEngineEditor"));

        AZ::SettingsRegistryMergeUtils::MergeSettingsToRegistry_AddBuildSystemTargetSpecialization(
            *AZ::SettingsRegistry::Get(), LY_CMAKE_TARGET);

        installEventFilter(new AzQtComponents::GlobalEventFilter(this));

        AZ::IO::FixedMaxPath engineRootPath;
        if (auto* settingsRegistry = AZ::SettingsRegistry::Get())
        {
            settingsRegistry->Get(engineRootPath.Native(), AZ::SettingsRegistryMergeUtils::FilePathKey_EngineRootFolder);
        }
        m_styleManager = AZStd::make_unique<AzQtComponents::StyleManager>(this);
        m_styleManager->initialize(this, engineRootPath);
    }

    CrossEngineEditorApplication::~CrossEngineEditorApplication()
    {
        if (AzToolsFramework::EditorRequests::Bus::Handler::BusIsConnected())
        {
            AzToolsFramework::EditorRequests::Bus::Handler::BusDisconnect();
        }
        if (AzToolsFramework::EditorWindowRequestBus::Handler::BusIsConnected())
        {
            AzToolsFramework::EditorWindowRequestBus::Handler::BusDisconnect();
        }
        m_styleManager.reset();
    }

    const char* CrossEngineEditorApplication::GetCurrentConfigurationName() const
    {
        return "CrossEngineEditor";
    }

    void CrossEngineEditorApplication::Reflect(AZ::ReflectContext* context)
    {
        ToolsApplication::Reflect(context);
        // Reflect AtomToolsFramework.Core types (DynamicProperty, InspectorWidget, etc.)
        // that the material editor stack needs. These are vendored copies in MaterialEditor/Vendor/.
        AtomToolsFramework::AtomToolsDocument::Reflect(context);
        AtomToolsFramework::AtomToolsDocumentSystem::Reflect(context);
        AtomToolsFramework::DynamicProperty::Reflect(context);
        AtomToolsFramework::DynamicPropertyGroup::Reflect(context);
        AtomToolsFramework::InspectorWidget::Reflect(context);

        // EngineNodeComponent is NOT reflected here: RegisterComponentDescriptor (in StartCommon)
        // reflects it through the descriptor's own entry point. Reflecting it here as well would be
        // a second, independent reflection entry for the same types, so EngineProperty et al. would
        // be registered twice and SerializeContext asserts on the duplicated Uuid.
        // CeePreferences is a plain reflected value type (no descriptor), so it registers here -
        // the Preferences dialog's ReflectedPropertyEditor resolves it through the serialize
        // context (editor_polish.md P2 / D9).
        CeePreferences::Reflect(context);
    }

    void CrossEngineEditorApplication::CreateReflectionManager()
    {
        ToolsApplication::CreateReflectionManager();
        // EditContext drives the reflection-based Inspector (Plan §B4, Unity SerializedObject equivalent).
        GetSerializeContext()->CreateEditContext();
    }

    void CrossEngineEditorApplication::StartCommon(AZ::Entity* systemEntity)
    {
        ToolsApplication::StartCommon(systemEntity);

        // Register the mirror component descriptor so it can be added to mirror entities and
        // shown in the Inspector (Plan §B4). The descriptor drives reflection (its
        // Reflect() runs via the standard descriptor path, not a manual call).
        RegisterComponentDescriptor(EngineNodeComponent::CreateDescriptor());

        // Intercept general editor requests (required-component creation, asset browsing,
        // main-window parenting) before any entity/level is created.
        AzToolsFramework::EditorRequests::Bus::Handler::BusConnect();

        // Register the engine backend first so the main window can wire the viewport to its
        // scene renderer while building panels. The backend is chosen by --backend on the
        // command line (Plan §B1): rbfx | godot | diligent | null. Unavailable choices
        // (submodule not built) fall back to the best compiled-in option. --project <path>
        // selects the engine project to edit and is forwarded via BackendInitParams.
        m_backend = CreateBackendFromCommandLine();
        AZ::Interface<IEngineBackend>::Register(m_backend.get());

        // Bring the engine runtime up for the selected project (Plan §B1). Surface
        // creation still flows through ISceneRenderer signals; this readies the entity mirror
        // and asset source. A failure is non-fatal - the shell keeps running on the empty
        // contracts so the editor UI still comes up.
        {
            const QStringList args = arguments();
            BackendInitParams initParams;
            initParams.m_projectPath = ReadArgValue(args, "--project");
            initParams.m_scenePath = ReadArgValue(args, "--scene");
            if (auto result = m_backend->Initialize(initParams); !result.has_value())
            {
                AZ_Warning("CrossEngineEditor", false, "Engine backend Initialize failed; running on empty contracts.");
            }
        }

        // Bridge editor edits back to the engine object model and pull engine objects in
        // (Plan §B4 / A6 C6). Connects to the standard transform/property buses.
        m_mirrorBridge = AZStd::make_unique<EntityMirrorBridge>();

        // Register the action contexts BEFORE the main window and its child widgets are constructed.
        // Framework controls (EntityPropertyEditor, AssetBrowserTreeView, ConsoleTextEdit) self-assign
        // to their context in their constructors (AssignWidgetToActionContextHelper), which requires
        // the context to already exist (HotKeyManager.cpp:41-47: watcher lookup fails silently
        // otherwise). The full CeeActionsHandler registration (including AssignWidgetToActionContext
        // for the main window) still runs later via TriggerRegistrationNotifications.
        {
            auto* actionManager = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get();
            if (actionManager)
            {
                for (const AZStd::string_view contextId :
                     { EditorIdentifiers::MainWindowActionContextIdentifier,
                       EditorIdentifiers::EditorAssetBrowserActionContextIdentifier,
                       EditorIdentifiers::EditorConsoleActionContextIdentifier,
                       EditorIdentifiers::EditorEntityPropertyEditorActionContextIdentifier })
                {
                    AzToolsFramework::ActionContextProperties contextProperties;
                    contextProperties.m_name = "Cross-Engine Editor";
                    actionManager->RegisterActionContext(AZStd::string(contextId), contextProperties);
                }
            }
        }

        m_mainWindow = new EditorMainWindow(m_mirrorBridge.get());

        // Wrap the main window like the native O3DE editor does (CryEdit.cpp:1537-1543). Without
        // the wrapper the window keeps the native Windows title bar: its light-theme color clashes
        // with the dark editor UI, and hovering the native maximize button opens the Win11 Snap
        // Layouts flyout. The wrapper draws the themed AzQtComponents title bar (with its own
        // min/max/close buttons) over the native one, and keeps native dragging and Aero Snap via
        // its WM_NCHITTEST handler. setGuest() reparents the guest, so the wrapper owns it.
#ifdef Q_OS_MACOS
        m_mainWindowWrapper = AZStd::make_unique<AzQtComponents::WindowDecorationWrapper>(
            AzQtComponents::WindowDecorationWrapper::OptionDisabled);
#else
        m_mainWindowWrapper = AZStd::make_unique<AzQtComponents::WindowDecorationWrapper>(
            AzQtComponents::WindowDecorationWrapper::OptionAutoTitleBarButtons);
#endif
        m_mainWindowWrapper->setGuest(m_mainWindow);
        AzToolsFramework::EditorWindowRequestBus::Handler::BusConnect();

        // Status bar + title scene name (editor_polish.md P1-17 / P2): project path and backend
        // name come from the command line - no contract growth (the backend has no name/version
        // query; the --backend choice IS the displayed name, version is deferred with the doc).
        {
            AZStd::string backendName = ReadArgValue(arguments(), "--backend");
            if (backendName.empty())
            {
                backendName = "default";
            }
            const AZStd::string projectPath = ReadArgValue(arguments(), "--project");
            m_mainWindow->SetStatusInfo(
                QString::fromUtf8(projectPath.c_str()), QString::fromUtf8(backendName.c_str()));

            const AZStd::string scenePath = ReadArgValue(arguments(), "--scene");
            if (!scenePath.empty())
            {
                m_mainWindow->SetSceneDisplayName(QString::fromUtf8(scenePath.c_str()));
            }
        }

        // Install our viewport interaction handler. Unlike the engine default
        // (EditorDefaultSelection + EditorTransformComponentSelection), this keeps entity
        // picking but replaces the transform gizmos with our self-drawn themed gizmos (route B,
        // Plan §B7). Selecting an entity now produces our gizmos, not the engine's. The startup
        // style comes from the persisted preferences (P2), defaulting to Blender.
        AzToolsFramework::EditorInteractionSystemViewportSelectionRequestBus::Event(
            AzToolsFramework::GetEntityContextId(),
            &AzToolsFramework::EditorInteractionSystemViewportSelectionRequestBus::Events::SetHandler,
            [defaultStyle = m_mainWindow->GetPreferences().m_defaultGizmoStyle](
                const AzToolsFramework::EditorVisibleEntityDataCacheInterface* entityDataCache,
                AzToolsFramework::ViewportEditorModeTrackerInterface* viewportEditorModeTracker)
            {
                return AZStd::make_unique<CrossEngineViewportSelection>(
                    entityDataCache, viewportEditorModeTracker,
                    defaultStyle == CeePreferences::GizmoStylePreference::Unreal ? GizmoStyle::Unreal
                                                                                : GizmoStyle::Blender);
            });

        CreateNewLevel();

        // ActionManager bootstrap (editor_polish.md P0-4): create the registration handler and
        // fire the one-shot registration broadcast, positioned like the native editor's
        // CryEdit.cpp:1669-1676 - main window built, layout restored, before show(). Framework
        // components with registered handlers (e.g. GlobalPaintBrushSettingsSystemComponent)
        // activate here too. Later components (material panel etc.) register directly through
        // the ActionManager interfaces and never need a second trigger (A7).
        m_actionsHandler = AZStd::make_unique<CeeActionsHandler>(m_mainWindow);
        AzToolsFramework::ActionManagerSystemComponent::TriggerRegistrationNotifications();

        // Material document system bootstrap (material_migration_final.md SS6.3).
        // Create the document system and register the material document type.
        // This is positioned after backend registration so GetMaterialSource() is available.
        m_materialDocumentSystem = new AtomToolsFramework::AtomToolsDocumentSystem(k_ceeMaterialToolId);
        {
            auto typeInfo = CeeMaterialDocument::BuildDocumentTypeInfo();
            m_materialDocumentSystem->RegisterDocumentType(typeInfo);
        }

        // Register the StringFilePath property handler for texture file browsing (S5).
        AtomToolsFramework::RegisterStringBrowseEditHandler();

        m_mainWindow->show();
    }

    void CrossEngineEditorApplication::Destroy()
    {
        // Tear down the UI first: the viewport holds a raw pointer into the backend's
        // scene renderer, so the window must die before the backend is released.
        if (AzToolsFramework::EditorRequests::Bus::Handler::BusIsConnected())
        {
            AzToolsFramework::EditorRequests::Bus::Handler::BusDisconnect();
        }
        if (AzToolsFramework::EditorWindowRequestBus::Handler::BusIsConnected())
        {
            AzToolsFramework::EditorWindowRequestBus::Handler::BusDisconnect();
        }
        // The actions handler points at the main window and its registered actions parent
        // their QActions to ActionManager-owned storage - drop it before the window dies.
        // Order matters: ActionManagerSystemComponent::Destroy() runs during
        // ToolsApplication::Destroy() and tears down the action/menu managers; the handler's
        // dtor disconnects from buses those managers own, so it must die first.
        m_actionsHandler.reset();

        // Material document system: close all documents before tearing down.
        if (m_materialDocumentSystem)
        {
            m_materialDocumentSystem->CloseAllDocuments();
            delete m_materialDocumentSystem;
            m_materialDocumentSystem = nullptr;
        }

        // Reset the wrapper, not the guest: the wrapper deletes the guest.
        m_mainWindowWrapper.reset();
        m_mainWindow = nullptr;

        // The bridge holds bus connections keyed off the entity context; drop it before
        // the backend it forwards to is unregistered.
        m_mirrorBridge.reset();

        if (m_backend)
        {
            AZ::Interface<IEngineBackend>::Unregister(m_backend.get());
            m_backend.reset();
        }
        ToolsApplication::Destroy();
    }

    QWidget* CrossEngineEditorApplication::GetAppMainWindow()
    {
        // Return the guest, not the wrapper - the native editor does the same
        // (SandboxIntegrationManager::GetAppMainWindow returns MainWindow::instance()).
        return m_mainWindow;
    }

    QWidget* CrossEngineEditorApplication::GetMainWindow()
    {
        return m_mainWindow;
    }

    AZStd::unique_ptr<IEngineBackend> CrossEngineEditorApplication::CreateBackendFromCommandLine()
    {
        AZStd::string choice = ReadArgValue(arguments(), "--backend");
        AZStd::to_lower(choice.begin(), choice.end());

#if defined(CEE_HAVE_RBFX)
        if (choice == "rbfx")
        {
            return AZStd::make_unique<RbfxBackend>();
        }
#endif
#if defined(CEE_HAVE_GODOT)
        if (choice == "godot")
        {
            return AZStd::make_unique<GodotBackend>();
        }
#endif
#if defined(CEE_HAVE_DILIGENT)
        if (choice == "diligent" || choice.empty())
        {
            // Diligent is the default "no-engine" demo backend when nothing is requested.
            return AZStd::make_unique<DiligentBackend>();
        }
#endif
        // "null", an unknown value, or no compiled-in match: run on the do-nothing backend.
        return AZStd::make_unique<NullBackend>();
    }

    void CrossEngineEditorApplication::CreateEditorRepresentation(AZ::Entity* entity)
    {
        if (!entity)
        {
            return;
        }
        AzToolsFramework::EditorEntityContextRequestBus::Broadcast(
            &AzToolsFramework::EditorEntityContextRequests::AddRequiredComponents, *entity);
    }

    void CrossEngineEditorApplication::BrowseForAssets(AzToolsFramework::AssetBrowser::AssetSelectionModel& /*selection*/)
    {
        // v1 opens the Asset Browser dock without consuming the selection model
        // (no asset picking pipeline yet, rbfx_migration.md §2.8).
        if (m_mainWindow)
        {
            m_mainWindow->ShowAssetBrowser();
        }
    }

    // ------------------------------------------------------- EditorRequests (P0-3 / A4)

    bool CrossEngineEditorApplication::IsLevelDocumentOpen()
    {
        // CEE always has the level container open (CreateNewLevel at startup); the Outliner
        // gates its whole context menu on this answer (EntityOutlinerWidget.cpp:582-586).
        return true;
    }

    AzFramework::EntityContextId CrossEngineEditorApplication::GetEntityContextId()
    {
        return AzToolsFramework::GetEntityContextId();
    }

    AZ::EntityId CrossEngineEditorApplication::CreateNewEntity([[maybe_unused]] AZ::EntityId parentId)
    {
        // Delegate to the create path the Edit menu uses. parentId is intentionally unused: the
        // mirror contract creates engine nodes at the scene root (no parent in ObjectSpec);
        // parent-aware creation waits for a real consumer (A4 scope ruling).
        return m_mainWindow ? m_mainWindow->CreateEngineEntity() : AZ::EntityId();
    }

    void CrossEngineEditorApplication::CloneSelection(bool& handled)
    {
        // The Outliner's "Duplicate" (DoDuplicateSelection) routes here inside a
        // ScopedUndoBatch; the backend duplicate path is the same one the Edit menu drives.
        if (m_mainWindow)
        {
            m_mainWindow->DuplicateSelection();
            handled = true;
        }
    }

    void CrossEngineEditorApplication::DeleteSelectedEntities([[maybe_unused]] bool includeDescendants)
    {
        // Engine-side removal already cascades to node subtrees, so includeDescendants needs no
        // separate path; the post-delete re-mirror clears any orphaned editor entities.
        if (m_mainWindow)
        {
            m_mainWindow->DeleteSelection();
        }
    }

    AZStd::string CrossEngineEditorApplication::GetDefaultEntityIcon()
    {
        // Stock AzQtComponents resource (the same icon EntityOutlinerListModel falls back to);
        // it is what the Inspector's entity header shows when no per-entity icon exists.
        return ":/Entity/entity.svg";
    }

    AZStd::string CrossEngineEditorApplication::GetComponentEditorIcon(
        const AZ::Uuid& componentType, const AZ::Component* /*component*/)
    {
        // The Inspector's component-header icon path. Only mirrored engine nodes get an icon;
        // everything else (transform etc.) stays icon-less, matching the native editor where
        // components opt in through their edit context.
        if (componentType == azrtti_typeid<EngineNodeComponent>())
        {
            return ":/Entity/entity.svg";
        }
        return {};
    }

    AZStd::string CrossEngineEditorApplication::GetComponentTypeEditorIcon(const AZ::Uuid& componentType)
    {
        // Type-keyed twin of GetComponentEditorIcon (palette-facing; harmless to answer now).
        if (componentType == azrtti_typeid<EngineNodeComponent>())
        {
            return ":/Entity/entity.svg";
        }
        return {};
    }

    void CrossEngineEditorApplication::GoToSelectedEntitiesInViewports()
    {
        // "Focus on selection": EntityIdQLabel (Inspector entity links) and the Outliner menu
        // route here. The main window frames the shared editor viewport camera.
        if (m_mainWindow)
        {
            m_mainWindow->FocusSelection();
        }
    }

    bool CrossEngineEditorApplication::CanGoToSelectedEntitiesInViewports()
    {
        // Direct call, not the bus: this application IS the ToolsApplication (and thus the
        // selection holder), and forming the member pointer through our own private-inheriting
        // base chain is ill-formed (C2247). Unqualified lookup finds the public override.
        return AreAnyEntitiesSelected();
    }

    void CrossEngineEditorApplication::OpenPinnedInspector(const AzToolsFramework::EntityIdSet& entities)
    {
        // The Inspector's pin button (EntityPropertyEditor.cpp:605) asks the host app for a
        // second, locked grid. Implementation lives in the main window (P0-3 / A3).
        if (m_mainWindow)
        {
            m_mainWindow->OpenPinnedInspector(entities);
        }
    }

    void CrossEngineEditorApplication::ClosePinnedInspector(AzToolsFramework::EntityPropertyEditor* editor)
    {
        if (m_mainWindow)
        {
            m_mainWindow->ClosePinnedInspector(editor);
        }
    }

    void CrossEngineEditorApplication::CreateNewLevel()
    {
        using namespace AzToolsFramework;

        auto* ownership = AZ::Interface<PrefabEditorEntityOwnershipInterface>::Get();
        if (!ownership)
        {
            AZ_Warning("CrossEngineEditor", false, "PrefabEditorEntityOwnershipInterface unavailable; cannot create level.");
            return;
        }

        ownership->CreateNewLevelPrefab("NewLevel.prefab", "");

        Prefab::InstanceOptionalReference rootInstance = ownership->GetRootPrefabInstance();
        if (!rootInstance.has_value())
        {
            AZ_Warning("CrossEngineEditor", false, "Root prefab instance was not created.");
            return;
        }

        Prefab::EntityOptionalReference rootContainer = rootInstance->get().GetContainerEntity();
        if (!rootContainer.has_value())
        {
            return;
        }
        if (rootContainer->get().GetState() == AZ::Entity::State::Constructed)
        {
            rootContainer->get().Init();
        }

        if (auto* prefabFocus = AZ::Interface<Prefab::PrefabFocusPublicInterface>::Get())
        {
            prefabFocus->FocusOnOwningPrefab(rootContainer->get().GetId());
        }
    }

    void CrossEngineEditorApplication::RunMainLoop()
    {
        QTimer::singleShot(0, this, [this]() { OnIdle(); });
        exec();
    }

    void CrossEngineEditorApplication::OnIdle()
    {
        CEE_PROFILE_FUNCTION();
        if (WasExitMainLoopRequested())
        {
            quit();
            return;
        }

        using Clock = std::chrono::steady_clock;
        const Clock::time_point now = Clock::now();

        // NOTE: OS message pumping is intentionally NOT done here. This is a Qt-hosted editor, so OS
        // input (incl. WM_MOUSEMOVE) is drained by Qt's own dispatcher below - matching the native
        // O3DE editor, which does zero PeekMessage/DispatchMessage in OnIdle. A former hand-written
        // Win32 pump here double-pumped the queue and spun hundreds of moves/frame during orbit; the
        // real per-move fix is EditorViewportWidget::ApplyPendingMouseMove (Progress.md fix13).
        {
            CEE_PROFILE_SCOPE("Idle::TickSystem");
            TickSystem();
        }
        {
            CEE_PROFILE_SCOPE("Idle::Tick");
            Tick();
        }

        // Single main loop (Unreal/Godot/rbfx all render as one step of one loop, presenting once per
        // frame). The idle loop spins fast (~1 ms) to keep O3DE SystemTick / Qt input responsive; the
        // render frame is gated to ~60 fps by this SINGLE throttle - the sole cadence (not redundant
        // with vsync: Godot presents on this thread with vsync OFF, Progress.md fix11/fix12). Order matters:
        // TickRender FIRST (camera + overlay; rbfx presents in EndOverlayFrame), THEN backend->Tick
        // (Godot's iteration() presents with the just-submitted overlay). One present per backend/frame.
        constexpr float k_frameIntervalSeconds = 1.0f / 60.0f;
        const float sinceLastFrame = std::chrono::duration<float>(now - m_lastBackendTickTime).count();
        if (sinceLastFrame >= k_frameIntervalSeconds)
        {
            m_lastBackendTickTime = now;

            if (auto* viewportTick = AZ::Interface<IViewportTick>::Get())
            {
                viewportTick->TickRender(sinceLastFrame);
            }

            if (auto* backend = AZ::Interface<IEngineBackend>::Get())
            {
                backend->Tick(sinceLastFrame);

                // One-shot engine->editor pull. The backend builds its scene lazily on first surface
                // expose (OnSurfaceCreated), so we can only mirror its nodes into the Outliner once
                // the surface is ready. Do it exactly once.
                if (!m_engineSynced && m_mirrorBridge && backend->GetSceneRenderer().IsSurfaceReady())
                {
                    m_mirrorBridge->SyncFromEngine();
                    m_engineSynced = true;
                }

                // Material preview readback (material_migration_final.md SS6.4.4).
                // Called after backend->Tick() so the preview frame is ready.
                // Throttled to 10-15 Hz by the dock preview panel's dirty flag + timestamp.
                auto* previewDock = m_mainWindow ? m_mainWindow->FindMaterialPreviewPanel() : nullptr;
                if (previewDock && previewDock->IsDirty() && previewDock->isVisible())
                {
                    using sys_clock = AZStd::chrono::system_clock;
                    const auto previewNowMs = AZStd::chrono::duration_cast<AZStd::chrono::milliseconds>(
                        sys_clock::now().time_since_epoch()).count();
                    constexpr decltype(previewNowMs) k_previewIntervalMs = 100; // ~10 Hz
                    if (previewNowMs - previewDock->LastAcquireTime() >= k_previewIntervalMs)
                    {
                        AZStd::vector<AZ::u8> pixels;
                        IMaterialSource& matSrc = backend->GetMaterialSource();
                        PreviewResult result = matSrc.AcquirePreviewImage(512, 512, pixels);
                        previewDock->SetLastAcquireTime(previewNowMs);
                        if (result == PreviewResult::Updated)
                        {
                            previewDock->SetImageData(512, 512, pixels);
                        }
                        else if (result == PreviewResult::Unsupported)
                        {
                            previewDock->ClearDirty();
                        }
                        else if (result == PreviewResult::Unchanged)
                        {
                            // Preview frame not ready yet; keep dirty for next poll.
                        }
                    }
                }
            }

            // Frame boundary for Tracy sits on the throttle gate: one FrameMark == one real
            // present frame (~60 fps), not the ~1 kHz idle pump, so per-frame zones group under a
            // stable frame and FPS/spike patterns are readable (fixes the old ~1000 "fps" artifact).
            CEE_PROFILE_FRAME();
        }

        // Idle at a lower rate when the window is not active to free CPU/GPU.
        const int intervalMs = (applicationState() & Qt::ApplicationActive) ? 1 : 100;
        QTimer::singleShot(intervalMs, this, &CrossEngineEditorApplication::OnIdle);
    }
} // namespace CrossEngineEditor
