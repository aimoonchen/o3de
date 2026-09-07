/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <MaterialEditor/MaterialSchema.h>

#include <QFile>
#include <QTextStream>

#include <AzCore/JSON/document.h>
#include <AzCore/JSON/prettywriter.h>
#include <AzCore/JSON/stringbuffer.h>

namespace CrossEngineEditor
{
    // ======================================================================
    // Hand-written rapidjson Load/Save (bypasses AZ::JsonSerialization which
    // doesn't support AZStd::variant or AZStd::optional by design).
    // ======================================================================

    namespace
    {
        AZStd::string RjStr(const rapidjson::Value& v, const char* key, const char* def = "")
        {
            if (v.HasMember(key) && v[key].IsString())
            {
                return AZStd::string(v[key].GetString());
            }
            return def;
        }

        AZStd::vector<AZStd::string> RjStrArray(const rapidjson::Value& v, const char* key)
        {
            AZStd::vector<AZStd::string> out;
            if (v.HasMember(key) && v[key].IsArray())
            {
                for (const auto& item : v[key].GetArray())
                {
                    out.push_back(item.IsString() ? item.GetString() : "");
                }
            }
            return out;
        }

        bool RjBool(const rapidjson::Value& v, const char* key, bool def = false)
        {
            return v.HasMember(key) && v[key].IsBool() ? v[key].GetBool() : def;
        }

        AZStd::optional<double> RjOptionalDouble(const rapidjson::Value& v, const char* key)
        {
            if (v.HasMember(key) && v[key].IsNumber())
            {
                return v[key].GetDouble();
            }
            return AZStd::nullopt;
        }

        double RjDouble(const rapidjson::Value& v, const char* key, double def = 0.0)
        {
            return v.HasMember(key) && v[key].IsNumber() ? v[key].GetDouble() : def;
        }

        int64_t RjInt(const rapidjson::Value& v, const char* key, int64_t def = 0)
        {
            return v.HasMember(key) && v[key].IsInt64() ? v[key].GetInt64() : def;
        }

        // Parse m_defaultValue based on m_type.
        MaterialPropertyValue ParseDefaultValue(const rapidjson::Value& v, MaterialPropertyType type)
        {
            if (!v.HasMember("m_defaultValue"))
            {
                return AZStd::monostate{};
            }
            const auto& val = v["m_defaultValue"];

            switch (type)
            {
            case MaterialPropertyType::Bool:
                return val.IsBool() ? MaterialPropertyValue(val.GetBool()) : MaterialPropertyValue(AZStd::monostate{});
            case MaterialPropertyType::Int:
                return val.IsInt() ? MaterialPropertyValue(static_cast<AZ::s32>(val.GetInt())) : MaterialPropertyValue(AZStd::monostate{});
            case MaterialPropertyType::UInt:
            case MaterialPropertyType::Enum:
                return val.IsUint() ? MaterialPropertyValue(static_cast<AZ::u32>(val.GetUint())) :
                       val.IsInt() ? MaterialPropertyValue(static_cast<AZ::u32>(val.GetInt())) :
                       MaterialPropertyValue(AZStd::monostate{});
            case MaterialPropertyType::Float:
                return val.IsNumber() ? MaterialPropertyValue(static_cast<float>(val.GetDouble())) : MaterialPropertyValue(AZStd::monostate{});
            case MaterialPropertyType::Vec2:
                if (val.IsArray() && val.Size() >= 2)
                {
                    return MaterialPropertyValue(AZ::Vector2(
                        static_cast<float>(val[0].GetDouble()), static_cast<float>(val[1].GetDouble())));
                }
                break;
            case MaterialPropertyType::Vec3:
                if (val.IsArray() && val.Size() >= 3)
                {
                    return MaterialPropertyValue(AZ::Vector3(
                        static_cast<float>(val[0].GetDouble()), static_cast<float>(val[1].GetDouble()),
                        static_cast<float>(val[2].GetDouble())));
                }
                break;
            case MaterialPropertyType::Vec4:
                if (val.IsArray() && val.Size() >= 4)
                {
                    return MaterialPropertyValue(AZ::Vector4(
                        static_cast<float>(val[0].GetDouble()), static_cast<float>(val[1].GetDouble()),
                        static_cast<float>(val[2].GetDouble()), static_cast<float>(val[3].GetDouble())));
                }
                break;
            case MaterialPropertyType::Color:
                if (val.IsArray() && val.Size() >= 4)
                {
                    return MaterialPropertyValue(AZ::Color(
                        static_cast<float>(val[0].GetDouble()), static_cast<float>(val[1].GetDouble()),
                        static_cast<float>(val[2].GetDouble()), static_cast<float>(val[3].GetDouble())));
                }
                break;
            case MaterialPropertyType::Texture:
            case MaterialPropertyType::String:
                return val.IsString() ? MaterialPropertyValue(AZStd::string(val.GetString())) :
                       MaterialPropertyValue(AZStd::monostate{});
            default:
                break;
            }
            return AZStd::monostate{};
        }

