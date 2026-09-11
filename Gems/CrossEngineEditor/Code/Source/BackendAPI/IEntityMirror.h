/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! C6: object-model mirror between the engine's native objects and AZ::Entity.
//!
//! Architectural premise (Plan §B4): the editor's canonical object model is
//! AZ::Entity + reflection. A backend mirrors its engine objects into AZ::Entity so
//! that Outliner / Inspector / Prefab / Undo all work unchanged. The mirror is
//! bidirectional: editor edits are forwarded back to the engine object.

#include <Framework/EngineProperty.h>

#include <AzCore/base.h>
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
    //! Describes an object the editor asks the backend to create.
    struct ObjectSpec
    {
        AZStd::string m_typeId;    //!< Engine-specific object/type identifier (empty = plain node).
        AZStd::string m_category;  //!< Optional engine object category (rbfx ObjectReflection category).
        AZStd::string m_assetPath; //!< Optional source asset (model/prefab) the new object loads.
        AZ::Transform m_transform = AZ::Transform::CreateIdentity();
    };

    //! One creatable object type, enumerated for the editor's Create menu
    //! (Migration batch 1 addition, rbfx_migration.md §3.1 Create-menu data source).
    struct ObjectTypeInfo
    {
        AZStd::string m_category;    //!< Menu group (engine object category).
        AZStd::string m_typeId;      //!< Value passed back as ObjectSpec::m_typeId.
        AZStd::string m_displayName; //!< Shown in the menu.
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

        //! Engine -> editor: list the creatable object types for the editor's Create menu
        //! (Migration batch 1 addition, rbfx_migration.md §3.1 Create-menu data source).
        virtual void EnumerateObjectTypes(AZStd::vector<ObjectTypeInfo>& out) = 0;

        //! Editor -> engine: a manipulator moved a mirrored entity.
        virtual void OnEditorTransformChanged(AZ::EntityId entityId, const AZ::Transform& worldTm) = 0;

        //! Editor -> engine: a reflected property changed in the inspector. The change bus only
        //! identifies the component, not the individual value, so the backend re-reads the
        //! EngineNodeComponent's PropertyBag and pushes it to the engine (rbfx SetAttribute /
        //! Godot set). Transform changes do NOT use this; they go through OnEditorTransformChanged.
        virtual void OnEditorPropertyChanged(AZ::EntityId entityId) = 0;

        //! Editor -> engine: create a new engine object; returns the mirror entity id.
        virtual AZ::EntityId CreateObject(const ObjectSpec& spec) = 0;

        //! Editor -> engine: destroy an engine object by its mirror entity id.
        virtual void DestroyObject(AZ::EntityId entityId) = 0;

        //! Editor -> engine: persist the engine's native scene (Plan §B4: the engine's native
        //! scene is the single source of truth, not an O3DE prefab). An empty path means "save to
        //! the scene's current file". Returns false if the backend can't save (e.g. no scene open
        //! or the format isn't supported). Default: unsupported (returns false).
        virtual bool SaveScene(const AZStd::string& path) { (void)path; return false; }

        // ----- Migration batch 1 (rbfx_migration.md §3.1) ----------------------------
        // CreatePrefabFromNodes / AssignMaterial / AssignAnimation / SerializeNodes /
        // PasteNodes were slimmed off this surface (E2, filament_migration.md §7.1): they are
        // engine-extension operations, not shared editing value, so backends expose them (when
        // supported) through IEngineBackend::InvokeCustom instead of pure virtuals.

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
        //! This is the picking "seam" (Plan §B5b). It fixes the rotated-mesh problem where a
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

        //! Editor -> engine: scene-wide precise raycast for asset-drop ground placement
        //! (Migration batch 1 addition - the D&D spawn path needs a scene ray, not a per-node one).
        //! Same O3DE-space ray convention as RaycastNode. Fills outHitPoint / outHitNormal
        //! (world, O3DE convention); returns true on a hit, false on miss or when unsupported.
        //! Backends must skip editor-owned drawables (debug/grid overlay) so the drop lands on
        //! engine geometry, not on the editor's grid gizmo.
        virtual bool RaycastScene(
            const AZ::Vector3& rayOrigin,
            const AZ::Vector3& rayDirection,
            AZ::Vector3& outHitPoint,
            AZ::Vector3& outHitNormal) const = 0;

    };
} // namespace CrossEngineEditor
