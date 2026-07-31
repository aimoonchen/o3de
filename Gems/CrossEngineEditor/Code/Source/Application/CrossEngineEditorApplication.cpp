/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Application/CrossEngineEditorApplication.h>
#include <Application/EntityMirrorBridge.h>
#include <Backends/NullBackend.h>
#if defined(CEE_HAVE_DILIGENT)
#include <Backends/DiligentBackend.h>
#endif
#include <BackendAPI/IEngineBackend.h>
#include <Viewport/CrossEngineViewportSelection.h>
#include <Window/EditorMainWindow.h>

#include <AzCore/Component/Entity.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>

#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzToolsFramework/Entity/PrefabEditorEntityOwnershipInterface.h>
#include <AzToolsFramework/Prefab/PrefabFocusPublicInterface.h>
#include <AzToolsFramework/Viewport/ViewportMessages.h>
#include <AzToolsFramework/ViewportSelection/EditorInteractionSystemViewportSelectionRequestBus.h>

#include <AzCore/std/smart_ptr/unique_ptr.h>

#include <AzQtComponents/Components/GlobalEventFilter.h>
#include <AzQtComponents/Components/StyleManager.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QTimer>
AZ_POP_DISABLE_WARNING

#include <chrono>
#include <cstdio>

namespace CrossEngineEditor
{
    namespace
    {
        void BootLog(const char* stage)
        {
            FILE* f = nullptr;
            if (fopen_s(&f, "cee_boot.log", "a") == 0 && f != nullptr)
            {
                fprintf(f, "%s\n", stage);
                fclose(f);
            }
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
    }

    void CrossEngineEditorApplication::CreateReflectionManager()
    {
        ToolsApplication::CreateReflectionManager();
        // EditContext drives the reflection-based Inspector (plan §2, Unity SerializedObject equivalent).
        GetSerializeContext()->CreateEditContext();
    }

    void CrossEngineEditorApplication::StartCommon(AZ::Entity* systemEntity)
    {
        BootLog("StartCommon:enter");
        ToolsApplication::StartCommon(systemEntity);
        BootLog("StartCommon:base-done");

        // Intercept general editor requests (required-component creation, asset browsing,
        // main-window parenting) before any entity/level is created.
        AzToolsFramework::EditorRequests::Bus::Handler::BusConnect();

        // Register the engine backend first so the main window can wire the viewport to its
        // scene renderer while building panels (plan §6 阶段1/阶段2/阶段3). When the Diligent
        // submodule is built, use the D3D12 backend that presents its swapchain straight into the
        // viewport's native surface; otherwise fall back to the do-nothing NullBackend. Real
        // engine backends replace this via the same AZ::Interface.
#if defined(CEE_HAVE_DILIGENT)
        m_backend = AZStd::make_unique<DiligentBackend>();
#else
        m_backend = AZStd::make_unique<NullBackend>();
#endif
        AZ::Interface<IEngineBackend>::Register(m_backend.get());

        // Bridge editor edits back to the engine object model and pull engine objects in
        // (plan §6 阶段4 / C6). Connects to the standard transform/property buses.
        m_mirrorBridge = AZStd::make_unique<EntityMirrorBridge>();

        m_mainWindow = AZStd::make_unique<EditorMainWindow>();
        BootLog("StartCommon:mainwindow-built");
        AzToolsFramework::EditorWindowRequestBus::Handler::BusConnect();

        // Install our viewport interaction handler. Unlike the engine default
        // (EditorDefaultSelection + EditorTransformComponentSelection), this keeps entity
        // picking but replaces the transform gizmos with our self-drawn themed gizmos (route B,
        // plan §9). Selecting an entity now produces our gizmos, not the engine's.
        AzToolsFramework::EditorInteractionSystemViewportSelectionRequestBus::Event(
            AzToolsFramework::GetEntityContextId(),
            &AzToolsFramework::EditorInteractionSystemViewportSelectionRequestBus::Events::SetHandler,
            [](const AzToolsFramework::EditorVisibleEntityDataCacheInterface* entityDataCache,
               AzToolsFramework::ViewportEditorModeTrackerInterface* /*viewportEditorModeTracker*/)
            {
                return AZStd::make_unique<CrossEngineViewportSelection>(entityDataCache, GizmoStyle::Blender);
            });

        CreateNewLevel();
        BootLog("StartCommon:level-created");

        m_mainWindow->show();
        BootLog("StartCommon:shown");
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
        m_mainWindow.reset();

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
        return m_mainWindow.get();
    }

    QWidget* CrossEngineEditorApplication::GetMainWindow()
    {
        return m_mainWindow.get();
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
        // Asset picking arrives with the asset source contract (plan §6 阶段4 / C8).
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
        if (WasExitMainLoopRequested())
        {
            BootLog("OnIdle:exit-requested");
            quit();
            return;
        }

        using Clock = std::chrono::steady_clock;
        const Clock::time_point now = Clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - m_lastIdleTime).count();
        m_lastIdleTime = now;

        PumpSystemEventLoopUntilEmpty();
        TickSystem();
        Tick();

        // C7: drive the engine backend (if one is registered) from the editor tick.
        if (auto* backend = AZ::Interface<IEngineBackend>::Get())
        {
            backend->Tick(deltaSeconds);
        }

        // Idle at a lower rate when the window is not active to free CPU/GPU.
        const int intervalMs = (applicationState() & Qt::ApplicationActive) ? 1 : 100;
        QTimer::singleShot(intervalMs, this, &CrossEngineEditorApplication::OnIdle);
    }
} // namespace CrossEngineEditor