        bool ParsePropertyDesc(const rapidjson::Value& v, MaterialPropertyDesc& out)
        {
            out.m_id = RjStr(v, "m_id");
            out.m_displayName = RjStr(v, "m_displayName");
            out.m_description = RjStr(v, "m_description");
            out.m_readOnly = RjBool(v, "m_readOnly");
            out.m_visible = RjBool(v, "m_visible", true);
            out.m_softMin = RjOptionalDouble(v, "m_softMin");
            out.m_softMax = RjOptionalDouble(v, "m_softMax");
            out.m_min = RjOptionalDouble(v, "m_min");
            out.m_max = RjOptionalDouble(v, "m_max");
            out.m_step = RjOptionalDouble(v, "m_step");
            out.m_suffix = RjStr(v, "m_suffix");
            out.m_enumValues = RjStrArray(v, "m_enumValues");
            out.m_vectorLabels = RjStrArray(v, "m_vectorLabels");
            out.m_fileExtensions = RjStrArray(v, "m_fileExtensions");

            // Parse m_type as string name.
            const AZStd::string typeName = RjStr(v, "m_type");
            if (typeName == "Bool") out.m_type = MaterialPropertyType::Bool;
            else if (typeName == "Int") out.m_type = MaterialPropertyType::Int;
            else if (typeName == "UInt") out.m_type = MaterialPropertyType::UInt;
            else if (typeName == "Float") out.m_type = MaterialPropertyType::Float;
            else if (typeName == "Vec2") out.m_type = MaterialPropertyType::Vec2;
            else if (typeName == "Vec3") out.m_type = MaterialPropertyType::Vec3;
            else if (typeName == "Vec4") out.m_type = MaterialPropertyType::Vec4;
            else if (typeName == "Color") out.m_type = MaterialPropertyType::Color;
            else if (typeName == "Enum") out.m_type = MaterialPropertyType::Enum;
            else if (typeName == "Texture") out.m_type = MaterialPropertyType::Texture;
            else if (typeName == "String") out.m_type = MaterialPropertyType::String;
            else return false;

            // Parse m_colorSpace as string name.
            const AZStd::string csName = RjStr(v, "m_colorSpace");
            if (csName == "Srgb") out.m_colorSpace = MaterialColorSpace::Srgb;
            else out.m_colorSpace = MaterialColorSpace::Linear;

            out.m_defaultValue = ParseDefaultValue(v, out.m_type);
            return true;
        }

        void ParseGroupDesc(const rapidjson::Value& v, MaterialPropertyGroupDesc& out)
        {
            out.m_id = RjStr(v, "m_id");
            out.m_displayName = RjStr(v, "m_displayName");
            out.m_description = RjStr(v, "m_description");
            out.m_defaultCollapsed = RjBool(v, "m_defaultCollapsed");
            out.m_toggleProperty = RjStr(v, "m_toggleProperty");

            if (v.HasMember("m_properties") && v["m_properties"].IsArray())
            {
                for (const auto& prop : v["m_properties"].GetArray())
                {
                    MaterialPropertyDesc desc;
                    if (ParsePropertyDesc(prop, desc))
                    {
                        out.m_properties.push_back(AZStd::move(desc));
                    }
                }
            }
            if (v.HasMember("m_groups") && v["m_groups"].IsArray())
            {
                for (const auto& group : v["m_groups"].GetArray())
                {
                    MaterialPropertyGroupDesc sub;
                    ParseGroupDesc(group, sub);
                    out.m_groups.push_back(AZStd::move(sub));
                }
            }
        }

