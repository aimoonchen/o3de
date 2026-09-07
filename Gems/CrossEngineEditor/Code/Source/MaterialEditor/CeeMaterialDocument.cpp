/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <MaterialEditor/CeeMaterialDocument.h>

// Vendor: AtomToolsFramework Core types for material editor stack
#include "MaterialEditor/Vendor/AtomToolsFramework/Document/AtomToolsDocumentNotificationBus.h"
#include "MaterialEditor/Vendor/AtomToolsFramework/DynamicProperty/DynamicProperty.h"
#include "MaterialEditor/Vendor/AtomToolsFramework/DynamicProperty/DynamicPropertyGroup.h"

#include <AzCore/Interface/Interface.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/Serialization/Json/JsonSerialization.h>

#include <BackendAPI/IEngineBackend.h>

#include <AzToolsFramework/UI/PropertyEditor/PropertyEditorAPI.h>

namespace CrossEngineEditor
{
    AtomToolsFramework::DocumentTypeInfo CeeMaterialDocument::BuildDocumentTypeInfo()
    {
        AtomToolsFramework::DocumentTypeInfo info;
        info.m_documentTypeName = "Cross-Engine Material";

        // Factory callback: document system calls this to instantiate CeeMaterialDocument
        // (upstream pattern: MaterialDocument.cpp:147-148).
        info.m_documentFactoryCallback = [](const AZ::Crc32& toolId, const AtomToolsFramework::DocumentTypeInfo& docTypeInfo) {
            return aznew CeeMaterialDocument(toolId, docTypeInfo);
        };

        // Collect file extensions from the active backend's EnumerateMaterialTypes.
        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (backend)
        {
            AZStd::vector<MaterialTypeInfo> types;
            backend->GetMaterialSource().EnumerateMaterialTypes(types);
            for (const auto& type : types)
            {
                for (const auto& ext : type.m_fileExtensions)
                {
                    info.m_supportedExtensionsToCreate.emplace_back(type.m_displayName, ext);
                }
            }
            info.m_supportedExtensionsToOpen = info.m_supportedExtensionsToCreate;
            info.m_supportedExtensionsToSave = info.m_supportedExtensionsToCreate;
        }
        else
        {
            info.m_supportedExtensionsToCreate = { { "Cross-Engine Material", "json" } };
            info.m_supportedExtensionsToOpen = info.m_supportedExtensionsToCreate;
            info.m_supportedExtensionsToSave = info.m_supportedExtensionsToCreate;
        }
        return info;
    }

    AtomToolsFramework::DocumentObjectInfoVector CeeMaterialDocument::GetObjectInfo() const
    {
        AtomToolsFramework::DocumentObjectInfoVector objectInfo;

        for (const auto& group : m_groups)
        {
            if (!group)
            {
                continue;
            }

            // Match MaterialDocument::GetObjectInfoFromDynamicPropertyGroup pattern:
            // Pass the DynamicPropertyGroup pointer to the inspector, which reads its
            // reflected properties via the SerializeContext (DynamicPropertyGroup::Reflect).
            AtomToolsFramework::DocumentObjectInfo groupInfo;
            groupInfo.m_visible = group->m_visible;
            groupInfo.m_name = group->m_name;
            groupInfo.m_displayName = group->m_displayName;
            groupInfo.m_description = group->m_description;
            groupInfo.m_objectType = azrtti_typeid<AtomToolsFramework::DynamicPropertyGroup>();
            groupInfo.m_objectPtr = const_cast<AtomToolsFramework::DynamicPropertyGroup*>(group.get());

            groupInfo.m_nodeIndicatorFunction = [](const AzToolsFramework::InstanceDataNode* node) -> const char*
            {
                (void)node;
                return ":/Icons/blank.png";
            };

            objectInfo.push_back(AZStd::move(groupInfo));
        }

        return objectInfo;
    }

