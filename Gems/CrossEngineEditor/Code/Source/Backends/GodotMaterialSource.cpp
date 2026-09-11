/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Backends/GodotMaterialSource.h>
#include <Backends/GodotBackend.h>

#include <AzCore/Math/MathUtils.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace CrossEngineEditor
{
    // Godot property_info.h / variant.h constants (verified against I:\godot master 2026-09):
    // variant.h Variant::Type: BOOL=1, INT=2, FLOAT=3, STRING=4, VECTOR2=5, VECTOR3=9,
    //   VECTOR3I=10, PLANE=14, COLOR=20, OBJECT=24
    // property_info.h PropertyHint: RANGE=1, ENUM=2, RESOURCE_TYPE=17, GROUP_ENABLE=42
    // viewport.h UpdateMode: ONCE=1, ALWAYS=4
    constexpr int64_t kPropertyTypeBool = 1;
    constexpr int64_t kPropertyTypeInt = 2;
    constexpr int64_t kPropertyTypeFloat = 3;
    constexpr int64_t kPropertyTypeString = 4;
    constexpr int64_t kPropertyTypeVector3 = 9;
    constexpr int64_t kPropertyTypeColor = 20;
    constexpr int64_t kPropertyTypeObject = 24;

    constexpr int64_t kHintNone = 0;
    constexpr int64_t kHintRange = 1;
    constexpr int64_t kHintEnum = 2;
    constexpr int64_t kHintResourceType = 17;
    constexpr int64_t kHintGroupEnable = 42;

    constexpr int64_t kUsageStorage = 1 << 1;
    constexpr int64_t kUsageEditor = 1 << 2;
    constexpr int64_t kUsageGroup = 1 << 6;
    constexpr int64_t kUsageCategory = 1 << 7;
    constexpr int64_t kUsageSubgroup = 1 << 8;
    constexpr int64_t kUsageReadOnly = 1 << 28;

    constexpr const char* kMaterialClassName = "StandardMaterial3D";
    constexpr const char* kTextureClassName = "Texture2D";
    constexpr const char* kResourceLoader = "ResourceLoader";
    constexpr const char* kResourceSaver = "ResourceSaver";

    // Preview defaults (material_editor_plugin.cpp:321-340):
    constexpr float kCameraDistance = 1.1f;
    constexpr float kCameraFov = 20.0f;
    constexpr float kSphereScale = 0.375f;
    constexpr float kBoxScale = 0.25f;
    constexpr float kQuadScale = 0.375f;

    // Godot RESOURCE_CACHE_MODE_REUSE = 1
    constexpr int64_t kCacheModeReuse = 1;

    // Viewport update mode: UPDATE_ONCE = 1 (viewport.h:911)
    constexpr int64_t kViewportUpdateOnce = 1;

    namespace
    {
        // Split "Val1:2,Val2,Val3" into display names (colon-delimited explicit values stripped).
        void SplitEnumHint(const AZStd::string& hintString, AZStd::vector<AZStd::string>& out)
        {
            size_t start = 0;
            while (start <= hintString.size())
            {
                const size_t comma = hintString.find(',', start);
                const size_t end = (comma == AZStd::string::npos) ? hintString.size() : comma;
                AZStd::string token = hintString.substr(start, end - start);
                const size_t colon = token.find(':');
                if (colon != AZStd::string::npos)
                {
                    token = token.substr(0, colon);
                }
                if (!token.empty())
                {
                    out.push_back(token);
                }
                if (comma == AZStd::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
        }
    } // namespace

    CrossEngineEditor::GodotApi& GodotMaterialSource::Api() const
    {
        return *m_api;
    }

    // State() is inline in the header (returns void*).

    // ======================================================================
    // GodotMaterialSource
    // ======================================================================

    GodotMaterialSource::GodotMaterialSource(void* engineState)
        : m_engineState(engineState)
    {
        // Cache the GodotApi pointer from EngineState for safe access.
        if (m_engineState)
        {
            auto& state = *static_cast<GodotBackend::EngineState*>(m_engineState);
            m_api = &state.m_api;
        }
    }

    void GodotMaterialSource::EnumerateMaterialTypes(AZStd::vector<MaterialTypeInfo>& out)
    {
        MaterialTypeInfo info;
        info.m_id = kMaterialClassName;
        info.m_displayName = "Standard PBR";
        info.m_description = "Godot StandardMaterial3D PBR material";
        info.m_fileExtensions = { "tres", "res" };
        out.push_back(AZStd::move(info));
    }

    MaterialHandle GodotMaterialSource::CreateMaterial(AZStd::string_view typeId)
    {
        GodotApi& api = Api();
        if (!api.IsValid())
        {
            return 0;
        }

        GDExtensionObjectPtr obj = api.ConstructObject(kMaterialClassName);
        if (!obj)
        {
            AZ_Warning("CrossEngineEditor", false, "GodotMaterialSource: failed to create '%s'.", kMaterialClassName);
            return 0;
        }

        MaterialInstance* rawInstance = aznew MaterialInstance();
        rawInstance->m_object = obj;
        rawInstance->m_typeId = AZStd::string(typeId);
        rawInstance->m_schema = BuildSchemaFromObject(obj);
        ReadValuesFromObject(obj, rawInstance->m_schema, rawInstance->m_values);

        const MaterialHandle handle = m_nextHandle++;
        m_materials[handle] = AZStd::shared_ptr<MaterialInstance>(rawInstance);
        return handle;
    }

    MaterialHandle GodotMaterialSource::LoadMaterial(AZStd::string_view absolutePath)
    {
        GodotApi& api = Api();
        if (!api.IsValid())
        {
            return 0;
        }

        GDExtensionObjectPtr loader = api.GetSingleton(kResourceLoader);
        if (!loader)
        {
            AZ_Warning("CrossEngineEditor", false, "GodotMaterialSource: ResourceLoader singleton missing.");
            return 0;
        }

        // ResourceLoader.load(path, type_hint="", cache_mode=CACHE_MODE_REUSE)
        GodotVariant args[3];
        args[0] = api.MakeString(AZStd::string(absolutePath).c_str());
        args[1] = api.MakeString(""); // empty type hint -> auto-detect
        args[2] = api.MakeInt(kCacheModeReuse);

        GodotVariant ret = api.Call(loader, "load", args, 3);
        GDExtensionObjectPtr obj = api.AsObject(ret);
        if (!obj)
        {
            AZ_Warning("CrossEngineEditor", false, "GodotMaterialSource: load failed for '%s'.",
                AZStd::string(absolutePath).c_str());
            return 0;
        }

        MaterialInstance* rawInstance = aznew MaterialInstance();
        rawInstance->m_object = obj;
        rawInstance->m_typeId = kMaterialClassName;
        rawInstance->m_path = AZStd::string(absolutePath);
        rawInstance->m_schema = BuildSchemaFromObject(obj);
        ReadValuesFromObject(obj, rawInstance->m_schema, rawInstance->m_values);

        const MaterialHandle handle = m_nextHandle++;
        m_materials[handle] = AZStd::shared_ptr<MaterialInstance>(rawInstance);
        return handle;
    }

    void GodotMaterialSource::DestroyMaterial(MaterialHandle handle)
    {
        m_materials.erase(handle);
    }

    bool GodotMaterialSource::GetMaterialState(
        MaterialHandle handle, MaterialTypeDesc& outSchema, MaterialPropertyValueMap& outValues)
    {
        auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return false;
        }

        // Dynamic visibility: _validate_property may have changed since last call,
        // so we always rebuild the schema from the live object (no caching).
        outSchema = BuildSchemaFromObject(it->second->m_object);
        ReadValuesFromObject(it->second->m_object, outSchema, outValues);
        return true;
    }

    SetResult GodotMaterialSource::SetPropertyValue(
        MaterialHandle handle, AZStd::string_view propertyId, const MaterialPropertyValue& value)
    {
        auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return SetResult::Failed;
        }

        GodotApi& api = Api();
        if (!api.IsValid())
        {
            return SetResult::Failed;
        }

        // Find the property type from schema.
        MaterialPropertyType propType = MaterialPropertyType::Float;
        for (const auto& group : it->second->m_schema.m_groups)
        {
            for (const auto& prop : group.m_properties)
            {
                if (prop.m_id == propertyId)
                {
                    propType = prop.m_type;
                    break;
                }
            }
        }

        GodotVariant variant;
        if (!PropertyValueToVariant(value, propType, &variant))
        {
            return SetResult::Failed;
        }

        api.SetProperty(it->second->m_object, AZStd::string(propertyId).c_str(), variant);
        it->second->m_values[AZStd::string(propertyId)] = value;

        // Mark preview dirty for live preview update.
        if (m_preview.m_boundHandle == handle)
        {
            m_preview.m_dirty = true;
        }

        // Per material_migration.md §4.5: return AppliedSchemaChanged when the
        // property change may alter visibility of other properties (e.g. emission_enabled
        // showing/hiding emission sub-properties). Check for "_enabled" suffix, mode
        // changes, and texture assignments (which toggle *_texture_channel visibility).
        AZStd::string propIdStr(propertyId);
        static const AZStd::vector<AZStd::string> schemaChangingProps = {
            "emission_enabled", "normal_enabled", "bent_normal_enabled",
            "rim_enabled", "clearcoat_enabled", "anisotropy_enabled",
            "ao_enabled", "heightmap_enabled", "subsurf_scatter_enabled",
            "subsurf_scatter_transmittance_enabled", "backlight_enabled",
            "refraction_enabled", "detail_enabled", "grow",
            "proximity_fade_enabled", "transparency", "shading_mode",
            // Texture properties: assigning a texture reveals *_texture_channel rows.
            "albedo_texture", "metallic_texture", "roughness_texture",
            "normal_texture", "ao_texture", "emission_texture",
            "rim_texture", "clearcoat_texture", "anisotropy_flowmap",
            "heightmap_texture", "ao_light_affect"
        };
        for (const auto& name : schemaChangingProps)
        {
            if (propIdStr == name)
            {
                return SetResult::AppliedSchemaChanged;
            }
        }

        return SetResult::Applied;
    }

    bool GodotMaterialSource::SaveMaterial(MaterialHandle handle, AZStd::string_view absolutePath)
    {
        GodotApi& api = Api();
        if (!api.IsValid())
        {
            return false;
        }

        auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return false;
        }

        GDExtensionObjectPtr saver = api.GetSingleton(kResourceSaver);
        if (!saver)
        {
            AZ_Warning("CrossEngineEditor", false, "GodotMaterialSource: ResourceSaver singleton missing.");
            return false;
        }

        // ResourceSaver.save(resource, path, flags=0)
        GodotVariant args[3];
        args[0] = api.MakeObject(it->second->m_object);
        args[1] = api.MakeString(AZStd::string(absolutePath).c_str());
        args[2] = api.MakeInt(0);

        GodotVariant ret = api.Call(saver, "save", args, 3);
        const int64_t err = api.AsInt(ret);
        if (err != 0)
        {
            AZ_Warning("CrossEngineEditor", false, "GodotMaterialSource: save failed (Error %lld) for '%s'.",
                static_cast<long long>(err), AZStd::string(absolutePath).c_str());
            return false;
        }

        it->second->m_path = AZStd::string(absolutePath);
        return true;
    }

    void GodotMaterialSource::SetPreviewMaterial(MaterialHandle handle)
    {
        m_preview.m_boundHandle = handle;
        m_preview.m_dirty = true;
    }

    bool GodotMaterialSource::SetPreviewModel(PreviewModel model)
    {
        m_preview.m_model = static_cast<int>(model);
        m_preview.m_dirty = true;
        return true;
    }

    PreviewResult GodotMaterialSource::AcquirePreviewImage(
        uint32_t width, uint32_t height, AZStd::vector<AZ::u8>& outPixels)
    {
        GodotApi& api = Api();
        auto* state = static_cast<GodotBackend::EngineState*>(State());
        if (!api.IsValid() || !state->m_running)
        {
            return PreviewResult::Unsupported;
        }

        if (m_preview.m_boundHandle == 0)
        {
            return PreviewResult::Unsupported;
        }

        // Phase 2: If pending readback, try to get the image.
        if (m_preview.m_pendingReadback)
        {
            PreviewResult result = ReadPreviewTexture(width, height, outPixels);
            if (result == PreviewResult::Updated)
            {
                m_preview.m_pendingReadback = false;
                m_preview.m_dirty = false;
                return PreviewResult::Updated;
            }
            // If the image is not ready yet, keep waiting.
            return PreviewResult::Unchanged;
        }

        // Phase 1: Not pending readback. If not dirty, nothing to do.
        if (!m_preview.m_dirty)
        {
            return PreviewResult::Unchanged;
        }

        EnsurePreviewScene(m_preview);

        // Apply material to the active mesh.
        auto it = m_materials.find(m_preview.m_boundHandle);
        if (it != m_materials.end())
        {
            GodotVariant matV = api.MakeObject(it->second->m_object);
            api.Call(m_preview.m_sphere, "set_material_override", &matV, 1);
            api.Call(m_preview.m_box, "set_material_override", &matV, 1);
            api.Call(m_preview.m_quad, "set_material_override", &matV, 1);
        }

        // Show the active mesh, hide others.
        auto setVis = [&api](GDExtensionObjectPtr node, bool vis)
        {
            GodotVariant v = api.MakeBool(vis);
            api.Call(node, "set_visible", &v, 1);
        };
        setVis(m_preview.m_sphere, m_preview.m_model == static_cast<int>(PreviewModel::Sphere));
        setVis(m_preview.m_box, m_preview.m_model == static_cast<int>(PreviewModel::Cube));
        setVis(m_preview.m_quad, m_preview.m_model == static_cast<int>(PreviewModel::Plane));

        // Trigger one-frame render.
        if (m_preview.m_viewport)
        {
            GodotVariant modeArg = api.MakeInt(kViewportUpdateOnce);
            api.Call(m_preview.m_viewport, "set_update_mode", &modeArg, 1);
        }

        // Enter pending readback state. The next call will try to retrieve the image.
        m_preview.m_pendingReadback = true;
        m_preview.m_dirty = false;

        // Do not attempt immediate readback; let the engine render the new frame first.
        return PreviewResult::Unchanged;
    }

    // ======================================================================
    // BuildSchemaFromObject -- runtime reflection, zero hand-written schema
    // ======================================================================

    MaterialTypeDesc GodotMaterialSource::BuildSchemaFromObject(void* obj) const
    {
        GodotApi& api = Api();
        MaterialTypeDesc schema;

        // Get class name for schema id.
        GodotVariant classV = api.Call(obj, "get_class");
        schema.m_id = api.VariantStringToAz(classV);
        schema.m_displayName = schema.m_id;
        schema.m_description = AZStd::string("Auto-generated schema from ") + schema.m_id;

        // No schema cache: _validate_property evaluates on the live instance,
        // so visibility depends on current values and must be re-evaluated each call.

        // Walk get_property_list().
        GodotVariant listV = api.Call(obj, "get_property_list");
        if (api.TypeOf(listV) != GDEXTENSION_VARIANT_TYPE_ARRAY)
        {
            return schema;
        }

        MaterialPropertyGroupDesc* currentGroup = nullptr;
        AZStd::string currentGroupPrefix;
        AZStd::string currentGroupDisplayName;
        const int64_t count = api.ArraySize(listV);

        for (int64_t i = 0; i < count; ++i)
        {
            GodotVariant entry = api.ArrayGet(listV, i);
            if (api.TypeOf(entry) != GDEXTENSION_VARIANT_TYPE_DICTIONARY)
            {
                continue;
            }

            const AZStd::string propName = api.VariantStringToAz(api.DictGet(entry, "name"));
            const int64_t type = api.AsInt(api.DictGet(entry, "type"));
            const int64_t usage = api.AsInt(api.DictGet(entry, "usage"));
            const int64_t hint = api.AsInt(api.DictGet(entry, "hint"));
            const AZStd::string hintString = api.VariantStringToAz(api.DictGet(entry, "hint_string"));

            // GROUP rows start a new group.
            if (usage & kUsageGroup)
            {
                currentGroupDisplayName = propName;
                // hint_string is the attribute name prefix for all properties in this group.
                currentGroupPrefix = hintString;
                currentGroup = nullptr; // will be set on first property in the group
                continue;
            }

            // SUBGROUP rows.
            if (usage & kUsageSubgroup)
            {
                // Subgroups are flattened into the parent group in v1.
                currentGroupPrefix = hintString;
                currentGroupDisplayName.clear(); // Subgroups don't have display names
                continue;
            }

            // CATEGORY rows reset grouping.
            if (usage & kUsageCategory)
            {
                currentGroup = nullptr;
                currentGroupPrefix.clear();
                currentGroupDisplayName.clear();
                continue;
            }

            // GROUP_ENABLE hint -> toggle property for the current group.
            if (hint == kHintGroupEnable)
            {
                if (!currentGroup)
                {
                    // Create a group for the property that precedes this toggle.
                    MaterialPropertyGroupDesc group;
                    group.m_id = propName;
                    group.m_displayName = !currentGroupDisplayName.empty() ? currentGroupDisplayName : propName;
                    currentGroup = &*schema.m_groups.emplace(schema.m_groups.end(), AZStd::move(group));
                }
                currentGroup->m_toggleProperty = propName;
            }

            // Must be both editor-visible and storable.
            if (!((usage & kUsageEditor) && (usage & kUsageStorage)))
            {
                continue;
            }

            // Lazily create the current group if needed.
            if (!currentGroup)
            {
                MaterialPropertyGroupDesc group;
                group.m_id = currentGroupPrefix.empty() ? propName : currentGroupPrefix;
                group.m_displayName = !currentGroupDisplayName.empty() ? currentGroupDisplayName : group.m_id;
                currentGroup = &*schema.m_groups.emplace(schema.m_groups.end(), AZStd::move(group));
            }

            // Build the property descriptor.
            MaterialPropertyDesc prop;
            prop.m_id = propName;
            prop.m_displayName = propName;
            prop.m_readOnly = (usage & kUsageReadOnly) != 0;

            switch (type)
            {
            case kPropertyTypeBool:
                prop.m_type = MaterialPropertyType::Bool;
                break;
            case kPropertyTypeInt:
                if (hint == kHintEnum)
                {
                    prop.m_type = MaterialPropertyType::Enum;
                    SplitEnumHint(hintString, prop.m_enumValues);
                }
                else if (hint == kHintRange)
                {
                    prop.m_type = MaterialPropertyType::Int;
                    ParseRangeHint(hintString, prop.m_min, prop.m_max, prop.m_step,
                        prop.m_softMin, prop.m_softMax, prop.m_suffix);
                }
                else
                {
                    prop.m_type = MaterialPropertyType::Int;
                }
                break;
            case kPropertyTypeFloat:
                prop.m_type = MaterialPropertyType::Float;
                if (hint == kHintRange)
                {
                    ParseRangeHint(hintString, prop.m_min, prop.m_max, prop.m_step,
                        prop.m_softMin, prop.m_softMax, prop.m_suffix);
                }
                break;
            case kPropertyTypeString:
                prop.m_type = MaterialPropertyType::String;
                break;
            case kPropertyTypeVector3:
                prop.m_type = MaterialPropertyType::Vec3;
                break;
            case kPropertyTypeColor:
                prop.m_type = MaterialPropertyType::Color;
                prop.m_colorSpace = MaterialColorSpace::Linear;
                break;
            case kPropertyTypeObject:
                if (hint == kHintResourceType && hintString == kTextureClassName)
                {
                    prop.m_type = MaterialPropertyType::Texture;
                    prop.m_fileExtensions = { "png", "jpg", "jpeg", "webp", "tga", "dds" };
                }
                else
                {
                    continue; // Discard unsupported Object types.
                }
                break;
            default:
                continue; // Unsupported Variant type.
            }

            currentGroup->m_properties.push_back(AZStd::move(prop));
        }

        // Remove empty groups.
        auto& groups = schema.m_groups;
        groups.erase(
            AZStd::remove_if(groups.begin(), groups.end(),
                [](const MaterialPropertyGroupDesc& g) { return g.m_properties.empty() && g.m_toggleProperty.empty(); }),
            groups.end());

        return schema;
    }

    // ======================================================================
    // ReadValuesFromObject
    // ======================================================================

    void GodotMaterialSource::ReadValuesFromObject(
        void* obj, const MaterialTypeDesc& schema, MaterialPropertyValueMap& outValues) const
    {
        GodotApi& api = Api();
        outValues.clear();

        for (const auto& group : schema.m_groups)
        {
            for (const auto& prop : group.m_properties)
            {
                if (!prop.m_visible)
                {
                    continue;
                }

                GodotVariant valV = api.GetProperty(obj, prop.m_id.c_str());
                MaterialPropertyValue val;
                if (VariantToPropertyValue(obj, prop.m_id, api.TypeOf(valV),
                    prop.m_type == MaterialPropertyType::Enum ? kHintEnum : kHintNone, "", val))
                {
                    outValues[prop.m_id] = val;
                }
            }
        }
    }

    // ======================================================================
    // VariantToPropertyValue
    // ======================================================================

    bool GodotMaterialSource::VariantToPropertyValue(
        void* obj, const AZStd::string& propName,
        int64_t variantType, int64_t hint, const AZStd::string& /*hintString*/,
        MaterialPropertyValue& out) const
    {
        GodotApi& api = Api();
        GodotVariant valV = api.GetProperty(obj, propName.c_str());

        switch (variantType)
        {
        case GDEXTENSION_VARIANT_TYPE_BOOL:
            out = api.AsBool(valV);
            return true;
        case GDEXTENSION_VARIANT_TYPE_INT:
            if (hint == kHintEnum)
            {
                out = static_cast<AZ::u32>(api.AsInt(valV));
            }
            else
            {
                out = static_cast<AZ::s32>(api.AsInt(valV));
            }
            return true;
        case GDEXTENSION_VARIANT_TYPE_FLOAT:
            out = static_cast<float>(api.AsFloat(valV));
            return true;
        case GDEXTENSION_VARIANT_TYPE_STRING:
            out = api.VariantStringToAz(valV);
            return true;
        case GDEXTENSION_VARIANT_TYPE_VECTOR3:
            out = api.AsVector3(valV);
            return true;
        case GDEXTENSION_VARIANT_TYPE_COLOR:
            out = api.AsColor(valV);
            return true;
        case GDEXTENSION_VARIANT_TYPE_OBJECT:
        {
            // Texture objects: extract resource_path string for the editor.
            if (hint == kHintResourceType)
            {
                GodotVariant pathV = api.GetProperty(valV.Ptr(), "resource_path");
                AZStd::string path = api.VariantStringToAz(pathV);
                out = AZStd::move(path);
                return true;
            }
            return false;
        }
        default:
            return false;
        }
    }

    // ======================================================================
    // PropertyValueToVariant
    // ======================================================================

    bool GodotMaterialSource::PropertyValueToVariant(
        const MaterialPropertyValue& value, MaterialPropertyType /*type*/, void* out) const
    {
        GodotApi& api = Api();
        GodotVariant* variant = static_cast<GodotVariant*>(out);

        // Use AZStd::any directly since MaterialPropertyValue is AZStd::variant not AZStd::any.
        // Access via AZStd::visit instead of any_cast.
        auto visitor = [&](auto&& v) -> bool
        {
            using T = AZStd::decay_t<decltype(v)>;
            if constexpr (AZStd::is_same_v<T, bool>)
            {
                *variant = api.MakeBool(v);
                return true;
            }
            else if constexpr (AZStd::is_same_v<T, AZ::s32>)
            {
                *variant = api.MakeInt(static_cast<int64_t>(v));
                return true;
            }
            else if constexpr (AZStd::is_same_v<T, AZ::u32>)
            {
                *variant = api.MakeInt(static_cast<int64_t>(v));
                return true;
            }
            else if constexpr (AZStd::is_same_v<T, float>)
            {
                *variant = api.MakeFloat(static_cast<double>(v));
                return true;
            }
            else if constexpr (AZStd::is_same_v<T, AZ::Vector3>)
            {
                *variant = api.MakeVector3(v);
                return true;
            }
            else if constexpr (AZStd::is_same_v<T, AZ::Color>)
            {
                *variant = api.MakeColor(v);
                return true;
            }
            else if constexpr (AZStd::is_same_v<T, AZStd::string>)
            {
                *variant = api.MakeString(v.c_str());
                return true;
            }
            else if constexpr (AZStd::is_same_v<T, AZStd::monostate>)
            {
                return false;
            }
            else
            {
                return false;
            }
        };
        return AZStd::visit(visitor, value);
    }

    // ======================================================================
    // ParseRangeHint -- "min,max,step[,or_greater][,or_less][,suffix:xxx]"
    // ======================================================================

    void GodotMaterialSource::ParseRangeHint(const AZStd::string& hintString,
        AZStd::optional<double>& min, AZStd::optional<double>& max,
        AZStd::optional<double>& step, AZStd::optional<double>& softMin,
        AZStd::optional<double>& softMax, AZStd::string& suffix) const
    {
        AZStd::vector<AZStd::string> tokens;
        AZStd::string::size_type start = 0;
        while (start <= hintString.size())
        {
            const auto comma = hintString.find(',', start);
            const auto end = (comma == AZStd::string::npos) ? hintString.size() : comma;
            tokens.push_back(hintString.substr(start, end - start));
            if (comma == AZStd::string::npos)
            {
                break;
            }
            start = comma + 1;
        }

        if (tokens.size() >= 2)
        {
            min = atof(tokens[0].c_str());
            max = atof(tokens[1].c_str());
            // softMin/softMax default to hard min/max; or_greater/or_less will adjust them.
            softMin = min;
            softMax = max;
        }
        if (tokens.size() >= 3)
        {
            step = atof(tokens[2].c_str());
        }
        for (size_t i = 3; i < tokens.size(); ++i)
        {
            if (tokens[i] == "or_greater")
            {
                max.reset(); // no hard upper bound (matches Godot set_allow_greater)
            }
            else if (tokens[i] == "or_less")
            {
                min.reset(); // no hard lower bound (matches Godot set_allow_less)
            }
            else if (tokens[i].substr(0, 7) == "suffix:")
            {
                suffix = tokens[i].substr(7);
            }
        }
    }

    // ======================================================================
    // Preview scene (SubViewport + Camera3D + lights + meshes)
    // ======================================================================

    void GodotMaterialSource::EnsurePreviewScene(PreviewState& preview)
    {
        if (preview.m_sceneCreated)
        {
            return;
        }

        GodotApi& api = Api();
        auto* state = static_cast<GodotBackend::EngineState*>(State());
        if (!api.IsValid())
        {
            return;
        }

        if (!state->m_sceneTree)
        {
            return;
        }

        // Get the scene root to parent the SubViewport.
        GodotVariant rootV = api.Call(state->m_sceneTree, "get_root");
        GDExtensionObjectPtr root = api.AsObject(rootV);
        if (!root)
        {
            return;
        }

        // Create SubViewport.
        preview.m_viewport = api.ConstructObject("SubViewport");
        if (!preview.m_viewport)
        {
            return;
        }

        // Disable input and set transparent background.
        {
            GodotVariant v = api.MakeBool(true);
            api.Call(preview.m_viewport, "set_disable_input", &v, 1);
        }
        {
            GodotVariant v = api.MakeBool(true);
            api.Call(preview.m_viewport, "set_transparent_background", &v, 1);
        }

        // Set viewport size to match the preview window dimensions (512x512 default).
        {
            GodotVariant sizeArgs[2] = { api.MakeInt(512), api.MakeInt(512) };
            api.Call(preview.m_viewport, "set_size", sizeArgs, 2);
        }

        // Set initial update mode to DISABLED (will be set to ONCE when dirty).
        {
            GodotVariant mode = api.MakeInt(0); // VIEWPORT_UPDATE_DISABLED = 0
            api.Call(preview.m_viewport, "set_update_mode", &mode, 1);
        }

        // Add SubViewport to scene root.
        GodotVariant vpV = api.MakeObject(preview.m_viewport);
        api.Call(root, "add_child", &vpV, 1);

        // Camera3D at (0, 0, 1.1) with FOV 20 degrees (material_editor_plugin.cpp:321-325).
        preview.m_camera = api.ConstructObject("Camera3D");
        if (preview.m_camera)
        {
            float camPos[12] = {};
            camPos[9] = 0.0f; // origin x
            camPos[10] = 0.0f; // origin y
            camPos[11] = kCameraDistance; // origin z
            // Identity basis.
            camPos[0] = 1.0f; camPos[4] = 1.0f; camPos[8] = 1.0f;
            GodotVariant tm = api.MakeTransform3D(camPos);
            api.Call(preview.m_camera, "set_transform", &tm, 1);
            {
                GodotVariant fov = api.MakeFloat(static_cast<double>(kCameraFov));
                api.Call(preview.m_camera, "set_perspective", &fov, 1);
            }
            GodotVariant camV = api.MakeObject(preview.m_camera);
            GodotVariant vpV2 = api.MakeObject(preview.m_viewport);
            api.Call(preview.m_viewport, "add_child", &camV, 1);
            api.Call(preview.m_camera, "make_current");
        }

        // DirectionalLight3D key light (material_editor_plugin.cpp:333-335).
        preview.m_light1 = api.ConstructObject("DirectionalLight3D");
        if (preview.m_light1)
        {
            GodotVariant lightV = api.MakeObject(preview.m_light1);
            api.Call(preview.m_viewport, "add_child", &lightV, 1);
        }

        // DirectionalLight3D fill light (material_editor_plugin.cpp:337-340).
        preview.m_light2 = api.ConstructObject("DirectionalLight3D");
        if (preview.m_light2)
        {
            GodotVariant lightV = api.MakeObject(preview.m_light2);
            api.Call(preview.m_viewport, "add_child", &lightV, 1);
        }

        // Rotation parent node for mesh instances.
        preview.m_rotation = api.ConstructObject("Node3D");
        if (preview.m_rotation)
        {
            GodotVariant rotV = api.MakeObject(preview.m_rotation);
            api.Call(preview.m_viewport, "add_child", &rotV, 1);
        }

        // Sphere MeshInstance3D (material_editor_plugin.cpp:345-363).
        preview.m_sphere = api.ConstructObject("MeshInstance3D");
        if (preview.m_sphere)
        {
            GDExtensionObjectPtr sphereMesh = api.ConstructObject("SphereMesh");
            if (sphereMesh)
            {
                GodotVariant meshV = api.MakeObject(sphereMesh);
                api.Call(preview.m_sphere, "set_mesh", &meshV, 1);
            }
            float scale[12] = {};
            scale[0] = kSphereScale; scale[4] = kSphereScale; scale[8] = kSphereScale;
            scale[9] = 0.0f; scale[10] = 0.0f; scale[11] = 0.0f;
            GodotVariant tm = api.MakeTransform3D(scale);
            api.Call(preview.m_sphere, "set_transform", &tm, 1);
            GodotVariant sphereV = api.MakeObject(preview.m_sphere);
            api.Call(preview.m_rotation, "add_child", &sphereV, 1);
        }

        // Box MeshInstance3D.
        preview.m_box = api.ConstructObject("MeshInstance3D");
        if (preview.m_box)
        {
            GDExtensionObjectPtr boxMesh = api.ConstructObject("BoxMesh");
            if (boxMesh)
            {
                GodotVariant meshV = api.MakeObject(boxMesh);
                api.Call(preview.m_box, "set_mesh", &meshV, 1);
            }
            float scale[12] = {};
            scale[0] = kBoxScale; scale[4] = kBoxScale; scale[8] = kBoxScale;
            scale[9] = 0.0f; scale[10] = 0.0f; scale[11] = 0.0f;
            GodotVariant tm = api.MakeTransform3D(scale);
            api.Call(preview.m_box, "set_transform", &tm, 1);
            GodotVariant boxV = api.MakeObject(preview.m_box);
            api.Call(preview.m_rotation, "add_child", &boxV, 1);
        }

        // Quad MeshInstance3D.
        preview.m_quad = api.ConstructObject("MeshInstance3D");
        if (preview.m_quad)
        {
            GDExtensionObjectPtr quadMesh = api.ConstructObject("QuadMesh");
            if (quadMesh)
            {
                GodotVariant meshV = api.MakeObject(quadMesh);
                api.Call(preview.m_quad, "set_mesh", &meshV, 1);
            }
            float scale[12] = {};
            scale[0] = kQuadScale; scale[4] = kQuadScale; scale[8] = kQuadScale;
            scale[9] = 0.0f; scale[10] = 0.0f; scale[11] = 0.0f;
            GodotVariant tm = api.MakeTransform3D(scale);
            api.Call(preview.m_quad, "set_transform", &tm, 1);
            GodotVariant quadV = api.MakeObject(preview.m_quad);
            api.Call(preview.m_rotation, "add_child", &quadV, 1);
        }

        preview.m_sceneCreated = true;
    }

    void GodotMaterialSource::DestroyPreviewScene(PreviewState& preview)
    {
        GodotApi& api = Api();
        auto* state = static_cast<GodotBackend::EngineState*>(State());
        if (!api.IsValid())
        {
            return;
        }

        // Remove the SubViewport from the scene root and free it.
        if (preview.m_viewport)
        {
            GodotVariant vpV = api.MakeObject(preview.m_viewport);
            GodotVariant rootV = api.Call(state->m_sceneTree, "get_root");
            GDExtensionObjectPtr root = api.AsObject(rootV);
            if (root)
            {
                api.Call(root, "remove_child", &vpV, 1);
            }
            api.Call(preview.m_viewport, "queue_free");
        }

        preview.m_viewport = nullptr;
        preview.m_camera = nullptr;
        preview.m_light1 = nullptr;
        preview.m_light2 = nullptr;
        preview.m_sphere = nullptr;
        preview.m_box = nullptr;
        preview.m_quad = nullptr;
        preview.m_rotation = nullptr;
        preview.m_sceneCreated = false;
    }

    // ======================================================================
    // ReadPreviewTexture -- read SubViewport texture
    // ======================================================================

    PreviewResult GodotMaterialSource::ReadPreviewTexture(
        uint32_t width, uint32_t height, AZStd::vector<AZ::u8>& outPixels)
    {
        GodotApi& api = Api();
        if (!api.IsValid() || !m_preview.m_viewport)
        {
            return PreviewResult::Unsupported;
        }

        // Use Viewport.get_image() to get the rendered image as an Image object.
        GodotVariant imgV = api.Call(m_preview.m_viewport, "get_image");
        if (api.TypeOf(imgV) == GDEXTENSION_VARIANT_TYPE_NIL || api.AsObject(imgV) == nullptr)
        {
            return PreviewResult::Unchanged;
        }

        const int64_t imgW = api.AsInt(api.Call(api.AsObject(imgV), "get_width"));
        const int64_t imgH = api.AsInt(api.Call(api.AsObject(imgV), "get_height"));
        if (imgW <= 0 || imgH <= 0)
        {
            return PreviewResult::Unchanged;
        }

        // Image.get_data() returns PackedByteArray of raw RGBA8 pixels.
        GodotVariant dataV = api.Call(api.AsObject(imgV), "get_data");
        if (api.TypeOf(dataV) != GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY)
        {
            return PreviewResult::Unchanged;
        }

        // Get the size of the packed byte array.
        GodotVariant sizeV = api.CallVariant(dataV, "size");
        const int64_t dataSize = api.AsInt(sizeV);
        if (dataSize <= 0)
        {
            return PreviewResult::Unchanged;
        }

        // Bulk read via packed_byte_array_operator_index if available, else per-byte fallback.
        const size_t bytesNeeded = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        const size_t dataSizeStatic = static_cast<size_t>(dataSize);
        const size_t copySize = AZStd::min(dataSizeStatic, bytesNeeded);

        if (api.PackedByteReady())
        {
            if (void* internalData = api.InternalPtr(dataV, GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY))
            {
                if (const void* dataPtr = api.PackedByteData(internalData))
                {
                    outPixels.resize(copySize);
                    memcpy(outPixels.data(), dataPtr, copySize);
                    return PreviewResult::Updated;
                }
            }
        }

        // Fallback: per-byte read (slow, but functional).
        outPixels.resize(copySize);
        for (size_t b = 0; b < copySize; ++b)
        {
            GodotVariant idx = api.MakeInt(static_cast<int64_t>(b));
            GodotVariant byteV = api.CallVariant(dataV, "get", &idx, 1);
            outPixels[b] = static_cast<AZ::u8>(api.AsInt(byteV));
        }

        return PreviewResult::Updated;
    }
} // namespace CrossEngineEditor