        void WriteValue(rapidjson::Value& out, const MaterialPropertyValue& value, rapidjson::Document::AllocatorType& alloc)
        {
            AZStd::visit([&](const auto& v)
            {
                using T = AZStd::decay_t<decltype(v)>;
                if constexpr (AZStd::is_same_v<T, AZStd::monostate>)
                {
                    out.SetNull();
                }
                else if constexpr (AZStd::is_same_v<T, bool>)
                {
                    out.SetBool(v);
                }
                else if constexpr (AZStd::is_same_v<T, AZ::s32>)
                {
                    out.SetInt(v);
                }
                else if constexpr (AZStd::is_same_v<T, AZ::u32>)
                {
                    out.SetUint(v);
                }
                else if constexpr (AZStd::is_same_v<T, float>)
                {
                    out.SetDouble(static_cast<double>(v));
                }
                else if constexpr (AZStd::is_same_v<T, AZ::Vector2>)
                {
                    out.SetArray();
                    out.PushBack(v.GetX(), alloc);
                    out.PushBack(v.GetY(), alloc);
                }
                else if constexpr (AZStd::is_same_v<T, AZ::Vector3>)
                {
                    out.SetArray();
                    out.PushBack(v.GetX(), alloc);
                    out.PushBack(v.GetY(), alloc);
                    out.PushBack(v.GetZ(), alloc);
                }
                else if constexpr (AZStd::is_same_v<T, AZ::Vector4>)
                {
                    out.SetArray();
                    out.PushBack(v.GetX(), alloc);
                    out.PushBack(v.GetY(), alloc);
                    out.PushBack(v.GetZ(), alloc);
                    out.PushBack(v.GetW(), alloc);
                }
                else if constexpr (AZStd::is_same_v<T, AZ::Color>)
                {
                    out.SetArray();
                    out.PushBack(v.GetR(), alloc);
                    out.PushBack(v.GetG(), alloc);
                    out.PushBack(v.GetB(), alloc);
                    out.PushBack(v.GetA(), alloc);
                }
                else if constexpr (AZStd::is_same_v<T, AZStd::string>)
                {
                    out.SetString(v.c_str(), alloc);
                }
            }, value);
        }

        const char* TypeName(MaterialPropertyType type)
        {
            switch (type)
            {
            case MaterialPropertyType::Bool: return "Bool";
            case MaterialPropertyType::Int: return "Int";
            case MaterialPropertyType::UInt: return "UInt";
            case MaterialPropertyType::Float: return "Float";
            case MaterialPropertyType::Vec2: return "Vec2";
            case MaterialPropertyType::Vec3: return "Vec3";
            case MaterialPropertyType::Vec4: return "Vec4";
            case MaterialPropertyType::Color: return "Color";
            case MaterialPropertyType::Enum: return "Enum";
            case MaterialPropertyType::Texture: return "Texture";
            case MaterialPropertyType::String: return "String";
            default: return "Float";
            }
        }

        const char* ColorSpaceName(MaterialColorSpace cs)
        {
            return cs == MaterialColorSpace::Srgb ? "Srgb" : "Linear";
        }

        void WritePropertyDesc(rapidjson::Value& out, const MaterialPropertyDesc& prop,
            rapidjson::Document::AllocatorType& alloc)
        {
            out.SetObject();
            out.AddMember("m_id", rapidjson::Value(prop.m_id.c_str(), alloc), alloc);
            out.AddMember("m_displayName", rapidjson::Value(prop.m_displayName.c_str(), alloc), alloc);
            out.AddMember("m_description", rapidjson::Value(prop.m_description.c_str(), alloc), alloc);
            out.AddMember("m_type", rapidjson::Value(TypeName(prop.m_type), alloc), alloc);

            rapidjson::Value defaultVal;
            WriteValue(defaultVal, prop.m_defaultValue, alloc);
            out.AddMember("m_defaultValue", defaultVal, alloc);

            if (prop.m_min.has_value()) out.AddMember("m_min", prop.m_min.value(), alloc);
            if (prop.m_max.has_value()) out.AddMember("m_max", prop.m_max.value(), alloc);
            if (prop.m_softMin.has_value()) out.AddMember("m_softMin", prop.m_softMin.value(), alloc);
            if (prop.m_softMax.has_value()) out.AddMember("m_softMax", prop.m_softMax.value(), alloc);
            if (prop.m_step.has_value()) out.AddMember("m_step", prop.m_step.value(), alloc);
            if (!prop.m_suffix.empty()) out.AddMember("m_suffix", rapidjson::Value(prop.m_suffix.c_str(), alloc), alloc);
            if (!prop.m_enumValues.empty())
            {
                rapidjson::Value arr(rapidjson::kArrayType);
                for (const auto& s : prop.m_enumValues)
                    arr.PushBack(rapidjson::Value(s.c_str(), alloc), alloc);
                out.AddMember("m_enumValues", arr, alloc);
            }
            if (!prop.m_vectorLabels.empty())
            {
                rapidjson::Value arr(rapidjson::kArrayType);
                for (const auto& s : prop.m_vectorLabels)
                    arr.PushBack(rapidjson::Value(s.c_str(), alloc), alloc);
                out.AddMember("m_vectorLabels", arr, alloc);
            }
            if (!prop.m_fileExtensions.empty())
            {
                rapidjson::Value arr(rapidjson::kArrayType);
                for (const auto& s : prop.m_fileExtensions)
                    arr.PushBack(rapidjson::Value(s.c_str(), alloc), alloc);
                out.AddMember("m_fileExtensions", arr, alloc);
            }
            out.AddMember("m_colorSpace", rapidjson::Value(ColorSpaceName(prop.m_colorSpace), alloc), alloc);
            out.AddMember("m_visible", prop.m_visible, alloc);
            out.AddMember("m_readOnly", prop.m_readOnly, alloc);
        }