    bool CeeMaterialDocument::Open(const AZStd::string& loadPath)
    {
        Clear();

        IEngineBackend* backend = AZ::Interface<IEngineBackend>::Get();
        if (!backend)
        {
            AZ_Error("CeeMaterialDocument", false, "No engine backend registered.");
            return false;
        }

        m_source = &backend->GetMaterialSource();
        m_loadPath = loadPath;
        m_savePath = loadPath;

        // Create or load material through the backend contract.
        // Existing files should be loaded through the backend; only brand-new materials
        // should call CreateMaterial with the first available type id.
        if (loadPath.empty())
        {
            AZStd::vector<MaterialTypeInfo> types;
            m_source->EnumerateMaterialTypes(types);
            AZStd::string typeId = types.empty() ? AZStd::string("DemoPBR") : types.front().m_id;
            m_handle = m_source->CreateMaterial(typeId);
        }
        else
        {
            m_handle = m_source->LoadMaterial(loadPath);
        }

        if (m_handle == 0)
        {
            AZ_Error("CeeMaterialDocument", false, "Failed to create or load material instance.");
            return false;
        }

        RebuildFromBackend();
        m_valuesAtOpen = SnapshotValues();

        // Set the document absolute path (inherited from AtomToolsDocument).
        m_absolutePath = loadPath;

        return true;
    }

    bool CeeMaterialDocument::Save()
    {
        // Delegate to SaveAsCopy with the current save path.
        // Empty path means unsaved document - SaveAsCopy handles that.
        return SaveAsCopy(m_savePath);
    }

    bool CeeMaterialDocument::SaveAsCopy(const AZStd::string& savePath)
    {
        if (!m_source || m_handle == 0)
        {
            return false;
        }

        if (m_source->SaveMaterial(m_handle, savePath))
        {
            m_savePath = savePath;
            m_absolutePath = savePath;
            m_valuesAtOpen = SnapshotValues();
            return true;
        }
        return false;
    }

    bool CeeMaterialDocument::IsModified() const
    {
        if (!m_source || m_handle == 0)
        {
            return false;
        }

        MaterialPropertyValueMap currentValues;
        MaterialTypeDesc dummySchema;
        if (!m_source->GetMaterialState(m_handle, dummySchema, currentValues))
        {
            return false;
        }

        // Compare current values against opening snapshot.
        if (currentValues.size() != m_valuesAtOpen.size())
        {
            return true;
        }

        for (const auto& [key, value] : currentValues)
        {
            auto it = m_valuesAtOpen.find(key);
            if (it == m_valuesAtOpen.end())
            {
                return true;
            }
            if (value != it->second)
            {
                return true;
            }
        }
        return false;
    }

    bool CeeMaterialDocument::BeginEdit()
    {
        m_valuesBeforeEdit = SnapshotValues();
        return true;
    }

    bool CeeMaterialDocument::EndEdit()
    {
        if (!m_source || m_handle == 0)
        {
            return false;
        }

        MaterialPropertyValueMap currentValues;
        MaterialTypeDesc dummySchema;
        if (!m_source->GetMaterialState(m_handle, dummySchema, currentValues))
        {
            return false;
        }

        // Find changed values.
        MaterialPropertyValueMap undoValues;
        MaterialPropertyValueMap redoValues;
        bool anyChanged = false;

        for (const auto& [key, value] : currentValues)
        {
            auto beforeIt = m_valuesBeforeEdit.find(key);
            if (beforeIt == m_valuesBeforeEdit.end())
            {
                // New property added (shouldn't happen normally).
                continue;
            }
            if (beforeIt->second != value)
            {
                undoValues[key] = beforeIt->second;
                redoValues[key] = value;
                anyChanged = true;
            }
        }

        if (!anyChanged)
        {
            // Still process deferred schema rebuild even if no undo change.
            if (m_schemaRebuildPending)
            {
                m_schemaRebuildPending = false;
                RebuildFromBackend();
            }
            return true;
        }

        // AtomToolsDocument provides AddUndoRedoHistory(UndoRedoFunction, UndoRedoFunction).
        // Undo replay: suppress repeated schema rebuilds by accumulating a dirty flag and
        // rebuilding once after the whole value batch is applied (material_migration_final.md SS5.3).
        // m_undoRebuildPending is a member to avoid dangling references from stack captures.
        auto undoFn = [this, undoValues = AZStd::move(undoValues)]() mutable
        {
            for (const auto& [key, value] : undoValues)
            {
                const auto result = m_source->SetPropertyValue(m_handle, key, value);
                if (result == SetResult::AppliedSchemaChanged)
                {
                    m_undoRebuildPending = true;
                }
            }
            RebuildFromBackend();
            AtomToolsFramework::AtomToolsDocumentNotificationBus::Event(
                m_toolId,
                &AtomToolsFramework::AtomToolsDocumentNotifications::OnDocumentModified, m_id);
        };

        auto redoFn = [this, redoValues = AZStd::move(redoValues)]() mutable
        {
            for (const auto& [key, value] : redoValues)
            {
                const auto result = m_source->SetPropertyValue(m_handle, key, value);
                if (result == SetResult::AppliedSchemaChanged)
                {
                    m_undoRebuildPending = true;
                }
            }
            RebuildFromBackend();
            AtomToolsFramework::AtomToolsDocumentNotificationBus::Event(
                m_toolId,
                &AtomToolsFramework::AtomToolsDocumentNotifications::OnDocumentModified, m_id);
        };

        AddUndoRedoHistory(AZStd::move(undoFn), AZStd::move(redoFn));

        // Process deferred schema rebuild (AppliedSchemaChanged was deferred from
        // ApplyEdit to avoid destroying DynamicProperty objects during RPE iteration).
        if (m_schemaRebuildPending)
        {
            m_schemaRebuildPending = false;
            RebuildFromBackend();
        }

        return true;
    }

