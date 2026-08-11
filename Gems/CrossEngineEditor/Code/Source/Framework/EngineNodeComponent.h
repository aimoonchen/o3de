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
//!                     ObjectID). NOT reflected: it is a runtime handle and must not be
//!                     serialised (the engine's native scene is the source of truth).
//!
//! Engine classes are deliberately NOT registered one-by-one into AZ reflection; the
//! whole variable, per-instance property set is mirrored into m_properties instead.

#include <Framework/EngineProperty.h>

#include <AzCore/Serialization/EditContext.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzToolsFramework/ToolsComponents/EditorComponentBase.h>

namespace CrossEngineEditor
{
    class EngineNodeComponent final : public AzToolsFramework::Components::EditorComponentBase
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

        //! Opaque engine-side node handle (rbfx Node* / Godot ObjectID). Not reflected.
        void SetNodeHandle(AZ::u64 handle) { m_nodeHandle = handle; }
        [[nodiscard]] AZ::u64 GetNodeHandle() const { return m_nodeHandle; }

        //! Find a mirrored property by its engine name, or nullptr. Used by the backend to
        //! read a grid-edited value back before pushing it to the engine.
        [[nodiscard]] EngineProperty* FindProperty(AZStd::string_view name) const;

    private:
        // AZ::Component...
        void Init() override;
        void Activate() override;
        void Deactivate() override;

        //! Dynamic edit-data provider (AzCore reflection signature, same as
        //! ScriptEditorComponent). Returns per-element ElementData keyed by the address of
        //! the EngineProperty being displayed - this is what gives each mirrored property
        //! its name / UI handler / enum values / read-only flag at runtime.
        static const AZ::Edit::ElementData* GetPropertyEditData(
            const void* handlerPtr, const void* elementPtr, const AZ::Uuid& elementType);

        //! Build (or rebuild) m_editDataByElement from the current m_properties so the
        //! provider can answer lookups. Called whenever the property set changes.
        void RebuildEditData();

        AZStd::string m_className;
        PropertyBag m_properties;
        AZ::u64 m_nodeHandle = 0;

        //! Runtime edit-data, one entry per EngineProperty, keyed by its address. Rebuilt
        //! on every property-set change; owns the attributes it allocates.
        AZStd::unordered_map<const void*, AZ::Edit::ElementData> m_editDataByElement;
    };
} // namespace CrossEngineEditor