        void WriteGroupDesc(rapidjson::Value& out, const MaterialPropertyGroupDesc& group,
            rapidjson::Document::AllocatorType& alloc)
        {
            out.SetObject();
            out.AddMember("m_id", rapidjson::Value(group.m_id.c_str(), alloc), alloc);
            out.AddMember("m_displayName", rapidjson::Value(group.m_displayName.c_str(), alloc), alloc);
            out.AddMember("m_description", rapidjson::Value(group.m_description.c_str(), alloc), alloc);
            out.AddMember("m_defaultCollapsed", group.m_defaultCollapsed, alloc);
            if (!group.m_toggleProperty.empty())
                out.AddMember("m_toggleProperty", rapidjson::Value(group.m_toggleProperty.c_str(), alloc), alloc);

            rapidjson::Value props(rapidjson::kArrayType);
            for (const auto& prop : group.m_properties)
            {
                rapidjson::Value propVal;
                WritePropertyDesc(propVal, prop, alloc);
                props.PushBack(propVal, alloc);
            }
            out.AddMember("m_properties", props, alloc);

            rapidjson::Value groups(rapidjson::kArrayType);
            for (const auto& sub : group.m_groups)
            {
                rapidjson::Value subVal;
                WriteGroupDesc(subVal, sub, alloc);
                groups.PushBack(subVal, alloc);
            }
            out.AddMember("m_groups", groups, alloc);
        }
    } // anonymous namespace

    bool MaterialSchema::LoadFromFile(const AZStd::string& filePath, MaterialTypeDesc& outSchema)
    {
        QFile file(QString::fromUtf8(filePath.c_str()));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            return false;
        }

        QTextStream stream(&file);
        const AZStd::string content(stream.readAll().toUtf8().constData());
        file.close();

