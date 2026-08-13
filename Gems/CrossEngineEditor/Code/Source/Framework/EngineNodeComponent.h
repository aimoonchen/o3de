/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! The single self-authored mirror component (final plan section 3.1).
//!
//! One engine node is mirrored as one AZ::Entity carrying exactly two components:
//!   * the standard O3DE TransformComponent (Outliner parent lookup / Gizmo / Undo), and
//!   * this EngineNodeComponent, which is engine-agnostic and shared by every backend.
//!
//! It holds:
//!   * m_className   - engine class name, shown read-only in the Inspector,
//!   * m_properties  - the typed PropertyBag mirrored from the engine's reflection, shown
//!                     and edited in the Inspector via a dynamic edit-data provider (the
//!                     proven ScriptEditorComponent pattern: no custom PropertyHandler),
//!   * m_nodeHandle  - an opaque handle to the engine-side node (rbfx Node* / Godot
//!                     ObjectID). Reflected (Version 2) so it survives the editor's in-memory
//!                     prefab clone, but never persisted to disk (the engine's native scene is
//!                     the source of truth and the mirror prefab is not saved).
//!
//! Engine classes are deliberately NOT registered one-by-one into AZ reflection; the
//! whole variable, per-instance property set is mirrored into m_properties instead.

#include <Framework/EngineProperty.h>

#include <AzCore/Math/Aabb.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzFramework/Visibility/BoundsBus.h>
#include <AzToolsFramework/API/ComponentEntitySelectionBus.h>
#include <AzToolsFramework/ToolsComponents/EditorComponentBase.h>

namespace CrossEngineEditor
{
    class EngineNodeComponent final
        : public AzToolsFramework::Components::EditorComponentBase
        , public AzToolsFramework::EditorComponentSelectionRequestsBus::Handler
        , public AzFramework::BoundsRequestBus::Handler
        , public AZ::TransformNotificationBus::Handler
    {
    public:
        AZ_EDITOR_COMPONENT(EngineNodeComponent, "{4B8E2D1F-6A03-4C97-9E5B-1F2A7C4D8E60}");

        static void Reflect(AZ::ReflectContext* context);

        EngineNodeComponent() = default;
        ~EngineNodeComponent() override;

        //! Replace the mirrored class name + property set (called by the backend on sync).
        //! Takes ownership of the property objects in bag.
        void SetMirrorData(AZStd::string className, PropertyBag&& bag);

        [[nodiscard]] const AZStd::string& GetClassName() const { return m_className; }
        [[nodiscard]] PropertyBag& GetProperties() { return m_properties; }
        [[nodiscard]] const PropertyBag& GetProperties() const { return m_properties; }

        //! Opaque engine-side node handle (rbfx Node* / Godot ObjectID). Reflected in-memory only.
        void SetNodeHandle(AZ::u64 handle)
        {
            m_nodeHandle = handle;
            m_boundsCacheValid = false; // handle may point at different geometry; drop cached bounds.
        }
        [[nodiscard]] AZ::u64 GetNodeHandle() const { return m_nodeHandle; }

        //! Find a mirrored property by its engine name, or nullptr. Used by the backend to
        //! read a grid-edited value back before pushing it to the engine.
        [[nodiscard]] EngineProperty* FindProperty(AZStd::string_view name) const;

    private:
        // AZ::Component...
        void Init() override;
        void Activate() override;
        void Deactivate() override;

        // AzToolsFramework::EditorComponentSelectionRequestsBus::Handler...
        //! World-space AABB for viewport picking + selection outline (plan pick §5.1). Sourced
        //! from the raw backend visual bounds; a non-visual node returns null (not ray-pickable)
        //! and stays selectable via its editor icon / the Outliner.
        AZ::Aabb GetEditorSelectionBoundsViewport(const AzFramework::ViewportInfo& viewportInfo) override;
        //! We answer the ray test ourselves (AABB level), matching both engines' native editor
        //! selection precision, so O3DE runs the precise pass instead of the AABB-only fallback.
        bool SupportsEditorRayIntersectViewport(const AzFramework::ViewportInfo& viewportInfo) override;
        bool EditorSelectionIntersectRayViewport(
            const AzFramework::ViewportInfo& viewportInfo,
            const AZ::Vector3& rayOrigin,
            const AZ::Vector3& rayDirection,
            float& distance) override;

        // AzFramework::BoundsRequestBus::Handler...
        //! World / local bounds. Implementing this bus registers the entity in the visibility
        //! system (EntityVisibilityBoundsUnionSystem) octree. This is NOT required for picking -
        //! the viewport picker iterates FindVisibleEntities, not that octree, and no code reads the
        //! tree today. Provided for O3DE parity (selectable entities expose bounds) and to seed
        //! future box-select / frustum-cull. Shares the same world-bounds source as picking.
        AZ::Aabb GetWorldBounds() const override;
        AZ::Aabb GetLocalBounds() const override;

        //! Dynamic edit-data provider (AzCore reflection signature, same as
        //! ScriptEditorComponent). Returns per-element ElementData keyed by the address of
        //! the EngineProperty being displayed - this is what gives each mirrored property
        //! its name / UI handler / enum values / read-only flag at runtime.
        static const AZ::Edit::ElementData* GetPropertyEditData(
            const void* handlerPtr, const void* elementPtr, const AZ::Uuid& elementType);

        //! Build (or rebuild) m_editDataByElement from the current m_properties so the
        //! provider can answer lookups. Called whenever the property set changes.
        void RebuildEditData();

        //! Raw engine-side visual bounds for this node (null AABB if the node has no renderable
        //! geometry). Selection/pick uses this directly (null = not ray-pickable); the visibility
        //! bus wraps it with a pivot-box fallback so non-visual nodes still register + show an icon.
        AZ::Aabb GetBackendVisualBounds() const;

        // AZ::TransformNotificationBus::Handler...
        //! Invalidate the cached world bounds when THIS entity's transform changes (gizmo drag,
        //! parent move, engine sync). Camera orbit does NOT fire this, so while a static object is
        //! selected the cache stays valid and the per-frame selection-outline / pick queries no
        //! longer cross the GDExtension boundary to recompute an unchanged box (PROGRESS.md §13
        //! fix 3). This is a transform-invalidated cache, not the always-live component AABB the
        //! plan's D1 rejected: it holds nothing stale because the only thing that changes a world
        //! AABB (short of geometry edits, which re-sync) is the transform we listen to here.
        void OnTransformChanged(const AZ::Transform& local, const AZ::Transform& world) override;

        AZStd::string m_className;
        PropertyBag m_properties;
        AZ::u64 m_nodeHandle = 0;

        //! Transform-invalidated cache of GetBackendVisualBounds(). mutable: filled from the const
        //! bounds accessors. m_boundsCacheValid=false forces the next query to recompute.
        mutable AZ::Aabb m_cachedVisualBounds = AZ::Aabb::CreateNull();
        mutable bool m_boundsCacheValid = false;

        //! Runtime edit-data, one entry per EngineProperty, keyed by its address. Rebuilt
        //! on every property-set change; owns the attributes it allocates.
        AZStd::unordered_map<const void*, AZ::Edit::ElementData> m_editDataByElement;
    };
} // namespace CrossEngineEditor
