/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Application/EntityMirrorBridge.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>

#include <AzCore/Component/Entity.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Interface/Interface.h>

namespace CrossEngineEditor
{
    EntityMirrorBridge::EntityMirrorBridge()
    {
        AzToolsFramework::EditorTransformChangeNotificationBus::Handler::BusConnect();
        AzToolsFramework::ToolsApplicationNotificationBus::Handler::BusConnect();
    }

    EntityMirrorBridge::~EntityMirrorBridge()
    {
        AzToolsFramework::PropertyEditorEntityChangeNotificationBus::MultiHandler::BusDisconnect();
        AzToolsFramework::ToolsApplicationNotificationBus::Handler::BusDisconnect();
        AzToolsFramework::EditorTransformChangeNotificationBus::Handler::BusDisconnect();
    }

    IEngineBackend* EntityMirrorBridge::GetBackend() const
    {
        return AZ::Interface<IEngineBackend>::Get();
    }

    void EntityMirrorBridge::SyncFromEngine()
    {
        // Engine -> editor. NullBackend returns nothing; a real backend fills the list
        // and the shell would add the entities to the editor entity context.
        if (auto* backend = GetBackend())
        {
            AZStd::vector<AZ::Entity*> mirrored;
            backend->GetEntityMirror().SyncToEditor(mirrored);
        }
    }

    void EntityMirrorBridge::OnEntityTransformChanged(const AzToolsFramework::EntityIdList& entityIds)
    {
        auto* backend = GetBackend();
        if (!backend)
        {
            return;
        }

        IEntityMirror& mirror = backend->GetEntityMirror();
        for (const AZ::EntityId entityId : entityIds)
        {
            AZ::Transform worldTm = AZ::Transform::CreateIdentity();
            AZ::TransformBus::EventResult(worldTm, entityId, &AZ::TransformBus::Events::GetWorldTM);
            mirror.OnEditorTransformChanged(entityId, worldTm);
        }
    }

    void EntityMirrorBridge::OnEntityComponentPropertyChanged(AZ::ComponentId /*componentId*/)
    {
        auto* backend = GetBackend();
        if (!backend)
        {
            return;
        }

        const AZ::EntityId entityId = *AzToolsFramework::PropertyEditorEntityChangeNotificationBus::GetCurrentBusId();
        backend->GetEntityMirror().OnEditorPropertyChanged(entityId, PropertyChange{});
    }

    void EntityMirrorBridge::AfterEntitySelectionChanged(
        const AzToolsFramework::EntityIdList& newlySelectedEntities,
        const AzToolsFramework::EntityIdList& newlyDeselectedEntities)
    {
        // Only listen for property changes on the current selection: the property editor
        // fires this bus addressed by entity id, so we mirror connect/disconnect to it.
        for (const AZ::EntityId entityId : newlyDeselectedEntities)
        {
            AzToolsFramework::PropertyEditorEntityChangeNotificationBus::MultiHandler::BusDisconnect(entityId);
        }
        for (const AZ::EntityId entityId : newlySelectedEntities)
        {
            AzToolsFramework::PropertyEditorEntityChangeNotificationBus::MultiHandler::BusConnect(entityId);
        }
    }
} // namespace CrossEngineEditor
