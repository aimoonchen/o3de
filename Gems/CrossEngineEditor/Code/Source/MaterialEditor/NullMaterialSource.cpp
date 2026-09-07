/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <MaterialEditor/NullMaterialSource.h>
#include <MaterialEditor/MaterialSchema.h>

namespace CrossEngineEditor
{
    NullMaterialSource::NullMaterialSource() = default;

    void NullMaterialSource::EnumerateMaterialTypes(AZStd::vector<MaterialTypeInfo>& out)
    {
        MaterialTypeInfo info;
        info.m_id = "DemoPBR";
        info.m_displayName = "Demo PBR";
        info.m_description = "Built-in PBR material for testing (Null backend)";
        info.m_fileExtensions = { "json" };
        out.push_back(AZStd::move(info));
    }

    MaterialHandle NullMaterialSource::CreateMaterial([[maybe_unused]] AZStd::string_view typeId)
    {
        MaterialInstance* rawInstance = aznew MaterialInstance();
        rawInstance->m_schema = BuildDemoPbrSchema();

        // Set defaults from schema.
        for (auto& group : rawInstance->m_schema.m_groups)
        {
            for (auto& prop : group.m_properties)
            {
                rawInstance->m_values[prop.m_id] = prop.m_defaultValue;
            }
        }

        const MaterialHandle handle = m_nextHandle++;
        m_materials[handle] = AZStd::shared_ptr<MaterialInstance>(rawInstance);
        return handle;
    }

    MaterialHandle NullMaterialSource::LoadMaterial(AZStd::string_view absolutePath)
    {
        // Load schema and values from JSON file.
        MaterialTypeDesc schema;
        MaterialPropertyValueMap values;
        if (!MaterialSchema::LoadFromFile(AZStd::string(absolutePath), schema, values))
        {
            return 0;
        }

        MaterialInstance* rawInstance = aznew MaterialInstance();
        rawInstance->m_schema = AZStd::move(schema);
        rawInstance->m_values = AZStd::move(values);

        const MaterialHandle handle = m_nextHandle++;
        m_materials[handle] = AZStd::shared_ptr<MaterialInstance>(rawInstance);
        return handle;
    }

    void NullMaterialSource::DestroyMaterial(MaterialHandle handle)
    {
        m_materials.erase(handle);
    }

