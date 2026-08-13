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
#include <AzCore/Math/Aabb.h>
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
        //! Editor -> engine: world-space selection bounds for a mirror entity, in the O3DE
        //! convention (Z-up RH metres). This is the sole engine-side input to O3DE's picking:
        //! EngineNodeComponent answers EditorComponentSelectionRequestsBus by calling this, and
        //! the viewport draws the selection outline from the same source. Backends return the
        //! world bounds of THIS NODE ONLY - not its sub-tree (rbfx Drawable::GetWorldBoundingBox
        //! with SelfDerived / Godot GeometryInstance3D::get_aabb x global transform on the node
        //! itself). Every engine node is mirrored as its own selectable entity, so unioning the
        //! sub-tree would make a parent swallow its children into one giant box that always wins
        //! the ray test and makes small objects unpickable (matches each engine's per-instance
        //! gizmo). A non-visual node returns null (not ray-pickable); EngineNodeComponent's
        //! visibility-bus path is what wraps null with a pivot-box fallback, not this contract.
        //! Default: null (unsupported).
        virtual AZ::Aabb GetWorldBounds(AZ::EntityId entityId) const { (void)entityId; return AZ::Aabb::CreateNull(); }

        //! Editor -> engine: precise (triangle-level) ray/geometry intersection against THIS NODE's
        //! own renderable geometry. Ray is given in the O3DE convention (Z-up RH metres); the backend
        //! converts it to engine space at its edge.
        //!
        //! Three-state result, so the caller can tell "no precise path" from "precise miss":
        //!   * returns false               -> backend has no precise test; caller keeps its coarse
        //!                                    AABB decision.
        //!   * returns true,  outHit true  -> triangle hit at outDistance (authoritative).
        //!   * returns true,  outHit false -> precise MISS; caller must reject the inflated-AABB hit.
        //!
        //! This is the picking "seam" (pick_final_plan §8). It fixes the rotated-mesh problem where a
        //! large model's world-axis-aligned AABB inflates to cover empty space and steals clicks from
        //! objects behind it (e.g. rbfx "Geometry 100", a scale-100 rotated teapot). rbfx uses
        //! Drawable::ProcessRayQuery with RAY_TRIANGLE on the node's own drawables.
        //!
        //! Default: not supported (returns false).
        virtual bool RaycastNode(
            AZ::EntityId entityId,
            const AZ::Vector3& rayOrigin,
            const AZ::Vector3& rayDirection,
            bool& outHit,
            float& outDistance) const
        {
            (void)entityId;
            (void)rayOrigin;
            (void)rayDirection;
            (void)outHit;
            (void)outDistance;
            return false;
        }
    };
} // namespace CrossEngineEditor
