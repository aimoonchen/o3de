/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Typed property container for the object mirror (final plan section 3.2).
//!
//! An engine node's reflected properties are mirrored into a PropertyBag of concrete,
//! typed EngineProperty objects. Each subclass holds ONE primitive that O3DE's property
//! grid already has a stock editor for, so the Inspector renders and undoes it for free
//! (no custom PropertyHandler, no string encoding).
//!
//! This follows the proven O3DE ScriptComponent/ScriptProperty pattern: a container of
//! polymorphic property objects, each exposing a concrete typed field the grid edits
//! natively, with a dynamic edit-data provider (on EngineNodeComponent) supplying the
//! per-element name / UI handler / enum values at runtime. O3DE's property grid cannot
//! natively edit an AZStd::variant member, which is why the value is a concrete field on
//! a subclass rather than a variant.
//!
//! The backend fills the bag from the engine's own reflection (rbfx GetAttributes /
//! Godot get_property_list) and writes edited values back (rbfx SetAttribute / Godot set).

#include <AzCore/Math/Color.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Memory/SystemAllocator.h>
#include <AzCore/RTTI/RTTI.h>
#include <AzCore/Serialization/EditContextConstants.inl>
#include <AzCore/base.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>

namespace AZ
{
    class ReflectContext;
}

namespace CrossEngineEditor
{
    //! Abstract base for one mirrored engine property. Shared metadata lives here; the
    //! concrete value lives on the typed subclass so the property grid edits it natively.
    class EngineProperty
    {
    public:
        AZ_RTTI(EngineProperty, "{2E9C7A10-8D4B-4F0A-9C21-6B3E5F8A1C40}");
        AZ_CLASS_ALLOCATOR(EngineProperty, AZ::SystemAllocator);
        virtual ~EngineProperty() = default;

        static void Reflect(AZ::ReflectContext* context);

        //! Stock property-grid UI handler for this property's concrete type
        //! (AZ::Edit::UIHandlers::CheckBox / SpinBox / LineEdit / Vector3 / Color / ComboBox).
        [[nodiscard]] virtual AZ::Crc32 GetUIHandler() const = 0;

        //! Address of the concrete value member, used to key the dynamic edit data.
        [[nodiscard]] virtual const void* GetValueAddress() const = 0;

        AZStd::string m_name;      //!< Engine property name, shown verbatim.
        AZStd::string m_category;  //!< Group (engine component/class name).
        bool m_readOnly = false;
    };

    class EnginePropertyBool final : public EngineProperty
    {
    public:
        AZ_RTTI(EnginePropertyBool, "{A1B2C3D4-0001-4E5F-9A0B-1C2D3E4F5061}", EngineProperty);
        AZ_CLASS_ALLOCATOR(EnginePropertyBool, AZ::SystemAllocator);
        static void Reflect(AZ::ReflectContext* context);
        AZ::Crc32 GetUIHandler() const override { return AZ::Edit::UIHandlers::CheckBox; }
        const void* GetValueAddress() const override { return &m_value; }
        bool m_value = false;
    };

    //! Integer property. When m_enumNames is non-empty it is rendered as a ComboBox whose
    //! selected index is m_value (matches rbfx enum attributes / Godot ENUM hints).
    class EnginePropertyInt final : public EngineProperty
    {
    public:
        AZ_RTTI(EnginePropertyInt, "{A1B2C3D4-0002-4E5F-9A0B-1C2D3E4F5062}", EngineProperty);
        AZ_CLASS_ALLOCATOR(EnginePropertyInt, AZ::SystemAllocator);
        static void Reflect(AZ::ReflectContext* context);
        AZ::Crc32 GetUIHandler() const override
        {
            return m_enumNames.empty() ? AZ::Edit::UIHandlers::SpinBox : AZ::Edit::UIHandlers::ComboBox;
        }
        const void* GetValueAddress() const override { return &m_value; }
        AZ::s64 m_value = 0;
        AZStd::vector<AZStd::string> m_enumNames; //!< Non-empty => ComboBox of these names.
    };

    class EnginePropertyDouble final : public EngineProperty
    {
    public:
        AZ_RTTI(EnginePropertyDouble, "{A1B2C3D4-0003-4E5F-9A0B-1C2D3E4F5063}", EngineProperty);
        AZ_CLASS_ALLOCATOR(EnginePropertyDouble, AZ::SystemAllocator);
        static void Reflect(AZ::ReflectContext* context);
        AZ::Crc32 GetUIHandler() const override { return AZ::Edit::UIHandlers::SpinBox; }
        const void* GetValueAddress() const override { return &m_value; }
        double m_value = 0.0;
    };

    class EnginePropertyString final : public EngineProperty
    {
    public:
        AZ_RTTI(EnginePropertyString, "{A1B2C3D4-0004-4E5F-9A0B-1C2D3E4F5064}", EngineProperty);
        AZ_CLASS_ALLOCATOR(EnginePropertyString, AZ::SystemAllocator);
        static void Reflect(AZ::ReflectContext* context);
        AZ::Crc32 GetUIHandler() const override { return AZ::Edit::UIHandlers::LineEdit; }
        const void* GetValueAddress() const override { return &m_value; }
        AZStd::string m_value;
    };

    class EnginePropertyVector3 final : public EngineProperty
    {
    public:
        AZ_RTTI(EnginePropertyVector3, "{A1B2C3D4-0005-4E5F-9A0B-1C2D3E4F5065}", EngineProperty);
        AZ_CLASS_ALLOCATOR(EnginePropertyVector3, AZ::SystemAllocator);
        static void Reflect(AZ::ReflectContext* context);
        AZ::Crc32 GetUIHandler() const override { return AZ::Edit::UIHandlers::Vector3; }
        const void* GetValueAddress() const override { return &m_value; }
        AZ::Vector3 m_value = AZ::Vector3::CreateZero();
    };

    class EnginePropertyColor final : public EngineProperty
    {
    public:
        AZ_RTTI(EnginePropertyColor, "{A1B2C3D4-0006-4E5F-9A0B-1C2D3E4F5066}", EngineProperty);
        AZ_CLASS_ALLOCATOR(EnginePropertyColor, AZ::SystemAllocator);
        static void Reflect(AZ::ReflectContext* context);
        AZ::Crc32 GetUIHandler() const override { return AZ::Edit::UIHandlers::Color; }
        const void* GetValueAddress() const override { return &m_value; }
        AZ::Color m_value = AZ::Color::CreateOne();
    };

    //! The full set of mirrored properties for one engine node (one EngineNodeComponent).
    //! Owns the polymorphic property objects. Serialized as a vector of base pointers, the
    //! same shape O3DE's ScriptComponent uses for its runtime property set.
    //!
    //! Move-only: the owning EngineNodeComponent deletes the pointees, so an accidental copy
    //! would alias the raw pointers into two bags and double-free (O3DE's ScriptPropertyGroup
    //! deletes its copy constructor for the same reason).
    struct PropertyBag
    {
        AZ_TYPE_INFO(PropertyBag, "{7F3A9B21-4C6D-4E8A-8B15-2D9C0E7F3A11}");
        static void Reflect(AZ::ReflectContext* context);

        PropertyBag() = default;
        PropertyBag(const PropertyBag&) = delete;
        PropertyBag& operator=(const PropertyBag&) = delete;
        PropertyBag(PropertyBag&&) = default;
        PropertyBag& operator=(PropertyBag&&) = default;

        AZStd::vector<EngineProperty*> m_items;
    };
} // namespace CrossEngineEditor