    bool NullMaterialSource::GetMaterialState(
        MaterialHandle handle, MaterialTypeDesc& outSchema, MaterialPropertyValueMap& outValues)
    {
        auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return false;
        }
        outSchema = it->second->m_schema;
        outValues = it->second->m_values;
        return true;
    }

    SetResult NullMaterialSource::SetPropertyValue(
        MaterialHandle handle, AZStd::string_view propertyId, const MaterialPropertyValue& value)
    {
        auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return SetResult::Failed;
        }
        it->second->m_values[AZStd::string(propertyId)] = value;
        return SetResult::Applied;
    }

    bool NullMaterialSource::SaveMaterial(MaterialHandle handle, AZStd::string_view absolutePath)
    {
        auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return false;
        }

        // Save both schema and current values to JSON.
        return MaterialSchema::SaveToFile(AZStd::string(absolutePath), it->second->m_schema, it->second->m_values);
    }

    MaterialTypeDesc NullMaterialSource::BuildDemoPbrSchema()
    {
        MaterialTypeDesc schema;
        schema.m_id = "DemoPBR";
        schema.m_displayName = "Demo PBR";
        schema.m_description = "Built-in PBR material for testing";

        // Base group.
        MaterialPropertyGroupDesc baseGroup;
        baseGroup.m_id = "Base";
        baseGroup.m_displayName = "Base";

        MaterialPropertyDesc baseColor;
        baseColor.m_id = "baseColor";
        baseColor.m_displayName = "Base Color";
        baseColor.m_description = "Diffuse/albedo color of the surface";
        baseColor.m_type = MaterialPropertyType::Color;
        baseColor.m_defaultValue = AZ::Color(1.0f, 1.0f, 1.0f, 1.0f);
        baseColor.m_colorSpace = MaterialColorSpace::Linear;
        baseGroup.m_properties.push_back(baseColor);

        MaterialPropertyDesc metallic;
        metallic.m_id = "metallic";
        metallic.m_displayName = "Metallic";
        metallic.m_description = "Metalness of the surface (0=dielectric, 1=metal)";
        metallic.m_type = MaterialPropertyType::Float;
        metallic.m_defaultValue = 0.0f;
        metallic.m_min = 0.0;
        metallic.m_max = 1.0;
        metallic.m_step = 0.01;
        baseGroup.m_properties.push_back(metallic);

        MaterialPropertyDesc roughness;
        roughness.m_id = "roughness";
        roughness.m_displayName = "Roughness";
        roughness.m_description = "Surface roughness (0=mirror, 1=diffuse)";
        roughness.m_type = MaterialPropertyType::Float;
        roughness.m_defaultValue = 0.5f;
        roughness.m_min = 0.0;
        roughness.m_max = 1.0;
        roughness.m_step = 0.01;
        baseGroup.m_properties.push_back(roughness);

        MaterialPropertyDesc normal;
        normal.m_id = "normal";
        normal.m_displayName = "Normal Map";
        normal.m_description = "Tangent-space normal map texture";
        normal.m_type = MaterialPropertyType::Texture;
        normal.m_defaultValue = AZStd::string();
        normal.m_fileExtensions = { "png", "tga", "dds", "jpg" };
        baseGroup.m_properties.push_back(normal);

        MaterialPropertyDesc occlusion;
        occlusion.m_id = "occlusion";
        occlusion.m_displayName = "Occlusion Map";
        occlusion.m_description = "Ambient occlusion texture (R channel)";
        occlusion.m_type = MaterialPropertyType::Texture;
        occlusion.m_defaultValue = AZStd::string();
        occlusion.m_fileExtensions = { "png", "tga", "dds", "jpg" };
        baseGroup.m_properties.push_back(occlusion);

        schema.m_groups.push_back(AZStd::move(baseGroup));

        // Emissive group.
        MaterialPropertyGroupDesc emissiveGroup;
        emissiveGroup.m_id = "Emissive";
        emissiveGroup.m_displayName = "Emissive";

        MaterialPropertyDesc emissiveColor;
        emissiveColor.m_id = "emissiveColor";
        emissiveColor.m_displayName = "Emissive Color";
        emissiveColor.m_description = "Emissive (self-illumination) color";
        emissiveColor.m_type = MaterialPropertyType::Color;
        emissiveColor.m_defaultValue = AZ::Color(0.0f, 0.0f, 0.0f, 1.0f);
        emissiveColor.m_colorSpace = MaterialColorSpace::Linear;
        emissiveGroup.m_properties.push_back(emissiveColor);

        MaterialPropertyDesc emissiveTexture;
        emissiveTexture.m_id = "emissiveTexture";
        emissiveTexture.m_displayName = "Emissive Texture";
        emissiveTexture.m_description = "Emissive texture (RGB channels)";
        emissiveTexture.m_type = MaterialPropertyType::Texture;
        emissiveTexture.m_defaultValue = AZStd::string();
        emissiveTexture.m_fileExtensions = { "png", "tga", "dds", "jpg" };
        emissiveGroup.m_properties.push_back(emissiveTexture);

        schema.m_groups.push_back(AZStd::move(emissiveGroup));

        // Rendering group.
        MaterialPropertyGroupDesc renderGroup;
        renderGroup.m_id = "Rendering";
        renderGroup.m_displayName = "Rendering";
        renderGroup.m_defaultCollapsed = true;

        MaterialPropertyDesc alphaMode;
        alphaMode.m_id = "alphaMode";
        alphaMode.m_displayName = "Alpha Mode";
        alphaMode.m_description = "How the material handles transparency";
        alphaMode.m_type = MaterialPropertyType::Enum;
        alphaMode.m_defaultValue = AZ::u32(0);
        alphaMode.m_enumValues = { "Opaque", "AlphaTest", "Blend" };
        renderGroup.m_properties.push_back(alphaMode);

        MaterialPropertyDesc doubleSided;
        doubleSided.m_id = "doubleSided";
        doubleSided.m_displayName = "Double Sided";
        doubleSided.m_description = "Disable backface culling";
        doubleSided.m_type = MaterialPropertyType::Bool;
        doubleSided.m_defaultValue = false;
        renderGroup.m_properties.push_back(doubleSided);

        schema.m_groups.push_back(AZStd::move(renderGroup));

        return schema;
    }
} // namespace CrossEngineEditor