        return LoadFromString(content, outSchema);
    }

    bool MaterialSchema::LoadFromString(const AZStd::string& jsonString, MaterialTypeDesc& outSchema)
    {
        rapidjson::Document doc;
        doc.Parse(jsonString.c_str());
        if (doc.HasParseError() || !doc.IsObject())
        {
            return false;
        }

        outSchema.m_id = RjStr(doc, "m_id");
        outSchema.m_displayName = RjStr(doc, "m_displayName");
        outSchema.m_description = RjStr(doc, "m_description");

        outSchema.m_groups.clear();
        if (doc.HasMember("m_groups") && doc["m_groups"].IsArray())
        {
            for (const auto& group : doc["m_groups"].GetArray())
            {
                MaterialPropertyGroupDesc groupDesc;
                ParseGroupDesc(group, groupDesc);
                outSchema.m_groups.push_back(AZStd::move(groupDesc));
            }
        }
        return true;
    }

    bool MaterialSchema::SaveToFile(const AZStd::string& filePath, const MaterialTypeDesc& schema)
    {
        rapidjson::Document doc;
        auto& alloc = doc.GetAllocator();

        doc.SetObject();
        doc.AddMember("m_id", rapidjson::Value(schema.m_id.c_str(), alloc), alloc);
        doc.AddMember("m_displayName", rapidjson::Value(schema.m_displayName.c_str(), alloc), alloc);
        doc.AddMember("m_description", rapidjson::Value(schema.m_description.c_str(), alloc), alloc);

        rapidjson::Value groups(rapidjson::kArrayType);
        for (const auto& group : schema.m_groups)
        {
            rapidjson::Value groupVal;
            WriteGroupDesc(groupVal, group, alloc);
            groups.PushBack(groupVal, alloc);
        }
        doc.AddMember("m_groups", groups, alloc);

        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        AZStd::string output(buffer.GetString(), buffer.GetSize());

        QFile file(QString::fromUtf8(filePath.c_str()));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            return false;
        }

        QTextStream stream(&file);
        stream << QString::fromUtf8(output.c_str(), static_cast<int>(output.size()));
        file.close();

        return true;
    }

    bool MaterialSchema::SaveToFile(
        const AZStd::string& filePath, const MaterialTypeDesc& schema, const MaterialPropertyValueMap& values)
    {
        rapidjson::Document doc;
        auto& alloc = doc.GetAllocator();

        doc.SetObject();

        // Write schema inline.
        {
            rapidjson::Value schemaVal(rapidjson::kObjectType);
            schemaVal.AddMember("m_id", rapidjson::Value(schema.m_id.c_str(), alloc), alloc);
            schemaVal.AddMember("m_displayName", rapidjson::Value(schema.m_displayName.c_str(), alloc), alloc);
            schemaVal.AddMember("m_description", rapidjson::Value(schema.m_description.c_str(), alloc), alloc);

            rapidjson::Value groups(rapidjson::kArrayType);
            for (const auto& group : schema.m_groups)
            {
                rapidjson::Value groupVal;
                WriteGroupDesc(groupVal, group, alloc);
                groups.PushBack(groupVal, alloc);
            }
            schemaVal.AddMember("m_groups", groups, alloc);

            doc.AddMember("m_schema", schemaVal, alloc);
        }

        // Write values map: propId -> JSON value.
        {
            rapidjson::Value valuesVal(rapidjson::kObjectType);
            for (const auto& [id, val] : values)
            {
                rapidjson::Value jsonVal;
                WriteValue(jsonVal, val, alloc);
                valuesVal.AddMember(rapidjson::Value(id.c_str(), alloc), jsonVal, alloc);
            }
            doc.AddMember("m_values", valuesVal, alloc);
        }

        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        AZStd::string output(buffer.GetString(), buffer.GetSize());

        QFile file(QString::fromUtf8(filePath.c_str()));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            return false;
        }

        QTextStream stream(&file);
        stream << QString::fromUtf8(output.c_str(), static_cast<int>(output.size()));
        file.close();

        return true;
    }

    bool MaterialSchema::LoadFromFile(
        const AZStd::string& filePath, MaterialTypeDesc& outSchema, MaterialPropertyValueMap& outValues)
    {
        QFile file(QString::fromUtf8(filePath.c_str()));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            return false;
        }

        QTextStream stream(&file);
        const AZStd::string content(stream.readAll().toUtf8().constData());
        file.close();

        rapidjson::Document doc;
        doc.Parse(content.c_str());
        if (doc.HasParseError() || !doc.IsObject())
        {
            return false;
        }

        // Support both formats: new {m_schema, m_values} and old {m_id, m_groups}.
        const rapidjson::Value* schemaObj = nullptr;
        if (doc.HasMember("m_schema") && doc["m_schema"].IsObject())
        {
            schemaObj = &doc["m_schema"];
        }
        else
        {
            // Old format: the document itself is the schema.
            schemaObj = &doc;
        }

        outSchema.m_id = RjStr(*schemaObj, "m_id");
        outSchema.m_displayName = RjStr(*schemaObj, "m_displayName");
        outSchema.m_description = RjStr(*schemaObj, "m_description");

        if (schemaObj->HasMember("m_groups") && (*schemaObj)["m_groups"].IsArray())
        {
            for (const auto& group : (*schemaObj)["m_groups"].GetArray())
            {
                MaterialPropertyGroupDesc groupDesc;
                ParseGroupDesc(group, groupDesc);
                outSchema.m_groups.push_back(AZStd::move(groupDesc));
            }
        }

        // Parse values from schema defaults first, then override with saved values.
        outValues.clear();
        for (const auto& group : outSchema.m_groups)
        {
            for (const auto& prop : group.m_properties)
            {
                outValues[prop.m_id] = prop.m_defaultValue;
            }
        }

        if (doc.HasMember("m_values") && doc["m_values"].IsObject())
        {
            const auto& valuesObj = doc["m_values"];
            for (auto it = valuesObj.MemberBegin(); it != valuesObj.MemberEnd(); ++it)
            {
                const AZStd::string propId(it->name.GetString());

                // Look up the property type to parse correctly.
                MaterialPropertyType propType = MaterialPropertyType::Float; // fallback
                for (const auto& group : outSchema.m_groups)
                {
                    for (const auto& prop : group.m_properties)
                    {
                        if (prop.m_id == propId)
                        {
                            propType = prop.m_type;
                            break;
                        }
                    }
                }

                // Parse using a synthetic wrapper that makes it look like m_defaultValue.
                rapidjson::Document wrapper;
                wrapper.SetObject();
                rapidjson::Value savedValue(it->value, wrapper.GetAllocator());
                wrapper.AddMember("m_defaultValue", AZStd::move(savedValue), wrapper.GetAllocator());
                outValues[propId] = ParseDefaultValue(wrapper, propType);
            }
        }

        return true;
    }
} // namespace CrossEngineEditor
