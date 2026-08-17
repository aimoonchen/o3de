/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! (Plan §B4): forwards editor-side edits to the engine backend's
//! IEntityMirror, and pulls the engine's objects into the editor entity context.
//!
//! The editor's canonical object model is AZ::Entity + reflection, so all edits are
//! observed on the standard AzToolsFramework buses (transform + property changes) and
//! written back through IEntityMirror. This keeps the shell engine-agnostic: it never
//! talks to a concrete engine, only to the mirror contract.

#include <AzCore/Component/EntityId.h>
#include <Framework/EngineProperty.h>

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

        //! Full engine -> editor re-sync: destroys every mirror entity currently in the
        //! editor context (loose entities carrying an EngineNodeComponent), then calls
        //! SyncFromEngine again. Used after engine-side create/delete/paste, because the
        //! sync path always creates fresh mirror entities instead of reusing existing ones.
        void RefreshFromEngine();

        //! Route an asset to the first selected mirror entity by extension (rbfx_migration.md
        //! §3.4): .mat -> AssignMaterial(slot 0), .ani -> AssignAnimation. Spawnable scene
        //! assets (.mdl/.xml) are NOT handled here - the caller places those itself (drop =
        //! raycast hit point, AssetBrowser double-click = origin). Returns false when the
        //! extension is not assignable or no mirror entity is selected.
        bool AssignAssetToSelection(const AZStd::string& assetPath);

        //! Spawn a scene asset (.mdl/.xml) at the origin through the backend, then re-sync
        //! (rbfx_migration.md §3.4: AssetBrowser double-click). Mirrors the viewport drop path
        //! minus the raycast placement.
        void SpawnAssetAtOrigin(const AZStd::string& assetPath);

        //! First selected entity carrying an EngineNodeComponent (the assign / paste-parent
        //! target), or invalid when the selection has none.
        AZ::EntityId FirstSelectedMirrorId() const;

        //! Read a resource's editable values into a PropertyBag for the generic resource panel
        //! (rbfx_migration.md §3.3). type is the engine's resource type name (e.g. "Material").
        bool ReadResourceProperties(const AZStd::string& type, const AZStd::string& path, PropertyBag& out);

        //! Push the generic panel's edited values back into the resource (name-matched; the
        //! backend ignores unknown names).
        bool WriteResourceProperties(const AZStd::string& type, const AZStd::string& path, const PropertyBag& bag);

        //! Persist the open resource to its source file: the backend serializes the cached
        //! resource (already edited via WriteResourceProperties) in its native format (rbfx:
        //! Material::Save). Returns false on failure.
        bool SaveResource(const AZStd::string& type, const AZStd::string& path);

    private:
        // EditorTransformChangeNotificationBus::Handler...
        void OnEntityTransformChanged(const AzToolsFramework::EntityIdList& entityIds) override;

        // PropertyEditorEntityChangeNotificationBus::MultiHandler...
        //! Transform-component events (one per gizmo-drag frame) are routed to the transform-only
        //! push; everything else re-pushes the whole PropertyBag. Suppressed while syncing.
        void OnEntityComponentPropertyChanged(AZ::ComponentId componentId) override;

        // ToolsApplicationNotificationBus::Handler...
        void AfterEntitySelectionChanged(
            const AzToolsFramework::EntityIdList& newlySelectedEntities,
            const AzToolsFramework::EntityIdList& newlyDeselectedEntities) override;

        //! The registered backend's mirror, or nullptr when no backend is active.
        IEngineBackend* GetBackend() const;

        //! True while SyncFromEngine runs: sync writes (SetParent etc.) must not echo back
        //! to the engine through the property bus.
        bool m_syncing = false;
    };
} // namespace CrossEngineEditor
