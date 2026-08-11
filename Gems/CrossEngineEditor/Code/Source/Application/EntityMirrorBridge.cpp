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

#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

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
        // Engine -> editor (plan section 3.5). The backend hands us fully-built mirror
        // entities (each already carrying an EngineNodeComponent and, for a real backend,
        // its parent EntityId on the TransformComponent so the Outliner tree forms). Here we
        // run the standard editor-entity intake so the Outliner / Inspector pick them up.
        auto* backend = GetBackend();
        if (!backend)
        {
            return;
        }

        AZStd::vector<AZ::Entity*> mirrored;
        backend->GetEntityMirror().SyncToEditor(mirrored);

        using AzToolsFramework::EditorEntityContextRequestBus;
        for (AZ::Entity* entity : mirrored)
        {
            if (!entity)
            {
                continue;
            }

            // Add the 9 required editor components (TransformComponent + editor bookkeeping)
            // if the backend has not already, then bring the entity into the editor context so
            // the Outliner shows it. Sequence verified against EditorEntityContextComponent.
            EditorEntityContextRequestBus::Broadcast(
                &EditorEntityContextRequestBus::Events::AddRequiredComponents, *entity);

            if (entity->GetState() == AZ::Entity::State::Constructed)
            {
                entity->Init();
            }

            EditorEntityContextRequestBus::Broadcast(
                &EditorEntityContextRequestBus::Events::AddEditorEntity, entity);
            EditorEntityContextRequestBus::Broadcast(
                &EditorEntityContextRequestBus::Events::FinalizeEditorEntity, entity);
        }

        // All mirror entities are now added and activated; let the backend wire the
        // parent-child links (TransformBus::SetParent) so the Outliner tree forms.
        backend->GetEntityMirror().FinishSync();
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
