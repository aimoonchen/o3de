/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <MaterialEditor/RbfxMaterialSource.h>
#include <MaterialEditor/MaterialSchema.h>

#include <AzCore/Utils/Utils.h>

#if defined(_MSC_VER)
#    pragma warning(push, 0)
#    pragma warning(disable : 5054)
#endif
AZ_PUSH_DISABLE_WARNING(4251 4244 4245 4267 4100 4263 4264 4265 4266, "-Wunknown-warning-option")
#include <Urho3D/Core/Context.h>
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/Texture2D.h>
#include <Urho3D/Graphics/TextureCube.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/Utility/SceneRendererToTexture.h>
AZ_POP_DISABLE_WARNING
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

namespace CrossEngineEditor
{
    RbfxMaterialSource::RbfxMaterialSource(Urho3D::Context* context)
        : m_context(context)
    {
        LoadSchema();
    }

    RbfxMaterialSource::~RbfxMaterialSource() = default;

    void RbfxMaterialSource::LoadSchema()
    {
        AZ::IO::FixedMaxPath projectRoot = AZ::Utils::GetProjectPath();
        const AZStd::string fullPath = (projectRoot / "Gems/CrossEngineEditor/Assets/MaterialTypes/rbfx/StandardPBR.json").String();
        if (!MaterialSchema::LoadFromFile(fullPath, m_schema))
        {
            AZ_Error("RbfxMaterialSource", false,
                "Failed to load rbfx schema from '%s'", fullPath.c_str());
        }
    }

    void RbfxMaterialSource::EnumerateMaterialTypes(AZStd::vector<MaterialTypeInfo>& out)
    {
        MaterialTypeInfo info;
        info.m_id = "StandardPBR";
        info.m_displayName = "Standard PBR";
        info.m_description = "Standard PBR material for rbfx";
        info.m_fileExtensions = { "xml", "material" };
        out.push_back(AZStd::move(info));
    }

    MaterialHandle RbfxMaterialSource::CreateMaterial([[maybe_unused]] AZStd::string_view typeId)
    {
        auto* material = new Urho3D::Material(m_context);

        auto instance = AZStd::make_shared<MaterialInstance>();
        instance->m_material = material;
        instance->m_schema = m_schema;

        const MaterialHandle handle = m_nextHandle++;
        m_materials[handle] = instance;
        return handle;
    }

    MaterialHandle RbfxMaterialSource::LoadMaterial(AZStd::string_view absolutePath)
    {
        if (!m_context)
        {
            return 0;
        }

        auto* cache = m_context->GetSubsystem<Urho3D::ResourceCache>();
        if (!cache)
        {
            return 0;
        }

        Urho3D::Material* material =
            cache->GetResource<Urho3D::Material>(ea::string(absolutePath.data(), absolutePath.size()));
        if (!material)
        {
            return 0;
        }

        MaterialInstance* rawInstance = aznew MaterialInstance();
        rawInstance->m_material = material;
        rawInstance->m_schema = m_schema;

        const MaterialHandle handle = m_nextHandle++;
        m_materials[handle] = AZStd::shared_ptr<MaterialInstance>(rawInstance);
        return handle;
    }

    void RbfxMaterialSource::DestroyMaterial(MaterialHandle handle)
    {
        if (handle == m_previewTarget)
        {
            m_previewTarget = 0;
            m_rtt.reset();
        }
        m_materials.erase(handle);
    }

    bool RbfxMaterialSource::GetMaterialState(
        MaterialHandle handle, MaterialTypeDesc& outSchema, MaterialPropertyValueMap& outValues)
    {
        auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return false;
        }

