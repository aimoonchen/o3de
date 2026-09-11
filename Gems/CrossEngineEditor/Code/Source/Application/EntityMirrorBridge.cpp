/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Application/EntityMirrorBridge.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>
#include <Framework/EngineNodeComponent.h>

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Math/Transform.h>

#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzToolsFramework/ToolsComponents/TransformComponent.h>

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
        // Engine -> editor (Plan §B4). The backend hands us fully-built mirror
        // entities (each already carrying an EngineNodeComponent and, for a real backend,
        // its parent EntityId on the TransformComponent so the Outliner tree forms). Here we
        // run the standard editor-entity intake so the Outliner / Inspector pick them up.
        auto* backend = GetBackend();
        if (!backend)
        {
            return;
        }

        // Guard the whole sync: intake + FinishSync fire the property bus per entity, and
        // echoing those writes back to the engine is pure feedback (plus SetDirty pollution).
        m_syncing = true;
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
        m_syncing = false;
    }

    void EntityMirrorBridge::RefreshFromEngine()
    {
        // Drop every mirror entity currently in the editor context. Mirror entities are
        // root-instance overlay entities (AddEditorEntity in SyncFromEngine routes them into
        // the focused root prefab instance; GetLooseEditorEntities now means "direct
        // children of the root instance"), each carrying an EngineNodeComponent;
        // DestroyEditorEntity is the
        // exact inverse of that intake. Then re-pull the whole engine scene, because
        // SyncToEditor always builds fresh entities instead of reusing the existing ones.
        using AzToolsFramework::EditorEntityContextRequestBus;
        AzToolsFramework::EntityList loose;
        EditorEntityContextRequestBus::Broadcast(
            &EditorEntityContextRequestBus::Events::GetLooseEditorEntities, loose);
        for (const AZ::Entity* entity : loose)
        {
            if (entity && entity->FindComponent<EngineNodeComponent>())
            {
                EditorEntityContextRequestBus::Broadcast(
                    &EditorEntityContextRequestBus::Events::DestroyEditorEntity, entity->GetId());
            }
        }

        SyncFromEngine();
    }

    AZ::EntityId EntityMirrorBridge::FirstSelectedMirrorId() const
    {
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
                return entityId;
            }
        }
        return AZ::EntityId();
    }

    bool EntityMirrorBridge::AssignAssetToSelection(const AZStd::string& assetPath)
    {
        // Extension -> assignment action (migration L1, §B9): the rbfx editor's
        // drop-onto-selection flow for materials and animations. Spawnable scene assets
        // (.mdl/.xml) need a world position, so the callers place those themselves.
        const AZStd::string extension = AZ::IO::PathView(assetPath).Extension().Native();

        auto* backend = GetBackend();
        const AZ::EntityId target = FirstSelectedMirrorId();
        if (!backend || !target.IsValid())
        {
            return false;
        }

        // Engine-extension assignment commands go through the backend's InvokeCustom escape
        // hatch (E2 slimming); backends without the capability report "unsupported".
        AZStd::vector<AZStd::string> args;
        args.reserve(3);
        args.push_back(target.ToString());
        args.push_back(assetPath);
        AZStd::vector<AZ::u8> payload;
        if (azstricmp(extension.c_str(), ".mat") == 0
            || azstricmp(extension.c_str(), ".material") == 0)
        {
            args.push_back("0");
            return backend->InvokeCustom("AssignMaterial", args, payload);
        }
        if (azstricmp(extension.c_str(), ".ani") == 0)
        {
            return backend->InvokeCustom("AssignAnimation", args, payload);
        }
        return false;
    }

    void EntityMirrorBridge::SpawnAssetAtOrigin(const AZStd::string& assetPath)
    {
        auto* backend = GetBackend();
        if (!backend)
        {
            return;
        }

        ObjectSpec spec;
        spec.m_assetPath = assetPath;
        spec.m_transform = AZ::Transform::CreateIdentity();
        backend->GetEntityMirror().CreateObject(spec);

        // The mirror entity is built on the next full re-sync (rbfx CreateObject deliberately
        // returns an invalid id - see RbfxBackend::CreateObject).
        RefreshFromEngine();
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

        // A real editor->engine transform write (gizmo drag / Outliner reparent) means the
        // scene changed. Sync-driven broadcasts are not distinguished here (the engine write
        // they echo is idempotent), but they must NOT mark the scene dirty.
        if (!m_syncing && m_dirtyCallback)
        {
            m_dirtyCallback();
        }
    }

    void EntityMirrorBridge::OnEntityComponentPropertyChanged(AZ::ComponentId componentId)
    {
        if (m_syncing)
        {
            return;
        }

        auto* backend = GetBackend();
        if (!backend)
        {
            return;
        }

        const AZ::EntityId entityId = *AzToolsFramework::PropertyEditorEntityChangeNotificationBus::GetCurrentBusId();

        // TransformComponent fires this bus once per gizmo-drag frame (OnTransformChanged
        // broadcasts without delta filtering). Re-pushing the whole PropertyBag per frame is
        // wasted echo - route transform changes through the transform-only path instead.
        AZ::Entity* entity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(entity, &AZ::ComponentApplicationBus::Events::FindEntity, entityId);
        const AZ::Component* component = entity ? entity->FindComponent(componentId) : nullptr;
        if (component
            && component->RTTI_GetType() == azrtti_typeid<AzToolsFramework::Components::TransformComponent>())
        {
            OnEntityTransformChanged({ entityId });
            return;
        }

        backend->GetEntityMirror().OnEditorPropertyChanged(entityId);

        if (m_dirtyCallback)
        {
            m_dirtyCallback();
        }
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