    void CeeMaterialDocument::Clear()
    {
        if (m_source && m_handle != 0)
        {
            m_source->DestroyMaterial(m_handle);
        }
        m_source = nullptr;
        m_handle = 0;
        m_loadPath.clear();
        m_savePath.clear();
        m_groups.clear();
        m_valuesAtOpen.clear();
        m_valuesBeforeEdit.clear();

        AtomToolsFramework::AtomToolsDocument::Clear();
    }

    void CeeMaterialDocument::RebuildFromBackend()
    {
        if (!m_source || m_handle == 0)
        {
            return;
        }

        MaterialTypeDesc schema;
        MaterialPropertyValueMap values;
        if (!m_source->GetMaterialState(m_handle, schema, values))
        {
            return;
        }

        m_groups.clear();
        m_schemaProperties.clear();
        m_groupToggleProperties.clear();

        AZStd::function<void(const MaterialPropertyGroupDesc&)> processGroup;
        processGroup = [&](const MaterialPropertyGroupDesc& groupDesc) -> void
        {
            auto group = AZStd::make_shared<AtomToolsFramework::DynamicPropertyGroup>();
            group->m_name = groupDesc.m_id;
            group->m_displayName = groupDesc.m_displayName;
            group->m_description = groupDesc.m_description;

            // S7: store group->toggle property mapping for inspector group-header checkbox.
            if (!groupDesc.m_toggleProperty.empty())
            {
                m_groupToggleProperties[groupDesc.m_id] = groupDesc.m_toggleProperty;
            }

            for (const auto& propDesc : groupDesc.m_properties)
            {
                if (!propDesc.m_visible)
                {
                    continue;
                }

                m_schemaProperties[propDesc.m_id] = propDesc;

                // Find the current value.
                MaterialPropertyValue currentValue;
                auto valueIt = values.find(propDesc.m_id);
                if (valueIt != values.end())
                {
                    currentValue = valueIt->second;
                }
                else
                {
                    currentValue = propDesc.m_defaultValue;
                }

                AtomToolsFramework::DynamicPropertyConfig config = BuildPropertyConfig(propDesc, currentValue);

                // Build the DynamicProperty manually (no AddDynamicProperty in DynamicPropertyGroup).
                AtomToolsFramework::DynamicProperty property(config);
                property.SetValue(ValueToAny(currentValue));

                group->m_properties.push_back(AZStd::move(property));
            }

            m_groups.push_back(AZStd::move(group));

            for (const auto& subGroup : groupDesc.m_groups)
            {
                processGroup(subGroup);
            }
        };

        for (const auto& groupDesc : schema.m_groups)
        {
            processGroup(groupDesc);
        }

        // Notify inspector about each group (upstream pattern: MaterialDocument.cpp:948-953).
        for (const auto& group : m_groups)
        {
            AtomToolsFramework::DocumentObjectInfo groupInfo;
            groupInfo.m_visible = group->m_visible;
            groupInfo.m_name = group->m_name;
            groupInfo.m_displayName = group->m_displayName;
            groupInfo.m_description = group->m_description;
            groupInfo.m_objectType = azrtti_typeid<AtomToolsFramework::DynamicPropertyGroup>();
            groupInfo.m_objectPtr = const_cast<AtomToolsFramework::DynamicPropertyGroup*>(group.get());
            AtomToolsFramework::AtomToolsDocumentNotificationBus::Event(
                m_toolId,
                &AtomToolsFramework::AtomToolsDocumentNotifications::OnDocumentObjectInfoChanged,
                m_id, groupInfo, true); // rebuilt=true for full rebuild
        }
    }

