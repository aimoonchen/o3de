/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! C6: object-model mirror between the engine's native objects and AZ::Entity.
//!
//! Architectural premise (plan §3.2): the editor's canonical object model is
//! AZ::Entity + reflection. A backend mirrors its engine objects into AZ::Entity so
//! that Outliner / Inspector / Prefab / Undo all work unchanged. The mirror is
//! bidirectional: editor edits are forwarded back to the engine object.

#include <AzCore/Component/EntityId.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>

namespace AZ
{
    class Entity;
}

namespace CrossEngineEditor
{
    //! Describes a property change originating from the editor property panel (plan 3.4).
    //!
    //! Path grammar:
    //!   ""                    the entity's mirrored property set changed; the backend
    //!                         re-reads the EngineNodeComponent's PropertyBag and pushes the
    //!                         values to the engine (rbfx SetAttribute / Godot set). This is
    //!                         what the standard property-change bus can report, since it only
    //!                         identifies the component, not the individual value.
    //!   "props/<name>"        a single named engine property changed (used when the caller
    //!                         does know the specific property, e.g. a targeted edit).
    //! Transform changes do NOT use this; they go through OnEditorTransformChanged.
    struct PropertyChange
    {
        AZStd::string m_path;   //!< See grammar above. Empty = re-push the whole bag.
    };

    //! Describes an object the editor asks the backend to create.
    struct ObjectSpec
    {
        AZStd::string m_typeId; //!< Engine-specific object/type identifier.
        AZ::Transform m_transform = AZ::Transform::CreateIdentity();
    };

    class IEntityMirror
    {
    public:
        virtual ~IEntityMirror() = default;

        //! Engine -> editor: populate the editor entity context with mirror entities.
        //! Entities are returned parent-before-child (pre-order) and are NOT yet parented;
        //! the shell adds/activates them, then calls FinishSync so the backend can wire the
        //! parent-child links via TransformBus (which requires activated entities).
        virtual void SyncToEditor(AZStd::vector<AZ::Entity*>& outEntities) = 0;

        //! Engine -> editor: called by the shell after the entities from SyncToEditor have
        //! been added and activated. The backend establishes parent-child relationships here
        //! (TransformBus::SetParent) so the Outliner tree matches the engine scene graph.
        virtual void FinishSync() {}

        //! Editor -> engine: a manipulator moved a mirrored entity.
        virtual void OnEditorTransformChanged(AZ::EntityId entityId, const AZ::Transform& worldTm) = 0;

        //! Editor -> engine: a reflected property changed in the inspector.
        virtual void OnEditorPropertyChanged(AZ::EntityId entityId, const PropertyChange& change) = 0;

        //! Editor -> engine: create a new engine object; returns the mirror entity id.
        virtual AZ::EntityId CreateObject(const ObjectSpec& spec) = 0;

        //! Editor -> engine: destroy an engine object by its mirror entity id.
        virtual void DestroyObject(AZ::EntityId entityId) = 0;

        //! Editor -> engine: persist the engine's native scene (plan §3.7: the engine's native
        //! scene is the single source of truth, not an O3DE prefab). An empty path means "save to
        //! the scene's current file". Returns false if the backend can't save (e.g. no scene open
        //! or the format isn't supported). Default: unsupported (returns false).
        virtual bool SaveScene(const AZStd::string& path) { (void)path; return false; }
    };
} // namespace CrossEngineEditor
