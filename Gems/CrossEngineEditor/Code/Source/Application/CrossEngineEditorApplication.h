/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

#include <AzCore/std/smart_ptr/unique_ptr.h>

#include <AzQtComponents/Application/AzQtApplication.h>
#include <AzToolsFramework/API/EditorWindowRequestBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Application/ToolsApplication.h>

#include <chrono>

namespace AzQtComponents
{
    class StyleManager;
}

namespace CrossEngineEditor
{
    class EditorMainWindow;
    class EntityMirrorBridge;
    class IEngineBackend;

    //! Cross-engine editor application (plan §3 / §4).
    //!
    //! Combines the Qt application (AzQtApplication) with the AZ tools application
    //! (ToolsApplication) so that the editor gets the EditorEntityContext, Prefab
    //! system, Undo and reflection for free, with no engine-runtime dependency.
    class CrossEngineEditorApplication
        : public AzQtComponents::AzQtApplication
        , public AzToolsFramework::ToolsApplication
        , private AzToolsFramework::EditorWindowRequestBus::Handler
        , private AzToolsFramework::EditorRequests::Bus::Handler
    {
    public:
        AZ_CLASS_ALLOCATOR(CrossEngineEditorApplication, AZ::SystemAllocator)
        AZ_RTTI(CrossEngineEditorApplication, "{4C8E1A90-3D2F-4B77-9A1E-6F0B2C7D5E33}", AzToolsFramework::ToolsApplication)

        CrossEngineEditorApplication(int* argc, char*** argv);
        ~CrossEngineEditorApplication() override;

        //! Enter the Qt event loop, driving the AZ system tick from idle (plan §7).
        void RunMainLoop();

        // AzFramework::Application / ToolsApplication overrides...
        const char* GetCurrentConfigurationName() const override;
        void Reflect(AZ::ReflectContext* context) override;
        void CreateReflectionManager() override;
        void StartCommon(AZ::Entity* systemEntity) override;
        void Destroy() override;

        // AzToolsFramework::EditorWindowRequestBus::Handler...
        QWidget* GetAppMainWindow() override;

        // AzToolsFramework::EditorRequests::Bus::Handler...
        //! Adds the editor components (e.g. transform) a prefab entity needs to be usable.
        void CreateEditorRepresentation(AZ::Entity* entity) override;
        void BrowseForAssets(AzToolsFramework::AssetBrowser::AssetSelectionModel& selection) override;
        QWidget* GetMainWindow() override;

    private:
        void OnIdle();

        //! Create an empty in-memory level prefab and focus it so the Outliner has a
        //! root container and entity creation works (plan §2 modern EC/Prefab workflow).
        void CreateNewLevel();

        AZStd::unique_ptr<AzQtComponents::StyleManager> m_styleManager;
        AZStd::unique_ptr<EditorMainWindow> m_mainWindow;
        AZStd::unique_ptr<IEngineBackend> m_backend;
        AZStd::unique_ptr<EntityMirrorBridge> m_mirrorBridge;

        //! Last idle timestamp, used to derive the per-frame delta for the backend tick.
        std::chrono::steady_clock::time_point m_lastIdleTime = std::chrono::steady_clock::now();
    };
} // namespace CrossEngineEditor