    void CeeMaterialDocument::ApplyEdit(const AZ::Name& id, const AZStd::any& value)
    {
        if (!m_source || m_handle == 0)
        {
            return;
        }

        // Resolve the expected property type from the cached schema snapshot.
        MaterialPropertyType expectedType = MaterialPropertyType::Float;
        for (const auto& [key, cachedProp] : m_schemaProperties)
        {
            if (key == id.GetStringView())
            {
                expectedType = cachedProp.m_type;
                break;
            }
        }

        MaterialPropertyValue matValue = AnyToValue(value, expectedType);
        SetResult result = m_source->SetPropertyValue(m_handle, id.GetStringView(), matValue);

        switch (result)
        {
        case SetResult::Applied:
            // Value applied; no schema change. The RPE already shows the new value,
            // no rebuild needed (material_migration_final.md SS4.6).
            break;

        case SetResult::AppliedSchemaChanged:
            // Visibility and/or other values changed. Defer rebuild to EndEdit to
            // avoid destroying DynamicProperty objects while the RPE is iterating them.
            m_schemaRebuildPending = true;
            break;

        case SetResult::Failed:
            AZ_Warning("CeeMaterialDocument", false, "SetPropertyValue failed for '%s'", id.GetCStr());
            RebuildFromBackend();
            return;
        }

        AtomToolsFramework::AtomToolsDocumentNotificationBus::Event(
            m_toolId,
            &AtomToolsFramework::AtomToolsDocumentNotifications::OnDocumentModified, m_id);
    }

    MaterialPropertyValueMap CeeMaterialDocument::SnapshotValues() const
    {
        if (!m_source || m_handle == 0)
        {
            return {};
        }

        MaterialTypeDesc dummySchema;
        MaterialPropertyValueMap values;
        [[maybe_unused]] const bool ok = m_source->GetMaterialState(m_handle, dummySchema, values);
        return values;
    }

    AZStd::any CeeMaterialDocument::ValueToAny(const MaterialPropertyValue& value)
    {
        return AZStd::visit(
            [](const auto& v) -> AZStd::any
            {
                using T = AZStd::decay_t<decltype(v)>;
                if constexpr (AZStd::is_same_v<T, AZStd::monostate>)
                {
                    return AZStd::any();
                }
                else
                {
                    return AZStd::any(v);
                }
            },
            value);
    }

    MaterialPropertyValue CeeMaterialDocument::AnyToValue(const AZStd::any& any, MaterialPropertyType expectedType)
    {
        if (any.empty())
        {
            return AZStd::monostate{};
        }

        // Try direct extraction based on expected type.
        switch (expectedType)
        {
        case MaterialPropertyType::Bool:
            if (auto* p = AZStd::any_cast<bool>(&any))
            {
                return *p;
            }
            break;
        case MaterialPropertyType::Int:
            if (auto* p = AZStd::any_cast<AZ::s32>(&any))
            {
                return *p;
            }
            break;
        case MaterialPropertyType::UInt:
        case MaterialPropertyType::Enum:
            if (auto* p = AZStd::any_cast<AZ::u32>(&any))
            {
                return *p;
            }
            break;
        case MaterialPropertyType::Float:
            if (auto* p = AZStd::any_cast<float>(&any))
            {
                return *p;
            }
            break;
        case MaterialPropertyType::Vec2:
            if (auto* p = AZStd::any_cast<AZ::Vector2>(&any))
            {
                return *p;
            }
            break;
        case MaterialPropertyType::Vec3:
            if (auto* p = AZStd::any_cast<AZ::Vector3>(&any))
            {
                return *p;
            }
            break;
        case MaterialPropertyType::Vec4:
            if (auto* p = AZStd::any_cast<AZ::Vector4>(&any))
            {
                return *p;
            }
            break;
        case MaterialPropertyType::Color:
            if (auto* p = AZStd::any_cast<AZ::Color>(&any))
            {
                return *p;
            }
            break;
        case MaterialPropertyType::Texture:
        case MaterialPropertyType::String:
            if (auto* p = AZStd::any_cast<AZStd::string>(&any))
            {
                return *p;
            }
            break;
        }

        return AZStd::monostate{};
    }

