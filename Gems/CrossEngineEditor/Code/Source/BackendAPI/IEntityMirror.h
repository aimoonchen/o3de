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
    //! Describes a property change originating from the editor property panel.
    struct PropertyChange
    {
        AZStd::string m_path;   //!< Reflected property path on the mirrored component.
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
        virtual void SyncToEditor(AZStd::vector<AZ::Entity*>& outEntities) = 0;

        //! Editor -> engine: a manipulator moved a mirrored entity.
        virtual void OnEditorTransformChanged(AZ::EntityId entityId, const AZ::Transform& worldTm) = 0;

        //! Editor -> engine: a reflected property changed in the inspector.
        virtual void OnEditorPropertyChanged(AZ::EntityId entityId, const PropertyChange& change) = 0;

        //! Editor -> engine: create a new engine object; returns the mirror entity id.
        virtual AZ::EntityId CreateObject(const ObjectSpec& spec) = 0;

        //! Editor -> engine: destroy an engine object by its mirror entity id.
        virtual void DestroyObject(AZ::EntityId entityId) = 0;
    };
} // namespace CrossEngineEditor