        outSchema = it->second->m_schema;
        ReadValues(it->second->m_material, outValues);
        return true;
    }

    void RbfxMaterialSource::ReadValues(Urho3D::Material* material, MaterialPropertyValueMap& outValues) const
    {
        if (!material)
        {
            return;
        }

        // PBR shader parameters
        const auto& params = material->GetShaderParameters();
        for (const auto& [hash, param] : params)
        {
            const Urho3D::Variant& value = param.value_;
            switch (value.GetType())
            {
            case Urho3D::VAR_FLOAT:
                outValues[param.name_.c_str()] = value.GetFloat();
                break;
            case Urho3D::VAR_VECTOR3:
                {
                    const Urho3D::Vector3& v = value.GetVector3();
                    outValues[param.name_.c_str()] = AZ::Vector3(v.x_, v.y_, v.z_);
                }
                break;
            case Urho3D::VAR_VECTOR4:
                {
                    const Urho3D::Vector4& v = value.GetVector4();
                    outValues[param.name_.c_str()] = AZ::Vector4(v.x_, v.y_, v.z_, v.w_);
                }
                break;
            case Urho3D::VAR_COLOR:
                {
                    const Urho3D::Color& c = value.GetColor();
                    // rbfx Color has r_, g_, b_, a_ members.
                    outValues[param.name_.c_str()] = AZ::Color(c.r_, c.g_, c.b_, c.a_);
                }
                break;
            case Urho3D::VAR_BOOL:
                outValues[param.name_.c_str()] = value.GetBool();
                break;
            case Urho3D::VAR_INT:
                outValues[param.name_.c_str()] = static_cast<AZ::s32>(value.GetInt());
                break;
            case Urho3D::VAR_INT64:
                outValues[param.name_.c_str()] = static_cast<AZ::s32>(value.GetInt64());
                break;
            default:
                break;
            }
        }

        // Texture slots (ea::unordered_map<StringHash, MaterialTexture> from Material.h:64-71)
        const auto& textures = material->GetTextures();
        for (const auto& [hash, tex] : textures)
        {
            if (tex.value_)
            {
                outValues[tex.name_.c_str()] = AZStd::string(tex.value_->GetName().c_str());
            }
        }

        // Render states
        outValues["Cull"] = AZ::u32(material->GetCullMode());
        outValues["ShadowCull"] = AZ::u32(material->GetShadowCullMode());
        outValues["Fill"] = AZ::u32(material->GetFillMode());
        outValues["AlphaToCoverage"] = material->GetAlphaToCoverage();
        outValues["LineAntiAlias"] = material->GetLineAntiAlias();
        outValues["RenderOrder"] = AZ::u32(material->GetRenderOrder());
        outValues["Occlusion"] = material->GetOcclusion();

        const Urho3D::BiasParameters& bias = material->GetDepthBias();
        outValues["ConstantBias"] = bias.constantBias_;
        outValues["SlopeScaledBias"] = bias.slopeScaledBias_;
        outValues["NormalOffset"] = bias.normalOffset_;
    }

    SetResult RbfxMaterialSource::SetPropertyValue(
        MaterialHandle handle, AZStd::string_view propertyId, const MaterialPropertyValue& value)
    {
        auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return SetResult::Failed;
        }

        Urho3D::Material* material = it->second->m_material;
        const AZStd::string propId(propertyId);
        bool applied = false;

        // Generic shader parameter path: covers color, float, vector, and offset properties
        // without growing an if/else chain for every PBR name.
        const auto visitVariant = [&](auto&& v)
        {
            using T = AZStd::decay_t<decltype(v)>;
            if constexpr (AZStd::is_same_v<T, AZ::Color>)
            {
                material->SetShaderParameter(propId.c_str(),
                    Urho3D::Variant(Urho3D::Color(v.GetR(), v.GetG(), v.GetB(), v.GetA())));
                applied = true;
            }
            else if constexpr (AZStd::is_same_v<T, AZ::Vector4>)
            {
                material->SetShaderParameter(propId.c_str(),
                    Urho3D::Variant(Urho3D::Vector4(v.GetX(), v.GetY(), v.GetZ(), v.GetW())));
                applied = true;
            }
            else if constexpr (AZStd::is_same_v<T, AZ::Vector3>)
            {
                material->SetShaderParameter(propId.c_str(),
                    Urho3D::Variant(Urho3D::Vector3(v.GetX(), v.GetY(), v.GetZ())));
                applied = true;
            }
            else if constexpr (AZStd::is_same_v<T, float>)
            {
                material->SetShaderParameter(propId.c_str(), Urho3D::Variant(v));
                applied = true;
            }
            else if constexpr (AZStd::is_same_v<T, AZ::s32>)
            {
                material->SetShaderParameter(propId.c_str(), Urho3D::Variant(v));
                applied = true;
            }
            else if constexpr (AZStd::is_same_v<T, AZ::u32>)
            {
                material->SetShaderParameter(propId.c_str(), static_cast<int>(v));
                applied = true;
            }
            else if constexpr (AZStd::is_same_v<T, bool>)
            {
                material->SetShaderParameter(propId.c_str(), Urho3D::Variant(v));
                applied = true;
            }
        };

        AZStd::visit(visitVariant, value);

        // Texture slots need explicit load + engine call.
        if (!applied)
        {
            if (auto* path = AZStd::get_if<AZStd::string>(&value))
            {
                if (propId == "Albedo" || propId == "Normal" || propId == "Properties" || propId == "Emission" || propId == "Reflection0")
                {
                    if (path->empty())
                    {
                        material->SetTexture(propId.c_str(), nullptr);
                        applied = true;
                    }
                    else if (auto* cache = m_context->GetSubsystem<Urho3D::ResourceCache>())
                    {
                        Urho3D::Texture2D* tex = cache->GetResource<Urho3D::Texture2D>(
                            ea::string(path->c_str(), path->size()));
                        if (tex)
                        {
                            material->SetTexture(propId.c_str(), tex);
                            applied = true;
                        }
                        else
                        {
                            return SetResult::Failed;
                        }
                    }
                }
            }
        }

        // Render states need explicit engine calls.
        if (!applied)
        {
            if (propId == "Cull")
            {
                if (auto* u = AZStd::get_if<AZ::u32>(&value))
                {
                    material->SetCullMode(static_cast<Urho3D::CullMode>(*u));
                    applied = true;
                }
            }
            else if (propId == "ShadowCull")
            {
                if (auto* u = AZStd::get_if<AZ::u32>(&value))
                {
                    material->SetShadowCullMode(static_cast<Urho3D::CullMode>(*u));
                    applied = true;
                }
            }
            else if (propId == "Fill")
            {
                if (auto* u = AZStd::get_if<AZ::u32>(&value))
                {
                    material->SetFillMode(static_cast<Urho3D::FillMode>(*u));
                    applied = true;
                }
            }
            else if (propId == "AlphaToCoverage")
            {
                if (auto* b = AZStd::get_if<bool>(&value))
                {
                    material->SetAlphaToCoverage(*b);
                    applied = true;
                }
            }
            else if (propId == "LineAntiAlias")
            {
                if (auto* b = AZStd::get_if<bool>(&value))
                {
                    material->SetLineAntiAlias(*b);
                    applied = true;
                }
            }
            else if (propId == "RenderOrder")
            {
                if (auto* u = AZStd::get_if<AZ::u32>(&value))
                {
                    material->SetRenderOrder(static_cast<unsigned char>(*u));
                    applied = true;
                }
            }
            else if (propId == "Occlusion")
            {
                if (auto* b = AZStd::get_if<bool>(&value))
                {
                    material->SetOcclusion(*b);
                    applied = true;
                }
            }
            else if (propId == "ConstantBias" || propId == "SlopeScaledBias" || propId == "NormalOffset")
            {
                if (auto* f = AZStd::get_if<float>(&value))
                {
                    Urho3D::BiasParameters bias = material->GetDepthBias();
                    if (propId == "ConstantBias")
                    {
                        bias.constantBias_ = *f;
                    }
                    else if (propId == "SlopeScaledBias")
                    {
                        bias.slopeScaledBias_ = *f;
                    }
                    else
                    {
                        bias.normalOffset_ = *f;
                    }
                    material->SetDepthBias(bias);
                    applied = true;
                }
            }
        }

        if (!applied)
        {
            return SetResult::Failed;
        }

        it->second->m_dirty = true;
        if (handle == m_previewTarget)
        {
            m_previewDirty = true;
        }
        return SetResult::Applied;
    }

    bool RbfxMaterialSource::SaveMaterial(MaterialHandle handle, AZStd::string_view absolutePath)
    {
        auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return false;
        }

        if (!m_context)
        {
            return false;
        }

        Urho3D::XMLFile xml(m_context);
        Urho3D::XMLElement root = xml.CreateRoot("material");
        if (!it->second->m_material->Save(root))
        {
            return false;
        }

        Urho3D::File file(m_context, ea::string(absolutePath.data(), absolutePath.size()), Urho3D::FILE_WRITE);
        if (!file.IsOpen())
        {
            return false;
        }

        return xml.Save(file);
    }

    void RbfxMaterialSource::SetPreviewMaterial(MaterialHandle handle)
    {
        m_previewTarget = handle;
        m_previewDirty = true;

        // Lazy create the preview scene
        if (!m_rtt && m_context)
        {
            // TODO: Create preview scene with Octree + Zone + Skybox + directional light + sphere
            // For now, just mark dirty
        }
    }

    bool RbfxMaterialSource::SetPreviewModel(PreviewModel model)
    {
        // v1: always use sphere. No model switching implemented yet.
        (void)model;
        return false;
    }

    PreviewResult RbfxMaterialSource::AcquirePreviewImage(
        [[maybe_unused]] uint32_t width, [[maybe_unused]] uint32_t height, [[maybe_unused]] AZStd::vector<AZ::u8>& outPixels)
    {
        // Preview readback is not yet wired; stay explicitly unsupported so the idle loop
        // clears the dirty flag immediately instead of spinning at 10 Hz.
        if (m_previewTarget != 0)
        {
            m_previewDirty = false;
        }

        return PreviewResult::Unsupported;
    }
} // namespace CrossEngineEditor
