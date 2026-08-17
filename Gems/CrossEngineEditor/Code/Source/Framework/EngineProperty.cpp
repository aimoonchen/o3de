/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Framework/EngineProperty.h>

#include <AzCore/Serialization/SerializeContext.h>

namespace CrossEngineEditor
{
    void EngineProperty::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            // Base holds the shared metadata. It is abstract, so no factory - only the
            // shared fields are declared here and inherited by the concrete subclasses.
            serialize->Class<EngineProperty>()
                ->Version(1)
                ->Field("name", &EngineProperty::m_name)
                ->Field("category", &EngineProperty::m_category)
                ->Field("readOnly", &EngineProperty::m_readOnly);
        }

        EnginePropertyBool::Reflect(context);
        EnginePropertyInt::Reflect(context);
        EnginePropertyDouble::Reflect(context);
        EnginePropertyString::Reflect(context);
        EnginePropertyVector3::Reflect(context);
        EnginePropertyColor::Reflect(context);
        EnginePropertyQuaternion::Reflect(context);
        EnginePropertyResourceRef::Reflect(context);
        EnginePropertyResourceRefList::Reflect(context);
        EnginePropertyVariant::Reflect(context);
    }

    void EnginePropertyBool::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyBool, EngineProperty>()->Version(1)->Field("value", &EnginePropertyBool::m_value);
        }
    }

    void EnginePropertyInt::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyInt, EngineProperty>()
                ->Version(1)
                ->Field("value", &EnginePropertyInt::m_value)
                ->Field("enumNames", &EnginePropertyInt::m_enumNames);
        }
    }

    void EnginePropertyDouble::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyDouble, EngineProperty>()->Version(1)->Field(
                "value", &EnginePropertyDouble::m_value);
        }
    }

    void EnginePropertyString::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyString, EngineProperty>()->Version(1)->Field(
                "value", &EnginePropertyString::m_value);
        }
    }

    void EnginePropertyVector3::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyVector3, EngineProperty>()->Version(1)->Field(
                "value", &EnginePropertyVector3::m_value);
        }
    }

    void EnginePropertyColor::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyColor, EngineProperty>()->Version(1)->Field(
                "value", &EnginePropertyColor::m_value);
        }
    }

    void EnginePropertyQuaternion::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyQuaternion, EngineProperty>()->Version(1)->Field(
                "value", &EnginePropertyQuaternion::m_value);
        }
    }

    void EnginePropertyResourceRef::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyResourceRef, EngineProperty>()
                ->Version(1)
                ->Field("value", &EnginePropertyResourceRef::m_value)
                ->Field("refType", &EnginePropertyResourceRef::m_refType);
        }
    }

    void EnginePropertyResourceRefList::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyResourceRefList, EngineProperty>()
                ->Version(1)
                ->Field("value", &EnginePropertyResourceRefList::m_value)
                ->Field("refType", &EnginePropertyResourceRefList::m_refType);
        }
    }

    void EnginePropertyVariant::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<EnginePropertyVariant, EngineProperty>()->Version(1)->Field(
                "value", &EnginePropertyVariant::m_value);
        }
    }

    void PropertyBag::Reflect(AZ::ReflectContext* context)
    {
        // The property objects themselves must be reflected before the bag that owns them.
        EngineProperty::Reflect(context);

        if (auto* serialize = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serialize->Class<PropertyBag>()->Version(1)->Field("items", &PropertyBag::m_items);
        }
    }
} // namespace CrossEngineEditor