    AtomToolsFramework::DynamicPropertyConfig CeeMaterialDocument::BuildPropertyConfig(
        const MaterialPropertyDesc& propDesc, [[maybe_unused]] const MaterialPropertyValue& currentValue)
    {
        AtomToolsFramework::DynamicPropertyConfig config;
        config.m_id = propDesc.m_id;
        config.m_name = propDesc.m_id;
        config.m_displayName = propDesc.m_displayName;
        config.m_description = propDesc.m_description;
        config.m_defaultValue = ValueToAny(propDesc.m_defaultValue);
        config.m_parentValue = config.m_defaultValue; // modified indicator vs type default
        config.m_readOnly = propDesc.m_readOnly;

        // Color space (S2).
        config.m_colorSpace = static_cast<AZ::u32>(propDesc.m_colorSpace);

        // Range hints.
        if (propDesc.m_type == MaterialPropertyType::Int)
        {
            if (propDesc.m_min.has_value())
            {
                config.m_min = AZStd::any(static_cast<AZ::s32>(propDesc.m_min.value()));
            }
            if (propDesc.m_max.has_value())
            {
                config.m_max = AZStd::any(static_cast<AZ::s32>(propDesc.m_max.value()));
            }
            if (propDesc.m_softMin.has_value())
            {
                config.m_softMin = AZStd::any(static_cast<AZ::s32>(propDesc.m_softMin.value()));
            }
            if (propDesc.m_softMax.has_value())
            {
                config.m_softMax = AZStd::any(static_cast<AZ::s32>(propDesc.m_softMax.value()));
            }
            if (propDesc.m_step.has_value())
            {
                config.m_step = AZStd::any(static_cast<AZ::s32>(propDesc.m_step.value()));
            }
        }
        else if (propDesc.m_type == MaterialPropertyType::UInt || propDesc.m_type == MaterialPropertyType::Enum)
        {
            if (propDesc.m_min.has_value())
            {
                config.m_min = AZStd::any(static_cast<AZ::u32>(propDesc.m_min.value()));
            }
            if (propDesc.m_max.has_value())
            {
                config.m_max = AZStd::any(static_cast<AZ::u32>(propDesc.m_max.value()));
            }
            if (propDesc.m_softMin.has_value())
            {
                config.m_softMin = AZStd::any(static_cast<AZ::u32>(propDesc.m_softMin.value()));
            }
            if (propDesc.m_softMax.has_value())
            {
                config.m_softMax = AZStd::any(static_cast<AZ::u32>(propDesc.m_softMax.value()));
            }
            if (propDesc.m_step.has_value())
            {
                config.m_step = AZStd::any(static_cast<AZ::u32>(propDesc.m_step.value()));
            }
        }
        else
        {
            if (propDesc.m_min.has_value())
            {
                config.m_min = aznumeric_cast<float>(propDesc.m_min.value());
            }
            if (propDesc.m_max.has_value())
            {
                config.m_max = aznumeric_cast<float>(propDesc.m_max.value());
            }
            if (propDesc.m_softMin.has_value())
            {
                config.m_softMin = aznumeric_cast<float>(propDesc.m_softMin.value());
            }
            if (propDesc.m_softMax.has_value())
            {
                config.m_softMax = aznumeric_cast<float>(propDesc.m_softMax.value());
            }
            if (propDesc.m_step.has_value())
            {
                config.m_step = aznumeric_cast<float>(propDesc.m_step.value());
            }
        }

        // S6: unit suffix.
        config.m_suffix = propDesc.m_suffix;

        // Enum values.
        for (const auto& enumVal : propDesc.m_enumValues)
        {
            config.m_enumValues.push_back(enumVal);
        }

        // Vector labels.
        for (const auto& label : propDesc.m_vectorLabels)
        {
            config.m_vectorLabels.push_back(label);
        }

        // S5: file extensions for texture picker.
        config.m_fileExtensions = propDesc.m_fileExtensions;

        // Type-specific configuration - inferred from m_defaultValue type and m_enumValues.
        // The DynamicProperty uses the value type + attributes to select the RPE widget.
        if (propDesc.m_type == MaterialPropertyType::Texture)
        {
            config.m_customHandler = AZ_CRC_CE("StringFilePath");
        }

        // Edit callback: RPE invokes this when user modifies a property value.
        // Routes the change back to the backend via ApplyEdit (upstream pattern:
        // MaterialDocument.cpp:706-711).
        config.m_dataChangeCallback = [this, id = AZ::Name(propDesc.m_id.c_str())](const AZStd::any& value) -> AZ::u32
        {
            ApplyEdit(id, value);
            return AZ::Edit::PropertyRefreshLevels::AttributesAndValues;
        };

        return config;
    }
} // namespace CrossEngineEditor
