/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Phase 4 (plan §6 4.1/4.2): forwards editor-side edits to the engine backend's
//! IEntityMirror, and pulls the engine's objects into the editor entity context.
//!
//! The editor's canonical object model is AZ::Entity + reflection, so all edits are
//! observed on the standard AzToolsFramework buses (transform + property changes) and
//! written back through IEntityMirror. This keeps the shell engine-agnostic: it never
//! talks to a concrete engine, only to the mirror contract.

#include <AzCore/Component/EntityId.h>

#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Entity/EditorEntityTransformBus.h>
#include <AzToolsFramework/UI/PropertyEditor/PropertyEditorAPI.h>

namespace CrossEngineEditor
{
    class IEngineBackend;

    class EntityMirrorBridge final
        : private AzToolsFramework::EditorTransformChangeNotificationBus::Handler
        , private AzToolsFramework::PropertyEditorEntityChangeNotificationBus::MultiHandler
        , private AzToolsFramework::ToolsApplicationNotificationBus::Handler
    {
    public:
        EntityMirrorBridge();
        ~EntityMirrorBridge();

        //! Engine -> editor: ask the backend for its objects (currently a no-op for
        //! NullBackend; real backends populate the editor entity context).
        void SyncFromEngine();

    private:
        // EditorTransformChangeNotificationBus::Handler...
        void OnEntityTransformChanged(const AzToolsFramework::EntityIdList& entityIds) override;

        // PropertyEditorEntityChangeNotificationBus::MultiHandler...
        void OnEntityComponentPropertyChanged(AZ::ComponentId componentId) override;

        // ToolsApplicationNotificationBus::Handler...
        void AfterEntitySelectionChanged(
            const AzToolsFramework::EntityIdList& newlySelectedEntities,
            const AzToolsFramework::EntityIdList& newlyDeselectedEntities) override;

        //! The registered backend's mirror, or nullptr when no backend is active.
        IEngineBackend* GetBackend() const;
    };
} // namespace CrossEngineEditor
