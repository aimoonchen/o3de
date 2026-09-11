/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/string/string.h>

#include <AzQtComponents/Application/AzQtApplication.h>
#include <AzToolsFramework/API/EditorWindowRequestBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Application/ToolsApplication.h>

// Vendor: AtomToolsFramework Core types for material editor stack
#include "MaterialEditor/Vendor/AtomToolsFramework/Document/AtomToolsDocumentSystem.h"

#include <chrono>

namespace AzQtComponents
{
    class StyleManager;
    class WindowDecorationWrapper;
}

namespace AzToolsFramework
{
    class EntityPropertyEditor;
}

namespace CrossEngineEditor
{
    class CeeActionsHandler;
    class CeeMaterialPreviewPanel;
    class EditorMainWindow;
    class EntityMirrorBridge;
    class IEngineBackend;

    //! Material document system tool id (material_migration.md §6.3).
    inline constexpr AZ::Crc32 k_ceeMaterialToolId = AZ_CRC_CE("CEE_MaterialEditor");

    //! Cross-engine editor application (Plan §B1-§B4).
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

        //! Enter the Qt event loop, driving the AZ system tick from idle (Plan §B3).
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

        // ----- EditorRequests host contract (editor_polish.md P0-3 / A4) -------------------
        // The precise set of methods with live framework consumers; each delegates to the
        // main window's single-source-of-truth operation so the Outliner / Inspector context
        // menus, the pin button and "go to" links all drive the same backend path as the menus.
        bool IsLevelDocumentOpen() override;
        AzFramework::EntityContextId GetEntityContextId() override;
        AZ::EntityId CreateNewEntity(AZ::EntityId parentId) override;
        void CloneSelection(bool& handled) override;
        void DeleteSelectedEntities(bool includeDescendants) override;
        AZStd::string GetDefaultEntityIcon() override;
        AZStd::string GetComponentEditorIcon(const AZ::Uuid& componentType, const AZ::Component* component) override;
        AZStd::string GetComponentTypeEditorIcon(const AZ::Uuid& componentType) override;
        void GoToSelectedEntitiesInViewports() override;
        bool CanGoToSelectedEntitiesInViewports() override;
        void OpenPinnedInspector(const AzToolsFramework::EntityIdSet& entities) override;
        void ClosePinnedInspector(AzToolsFramework::EntityPropertyEditor* editor) override;

        //! Create an empty in-memory level prefab and focus it so the Outliner has a
        //! root container and entity creation works (Plan §A2 modern EC/Prefab workflow).
        //! Single implementation behind both startup and the File > New Level action.
        void CreateNewLevel();

    private:
        void OnIdle();

        //! Build the engine backend selected by --backend on the command line (Plan §B1),
        //! falling back to the best compiled-in option when the choice is unavailable.
        //! Records the resolved backend name in m_backendName for the status bar.
        AZStd::unique_ptr<IEngineBackend> CreateBackendFromCommandLine();

        AZStd::unique_ptr<AzQtComponents::StyleManager> m_styleManager;

        //! Window-decoration wrapper that gives the main window the same custom dark title bar
        //! as the native O3DE editor (CryEdit.cpp pattern). setGuest() reparents the guest into
        //! the wrapper and the wrapper deletes it, so m_mainWindow must be a raw pointer.
        AZStd::unique_ptr<AzQtComponents::WindowDecorationWrapper> m_mainWindowWrapper;
        EditorMainWindow* m_mainWindow = nullptr;

        AZStd::unique_ptr<IEngineBackend> m_backend;
        AZStd::unique_ptr<EntityMirrorBridge> m_mirrorBridge;

        //! ActionManager bootstrap (editor_polish.md P0-4): registers the action context,
        //! the CEE action pool and the three context menus when TriggerRegistrationNotifications
        //! runs. Owned here so it dies with (and before) the main window it points at.
        AZStd::unique_ptr<CeeActionsHandler> m_actionsHandler;

        //! Material document system (material_migration.md §6.3).
        //! Created after backend registration so GetMaterialSource() is available.
        AZStd::unique_ptr<AtomToolsFramework::AtomToolsDocumentSystem> m_materialDocumentSystem;

        //! Canonical name of the backend that actually got created (status bar display;
        //! the --backend arg may be empty or uncompiled, so the factory records it).
        AZStd::string m_backendName;

        //! Last time the engine backend was stepped. The idle loop spins fast (~1 ms) to keep O3DE's
        //! system tick / Qt events responsive, but the backend (which renders a whole engine frame
        //! per step) is throttled to display cadence here so it does not free-run at ~500 fps
        //! (wasted CPU/GPU, and the 1 ms-vs-16 ms beat produced periodic stutter spikes).
        std::chrono::steady_clock::time_point m_lastBackendTickTime = std::chrono::steady_clock::now();

        //! Set once the engine scene has been pulled into the editor (after the surface is
        //! ready and the backend has built its scene). Guards the one-shot SyncFromEngine.
        bool m_engineSynced = false;
    };
} // namespace CrossEngineEditor
