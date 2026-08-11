/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Framework/EngineNodeComponent.h>

#include <AzCore/Serialization/EditContextConstants.inl>
#include <AzCore/Serialization/SerializeContext.h>

namespace CrossEngineEditor
{
    void EngineNodeComponent::Reflect(AZ::ReflectContext* context)
    {
        PropertyBag::Reflect(context);

        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EngineNodeComponent, AzToolsFramework::Components::EditorComponentBase>()
                ->Version(1)
                ->Field("className", &EngineNodeComponent::m_className)
                ->Field("properties", &EngineNodeComponent::m_properties);
            // m_nodeHandle is intentionally NOT reflected: it is a runtime engine handle
            // and must never be serialised (the engine's native scene owns persistence).

            if (AZ::EditContext* editContext = serialize->GetEditContext())
            {
                editContext->Class<EngineNodeComponent>("Engine Node", "A mirrored engine scene node")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::Category, "Engine")
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &EngineNodeComponent::m_className, "Class", "Engine class name")
                    ->Attribute(AZ::Edit::Attributes::ReadOnly, true)
                    // The property set is shown through a dynamic edit-data provider, the
                    // same mechanism ScriptEditorComponent uses: each mirrored property gets
                    // its name / UI handler / enum / read-only supplied at runtime. No custom
                    // PropertyHandler is registered.
                    ->DataElement(nullptr, &EngineNodeComponent::m_properties, "Properties", "Engine node properties")
                    ->SetDynamicEditDataProvider(&EngineNodeComponent::GetPropertyEditData)
                    ->Attribute(AZ::Edit::Attributes::Visibility, AZ::Edit::PropertyVisibility::ShowChildrenOnly);
            }
        }
    }

    EngineNodeComponent::~EngineNodeComponent()
    {
        for (auto& [element, editData] : m_editDataByElement)
        {
            editData.ClearAttributes();
        }
        for (EngineProperty* prop : m_properties.m_items)
        {
            delete prop;
        }
    }

    void EngineNodeComponent::SetMirrorData(AZStd::string className, PropertyBag&& bag)
    {
        m_className = AZStd::move(className);

        // Release any property objects we currently own before taking the new set.
        for (EngineProperty* prop : m_properties.m_items)
        {
            delete prop;
        }
        m_properties = AZStd::move(bag);

        RebuildEditData();
    }

    EngineProperty* EngineNodeComponent::FindProperty(AZStd::string_view name) const
    {
        for (EngineProperty* prop : m_properties.m_items)
        {
            if (prop && prop->m_name == name)
            {
                return prop;
            }
        }
        return nullptr;
    }

    void EngineNodeComponent::RebuildEditData()
    {
        // ElementData owns the attributes it allocates (enum entries), so free them first.
        for (auto& [element, editData] : m_editDataByElement)
        {
            editData.ClearAttributes();
        }
        m_editDataByElement.clear();

        for (EngineProperty* prop : m_properties.m_items)
        {
            if (!prop)
            {
                continue;
            }

            AZ::Edit::ElementData editData;
            // m_name / m_category own their storage and outlive this edit data (rebuilt on
            // every property-set change), so their c_str() is a stable label pointer.
            editData.m_name = prop->m_name.c_str();
            editData.m_description = prop->m_category.c_str();
            editData.m_elementId = prop->GetUIHandler();

            if (prop->m_readOnly)
            {
                editData.m_attributes.push_back(AZ::Edit::AttributePair(
                    AZ::Edit::Attributes::ReadOnly, aznew AZ::Edit::AttributeData<bool>(true)));
            }

            // Integer property with enum names -> ComboBox entries (one EnumValue attribute
            // per name), exactly as ScriptEditorComponent builds its enum attributes.
            if (auto* intProp = azrtti_cast<EnginePropertyInt*>(prop); intProp && !intProp->m_enumNames.empty())
            {
                using EnumPair = AZStd::pair<AZ::s64, AZStd::string>;
                for (AZ::s64 i = 0; i < static_cast<AZ::s64>(intProp->m_enumNames.size()); ++i)
                {
                    editData.m_attributes.push_back(AZ::Edit::AttributePair(
                        AZ::Edit::InternalAttributes::EnumValue,
                        aznew AZ::Edit::AttributeData<EnumPair>(EnumPair(i, intProp->m_enumNames[static_cast<size_t>(i)]))));
                }
            }

            // Key on the concrete value field address: that is the reflected editable leaf
            // the property grid passes to the dynamic provider. ElementData owns its enum /
            // read-only attributes, so it is stored exactly once to avoid a double free.
            m_editDataByElement.emplace(prop->GetValueAddress(), AZStd::move(editData));
        }
    }

    const AZ::Edit::ElementData* EngineNodeComponent::GetPropertyEditData(
        const void* handlerPtr, const void* elementPtr, const AZ::Uuid& /*elementType*/)
    {
        auto* self = const_cast<EngineNodeComponent*>(reinterpret_cast<const EngineNodeComponent*>(handlerPtr));
        auto it = self->m_editDataByElement.find(elementPtr);
        if (it == self->m_editDataByElement.end())
        {
            // A cache miss means the property objects were replaced out from under us - the only
            // path that does this is an Undo/Redo, which restores m_properties by deleting the old
            // EngineProperty objects and cloning new ones at fresh addresses (so both the cache
            // keys and the m_name label pointers are stale). Rebuild once from the live property
            // set and retry; without this the value row would vanish from the Inspector after the
            // first Ctrl+Z, and the stale c_str() labels would be a use-after-free read.
            self->RebuildEditData();
            it = self->m_editDataByElement.find(elementPtr);
        }
        return (it != self->m_editDataByElement.end()) ? &it->second : nullptr;
    }

    void EngineNodeComponent::Init()
    {
        EditorComponentBase::Init();
        // Edit data is built lazily by the dynamic provider (rebuild-on-miss), so no eager
        // build here: the property set may still be replaced by Undo before first display.
    }

    void EngineNodeComponent::Activate()
    {
        EditorComponentBase::Activate();
    }

    void EngineNodeComponent::Deactivate()
    {
        EditorComponentBase::Deactivate();
    }
} // namespace CrossEngineEditor
