/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include "FilamentMaterialSource.h"

#include <MaterialEditor/MaterialSchema.h>

#include <AzCore/IO/SystemFile.h>
#include <AzCore/Math/Color.h>
#include <AzCore/Math/Matrix3x3.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/Utils/Utils.h>

#include <AzCore/std/algorithm.h>

// Filament headers - macro-heavy, kept out of unity builds by the CMake rule. DESKTOP is
// parked because rbfx's public Urho3D define collides with ShaderModel::DESKTOP (see
// FilamentBackend.h for the full note).
#pragma push_macro("DESKTOP")
#undef DESKTOP
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
AZ_PUSH_DISABLE_WARNING(4100 4244 4267 4819, "-Wunknown-warning-option")

#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Renderer.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <filament/Camera.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>
#include <filament/LightManager.h>
#include <filament/TransformManager.h>
#include <utils/EntityManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>

#include <backend/PixelBufferDescriptor.h>

#include <cee_filamat.h> // CEE_FILAMAT_CEE_LIT_OPAQUE_DATA/SIZE

AZ_POP_DISABLE_WARNING
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#pragma pop_macro("DESKTOP")

#include <stb_image.h>

#include <cmath>
#include <cstring>

namespace CrossEngineEditor
{
    namespace
    {
        constexpr const char* k_logTag = "CrossEngineEditor";
        constexpr uint32_t k_previewSize = 512;

        //! Loads a PNG/JPEG file into an sRGB RGBA8 filament texture, or null.
        filament::Texture* LoadTextureFromFile(filament::Engine& engine, const AZStd::string& path)
        {
            if (path.empty())
            {
                return nullptr;
            }
            int w = 0, h = 0, comp = 0;
            stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
            if (!pixels || w <= 0 || h <= 0)
            {
                AZ_Warning(k_logTag, false, "filament material: could not load texture %s.", path.c_str());
                return nullptr;
            }
            const size_t size = static_cast<size_t>(w) * h * 4;
            filament::Texture* texture = filament::Texture::Builder()
                .width(static_cast<uint32_t>(w))
                .height(static_cast<uint32_t>(h))
                .levels(1)
                .sampler(filament::Texture::Sampler::SAMPLER_2D)
                .format(filament::Texture::InternalFormat::SRGB8_A8) // baseColor is sRGB
                .build(engine);
            texture->setImage(engine, 0,
                filament::Texture::PixelBufferDescriptor(
                    pixels, size, filament::backend::PixelDataFormat::RGBA,
                    filament::backend::PixelDataType::UBYTE,
                    [](void* p, size_t, void*) { stbi_image_free(p); }));
            return texture;
        }

        //! 1x1 opaque-white texture used as the "unset" sampler binding.
        filament::Texture* CreateWhiteTexture(filament::Engine& engine)
        {
            auto* texture = filament::Texture::Builder()
                .width(1)
                .height(1)
                .levels(1)
                .sampler(filament::Texture::Sampler::SAMPLER_2D)
                .format(filament::Texture::InternalFormat::SRGB8_A8)
                .build(engine);
            // The descriptor's bytes are consumed asynchronously: hand over a heap block.
            auto* white = new uint32_t[1]{ 0xFFFFFFFFu };
            texture->setImage(engine, 0,
                filament::Texture::PixelBufferDescriptor(
                    white, sizeof(uint32_t), filament::backend::PixelDataFormat::RGBA,
                    filament::backend::PixelDataType::UBYTE,
                    [](void* p, size_t, void*) { delete[] static_cast<uint32_t*>(p); }));
            return texture;
        }
    } // namespace

    //! Preview scene: own Scene/View/Camera + RenderTarget + sphere + light, all offscreen.
    struct FilamentPreviewScene
    {
        filament::Scene* scene = nullptr;
        filament::View* view = nullptr;
        filament::Camera* camera = nullptr;
        utils::Entity cameraEntity;
        filament::RenderTarget* renderTarget = nullptr;
        filament::Texture* color = nullptr;
        filament::Texture* depth = nullptr;
        utils::Entity sphere{};
        filament::VertexBuffer* sphereVB = nullptr;
        filament::IndexBuffer* sphereIB = nullptr;
        utils::Entity light{};
        bool built = false;
    };

    // =====================================================================
    // Lifecycle + schema
    // =====================================================================

    FilamentMaterialSource::FilamentMaterialSource(filament::Engine** engine, const bool* surfaceReady)
        : m_engine(engine)
        , m_surfaceReady(surfaceReady)
    {
        LoadSchema();
    }

    FilamentMaterialSource::~FilamentMaterialSource()
    {
        DestroyPreviewScene();
        if (*m_engine)
        {
            for (auto& [handle, entry] : m_materials)
            {
                if (entry->m_instance)
                {
                    (*m_engine)->destroy(entry->m_instance);
                }
                if (entry->m_texture)
                {
                    (*m_engine)->destroy(entry->m_texture);
                }
            }
            m_materials.clear();
            if (m_whiteTexture)
            {
                (*m_engine)->destroy(m_whiteTexture);
                m_whiteTexture = nullptr;
            }
            if (m_template)
            {
                (*m_engine)->destroy(m_template);
                m_template = nullptr;
            }
        }
    }

    void FilamentMaterialSource::LoadSchema()
    {
        // Curated schema JSON (plan §10.2), cross-validated against the compiled
        // Material::getParameters() the first time the template is built.
        const AZ::IO::FixedMaxPath projectRoot = AZ::Utils::GetProjectPath();
        const AZStd::string fullPath =
            (projectRoot / "Gems/CrossEngineEditor/TestAssets/MaterialTypes/filament/LitOpaque.json").String();
        if (!MaterialSchema::LoadFromFile(fullPath, m_schema))
        {
            AZ_Error(k_logTag, false, "Failed to load filament schema from '%s'", fullPath.c_str());
        }
    }

    filament::Material* FilamentMaterialSource::GetTemplateMaterial()
    {
        filament::Engine* engine = *m_engine;
        if (!engine)
        {
            return nullptr;
        }
        if (!m_template)
        {
            m_template = filament::Material::Builder()
                .package(CEE_FILAMAT_CEE_LIT_OPAQUE_DATA, CEE_FILAMAT_CEE_LIT_OPAQUE_SIZE)
                .build(*engine);

            // Cross-validation (M3): hide curated schema entries the .filamat does not
            // actually declare, instead of silently writing dead parameters.
            for (auto& group : m_schema.m_groups)
            {
                for (auto& prop : group.m_properties)
                {
                    if (!m_template->hasParameter(prop.m_id.c_str()))
                    {
                        AZ_Warning(k_logTag, false,
                            "filament material: schema property '%s' not in cee_lit_opaque; hidden.",
                            prop.m_id.c_str());
                        prop.m_visible = false;
                    }
                }
            }
        }
        return m_template;
    }

    // =====================================================================
    // IMaterialSource
    // =====================================================================

    void FilamentMaterialSource::EnumerateMaterialTypes(AZStd::vector<MaterialTypeInfo>& out)
    {
        MaterialTypeInfo info;
        info.m_id = "LitOpaque";
        info.m_displayName = "Lit Opaque";
        info.m_description = "Filament lit opaque PBR material";
        info.m_fileExtensions = { "fmat.json" };
        out.push_back(AZStd::move(info));
    }

    MaterialHandle FilamentMaterialSource::CreateMaterial(AZStd::string_view typeId)
    {
        if (typeId != "LitOpaque")
        {
            AZ_Warning(k_logTag, false, "filament material: unknown type '%.*s'.",
                static_cast<int>(typeId.size()), typeId.data());
            return 0;
        }
        filament::Material* material = GetTemplateMaterial();
        filament::Engine* engine = *m_engine;
        if (!material || !engine)
        {
            return 0;
        }

        auto entry = AZStd::make_unique<MaterialEntry>();
        entry->m_instance = material->createInstance();
        const MaterialHandle handle = m_nextHandle++;
        m_materials[handle] = AZStd::move(entry);
        // Fill the live instance (and the value map) with the schema defaults.
        for (const auto& group : m_schema.m_groups)
        {
            for (const auto& prop : group.m_properties)
            {
                (void)SetPropertyValue(handle, prop.m_id, prop.m_defaultValue);
            }
        }
        return handle;
    }

    MaterialHandle FilamentMaterialSource::LoadMaterial(AZStd::string_view absolutePath)
    {
        MaterialTypeDesc schema;
        MaterialPropertyValueMap values;
        if (!MaterialSchema::LoadFromFile(AZStd::string(absolutePath), schema, values))
        {
            return 0;
        }
        const MaterialHandle handle = CreateMaterial(schema.m_id);
        if (!handle)
        {
            return 0;
        }
        auto& entry = *m_materials[handle];
        entry.m_sourcePath = AZStd::string(absolutePath);
        for (const auto& [id, value] : values)
        {
            (void)SetPropertyValue(handle, id, value);
        }
        return handle;
    }

    void FilamentMaterialSource::DestroyMaterial(MaterialHandle handle)
    {
        if (handle == m_previewTarget)
        {
            m_previewTarget = 0;
            // The sphere's bound instance is about to die: rebind the template default so the
            // (persistent) preview sphere never dangles. The next SetPreviewMaterial rebinds it.
            if (m_preview && m_preview->built && !m_preview->sphere.isNull() && *m_engine)
            {
                auto& rm = (*m_engine)->getRenderableManager();
                if (auto inst = rm.getInstance(m_preview->sphere); inst.isValid())
                {
                    if (filament::Material* material = GetTemplateMaterial())
                    {
                        rm.setMaterialInstanceAt(inst, 0, material->getDefaultInstance());
                    }
                }
            }
        }
        const auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return;
        }
        if (*m_engine)
        {
            if (it->second->m_instance)
            {
                (*m_engine)->destroy(it->second->m_instance);
            }
            if (it->second->m_texture)
            {
                (*m_engine)->destroy(it->second->m_texture);
            }
        }
        m_materials.erase(it);
    }

    bool FilamentMaterialSource::GetMaterialState(
        MaterialHandle handle, MaterialTypeDesc& outSchema, MaterialPropertyValueMap& outValues)
    {
        const auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return false;
        }
        outSchema = m_schema;
        outValues = it->second->m_values;
        return true;
    }

    SetResult FilamentMaterialSource::SetPropertyValue(
        MaterialHandle handle, AZStd::string_view propertyId, const MaterialPropertyValue& value)
    {
        const auto it = m_materials.find(handle);
        if (it == m_materials.end() || !it->second->m_instance || !*m_engine)
        {
            return SetResult::Failed;
        }
        MaterialEntry& entry = *it->second;
        filament::MaterialInstance* mi = entry.m_instance;
        const AZStd::string id(propertyId);

        if (id == "baseColor")
        {
            if (auto* c = AZStd::get_if<AZ::Color>(&value))
            {
                mi->setParameter("baseColor", filament::RgbType::LINEAR,
                    filament::math::float3(c->GetR(), c->GetG(), c->GetB()));
            }
            else
            {
                return SetResult::Failed;
            }
        }
        else if (id == "metallic" || id == "roughness")
        {
            if (auto* f = AZStd::get_if<float>(&value))
            {
                mi->setParameter(id.c_str(), *f);
            }
            else
            {
                return SetResult::Failed;
            }
        }
        else if (id == "baseColorMap")
        {
            if (auto* s = AZStd::get_if<AZStd::string>(&value))
            {
                filament::Texture* texture = LoadTextureFromFile(**m_engine, *s);
                filament::Texture* bound = texture ? texture : GetPreviewWhite();
                static const filament::TextureSampler sampler(
                    filament::TextureSampler::MinFilter::LINEAR,
                    filament::TextureSampler::MagFilter::LINEAR);
                mi->setParameter("baseColorMap", bound, sampler);
                // The entry owns its loaded texture; replace after rebind, then retire the old
                // one (Engine::destroy defers until the GPU is done with it).
                if (entry.m_texture)
                {
                    (*m_engine)->destroy(entry.m_texture);
                }
                entry.m_texture = texture;
            }
            else
            {
                return SetResult::Failed;
            }
        }
        else
        {
            return SetResult::Failed;
        }

        entry.m_values[id] = value;
        if (handle == m_previewTarget)
        {
            m_previewRenderRequested = true; // Re-render the preview next editor frame.
            m_pixelsReady = false;
        }
        return SetResult::Applied;
    }

    bool FilamentMaterialSource::SaveMaterial(MaterialHandle handle, AZStd::string_view absolutePath)
    {
        const auto it = m_materials.find(handle);
        if (it == m_materials.end())
        {
            return false;
        }
        return MaterialSchema::SaveToFile(
            AZStd::string(absolutePath), m_schema, it->second->m_values);
    }

    // =====================================================================
    // Preview (offscreen RenderTarget + async readPixels, plan §10.3)
    // =====================================================================

    void FilamentMaterialSource::BuildPreviewScene()
    {
        filament::Engine* engine = *m_engine;
        if (!engine || m_preview->built)
        {
            return;
        }

        // Color attachment needs BLIT_SRC for readPixels (E7).
        m_preview->color = filament::Texture::Builder()
            .width(k_previewSize).height(k_previewSize).levels(1)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .usage(filament::Texture::Usage::COLOR_ATTACHMENT | filament::Texture::Usage::BLIT_SRC)
            .format(filament::Texture::InternalFormat::RGBA8)
            .build(*engine);
        m_preview->depth = filament::Texture::Builder()
            .width(k_previewSize).height(k_previewSize).levels(1)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .usage(filament::Texture::Usage::DEPTH_ATTACHMENT)
            .format(filament::Texture::InternalFormat::DEPTH24)
            .build(*engine);
        m_preview->renderTarget = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR, m_preview->color)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, m_preview->depth)
            .build(*engine);

        m_preview->scene = engine->createScene();
        m_preview->cameraEntity = utils::EntityManager::get().create();
        m_preview->camera = engine->createCamera(m_preview->cameraEntity);
        m_preview->camera->setExposure(16.0f, 1.0f / 125.0f, 100.0f);
        m_preview->camera->lookAt(
            filament::math::double3{0.0f, 0.0f, 2.2f},
            filament::math::double3{0.0f, 0.0f, 0.0f},
            filament::math::double3{0.0f, 1.0f, 0.0f});
        m_preview->camera->setProjection(35.0, 1.0, 0.1, 10.0);

        m_preview->view = engine->createView();
        m_preview->view->setScene(m_preview->scene);
        m_preview->view->setCamera(m_preview->camera);
        m_preview->view->setViewport(filament::Viewport(0, 0, k_previewSize, k_previewSize));
        m_preview->view->setRenderTarget(m_preview->renderTarget);

        // Three-point-ish lighting without an IBL: key directional + fill directional.
        m_preview->light = utils::EntityManager::get().create();
        filament::LightManager::Builder(filament::LightManager::Type::SUN)
            .color({0.98f, 0.98f, 0.95f})
            .intensity(110000.0f)
            .direction({-0.6f, -1.0f, -0.5f})
            .build(*engine, m_preview->light);
        m_preview->scene->addEntity(m_preview->light);

        m_preview->built = true;
    }

    void FilamentMaterialSource::DestroyPreviewScene()
    {
        filament::Engine* engine = *m_engine;
        if (!engine || !m_preview || !m_preview->built)
        {
            return;
        }
        engine->destroy(m_preview->view);
        engine->destroy(m_preview->scene);
        engine->destroy(m_preview->cameraEntity);
        if (m_preview->sphere)
        {
            engine->destroy(m_preview->sphere);
            m_preview->sphere.clear();
        }
        if (m_preview->sphereVB)
        {
            engine->destroy(m_preview->sphereVB);
            m_preview->sphereVB = nullptr;
        }
        if (m_preview->sphereIB)
        {
            engine->destroy(m_preview->sphereIB);
            m_preview->sphereIB = nullptr;
        }
        engine->destroy(m_preview->light);
        engine->destroy(m_preview->renderTarget);
        engine->destroy(m_preview->color);
        engine->destroy(m_preview->depth);
        m_preview->built = false;
    }

    filament::Texture* FilamentMaterialSource::GetPreviewWhite()
    {
        filament::Engine* engine = *m_engine;
        if (!engine)
        {
            return nullptr;
        }
        if (!m_whiteTexture)
        {
            m_whiteTexture = CreateWhiteTexture(*engine);
        }
        return m_whiteTexture;
    }

    void FilamentMaterialSource::SetPreviewMaterial(MaterialHandle handle)
    {
        m_previewTarget = handle;
        m_pixelsReady = false;
        m_previewRenderRequested = true;
        if (handle == 0)
        {
            return;
        }
        filament::Engine* engine = *m_engine;
        const auto it = m_materials.find(handle);
        if (!engine || it == m_materials.end())
        {
            return;
        }
        if (!m_preview)
        {
            m_preview = AZStd::make_unique<FilamentPreviewScene>();
        }
        BuildPreviewScene();
        if (!m_preview->built)
        {
            return;
        }

        if (m_preview->sphere.isNull())
        {
            m_preview->sphere = CreatePreviewSphere(*engine);
            if (m_preview->sphere)
            {
                m_preview->scene->addEntity(m_preview->sphere);
            }
        }
        auto& rm = engine->getRenderableManager();
        if (auto inst = rm.getInstance(m_preview->sphere); inst.isValid())
        {
            rm.setMaterialInstanceAt(inst, 0, it->second->m_instance);
        }
    }

    void FilamentMaterialSource::RenderPreviewFrame(filament::Renderer& renderer)
    {
        if (!m_previewRenderRequested || !m_preview || !m_preview->built || !*m_surfaceReady)
        {
            return;
        }
        m_previewRenderRequested = false;

        renderer.render(m_preview->view);

        // Async readback: the callback fires a few frames later on the main thread
        // (Renderer.h:562-564), which is exactly the panel's Unchanged -> Updated cadence.
        auto* heap = new uint8_t[k_previewSize * k_previewSize * 4];
        renderer.readPixels(m_preview->renderTarget, 0, 0, k_previewSize, k_previewSize,
            filament::backend::PixelBufferDescriptor(
                heap, static_cast<size_t>(k_previewSize) * k_previewSize * 4,
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                [](void* buffer, size_t size, void* user)
                {
                    auto* self = static_cast<FilamentMaterialSource*>(user);
                    const auto* src = static_cast<const uint8_t*>(buffer);
                    self->m_pixels.assign(src, src + size);
                    // GL readback is bottom-up; the panel wants top-down rows (L6).
                    const uint32_t rowBytes = k_previewSize * 4;
                    for (uint32_t y = 0; y < k_previewSize / 2; ++y)
                    {
                        auto* a = self->m_pixels.data() + y * rowBytes;
                        auto* b = self->m_pixels.data() + (k_previewSize - 1 - y) * rowBytes;
                        for (uint32_t i = 0; i < rowBytes; ++i)
                        {
                            AZStd::swap(a[i], b[i]);
                        }
                    }
                    self->m_pixelsReady = true;
                    delete[] static_cast<uint8_t*>(buffer);
                },
                this));
    }

    PreviewResult FilamentMaterialSource::AcquirePreviewImage(
        uint32_t width, uint32_t height, AZStd::vector<AZ::u8>& outPixels)
    {
        if (!m_preview || !m_preview->built || !*m_engine)
        {
            return PreviewResult::Unsupported;
        }
        if (!m_pixelsReady)
        {
            return PreviewResult::Unchanged;
        }
        const size_t needed = static_cast<size_t>(width) * height * 4;
        if (width != k_previewSize || height != k_previewSize || m_pixels.size() != needed)
        {
            return PreviewResult::Unsupported; // Panel requests the fixed 512x512 it gets.
        }
        outPixels = m_pixels;
        m_pixelsReady = false;
        return PreviewResult::Updated;
    }

    // =====================================================================
    // Preview sphere: lat-long UV sphere with analytic tangents
    // =====================================================================

    utils::Entity FilamentMaterialSource::CreatePreviewSphere(filament::Engine& engine)
    {
        constexpr int kSegments = 48;
        constexpr int kRings = 32;
        constexpr float kRadius = 1.0f;

        const size_t vertexCount = static_cast<size_t>(kSegments + 1) * (kRings + 1);
        AZStd::vector<float> positions(vertexCount * 3);
        AZStd::vector<float> tangents(vertexCount * 4); //!< TBN quaternions (FLOAT4).
        AZStd::vector<float> uvs(vertexCount * 2);
        AZStd::vector<uint16_t> indices;

        for (int r = 0; r <= kRings; ++r)
        {
            const float theta = AZ::Constants::Pi * static_cast<float>(r) / kRings; // Polar angle.
            const float sinT = std::sin(theta), cosT = std::cos(theta);
            for (int s = 0; s <= kSegments; ++s)
            {
                const float phi = 2.0f * AZ::Constants::Pi * static_cast<float>(s) / kSegments;
                const float sinP = std::sin(phi), cosP = std::cos(phi);
                const size_t v = static_cast<size_t>(r) * (kSegments + 1) + s;

                const AZ::Vector3 normal(sinT * cosP, cosT, sinT * sinP);
                // dP/dphi = (-sinP, 0, cosP): the analytic tangent along +u.
                const AZ::Vector3 tangent(-sinP, 0.0f, cosP);
                const AZ::Vector3 bitangent = normal.Cross(tangent);

                positions[v * 3 + 0] = kRadius * normal.GetX();
                positions[v * 3 + 1] = kRadius * normal.GetY();
                positions[v * 3 + 2] = kRadius * normal.GetZ();

                // TANGENTS is a quaternion mapping the identity basis onto (tangent,
                // bitangent, normal) - the rotation with those basis columns (VertexBuffer.h:127).
                AZ::Matrix3x3 tbn = AZ::Matrix3x3::CreateIdentity();
                tbn.SetRow(0, tangent);
                tbn.SetRow(1, bitangent);
                tbn.SetRow(2, normal);
                const AZ::Quaternion q =
                    AZ::Quaternion::CreateFromMatrix3x3(tbn.GetTranspose()).GetNormalized();
                tangents[v * 4 + 0] = q.GetX();
                tangents[v * 4 + 1] = q.GetY();
                tangents[v * 4 + 2] = q.GetZ();
                tangents[v * 4 + 3] = q.GetW();

                uvs[v * 2 + 0] = static_cast<float>(s) / kSegments;
                uvs[v * 2 + 1] = 1.0f - static_cast<float>(r) / kRings;
            }
        }
        for (int r = 0; r < kRings; ++r)
        {
            for (int s = 0; s < kSegments; ++s)
            {
                const uint16_t a = static_cast<uint16_t>(r * (kSegments + 1) + s);
                const uint16_t b = a + 1;
                const uint16_t c = static_cast<uint16_t>(a + kSegments + 1);
                const uint16_t d = c + 1;
                indices.push_back(a);
                indices.push_back(c);
                indices.push_back(b);
                indices.push_back(b);
                indices.push_back(c);
                indices.push_back(d);
            }
        }

        auto* vb = filament::VertexBuffer::Builder()
            .vertexCount(static_cast<uint32_t>(vertexCount))
            .bufferCount(3)
            .attribute(filament::VertexAttribute::POSITION, 0,
                filament::VertexBuffer::AttributeType::FLOAT3)
            .attribute(filament::VertexAttribute::TANGENTS, 1,
                filament::VertexBuffer::AttributeType::FLOAT4)
            .attribute(filament::VertexAttribute::UV0, 2,
                filament::VertexBuffer::AttributeType::FLOAT2)
            .build(engine);
        auto* ib = filament::IndexBuffer::Builder()
            .indexCount(static_cast<uint32_t>(indices.size()))
            .bufferType(filament::IndexBuffer::IndexType::USHORT)
            .build(engine);

        // The vertex data vectors are standard-library heap blocks; filament consumes them
        // asynchronously, so hand over copies with delete-callbacks (same rule as the overlay).
        using filament::backend::BufferDescriptor;
        auto heapCopy = [](const void* data, size_t size) -> BufferDescriptor
        {
            auto* heap = new uint8_t[size];
            memcpy(heap, data, size);
            return BufferDescriptor(heap, size,
                [](void* p, size_t, void*) { delete[] static_cast<uint8_t*>(p); });
        };
        vb->setBufferAt(engine, 0, heapCopy(positions.data(), positions.size() * sizeof(float)));
        vb->setBufferAt(engine, 1, heapCopy(tangents.data(), tangents.size() * sizeof(float)));
        vb->setBufferAt(engine, 2, heapCopy(uvs.data(), uvs.size() * sizeof(float)));
        ib->setBuffer(engine, heapCopy(indices.data(), indices.size() * sizeof(uint16_t)));

        filament::Material* material = GetTemplateMaterial();
        if (!material)
        {
            engine.destroy(vb);
            engine.destroy(ib);
            return {};
        }
        const utils::Entity entity = utils::EntityManager::get().create();
        filament::RenderableManager::Builder(1)
            .boundingBox(filament::Box{ .center = {0, 0, 0},
                                       .halfExtent = {kRadius, kRadius, kRadius} })
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, vb, ib, 0,
                static_cast<uint32_t>(indices.size()))
            .material(0, material->getDefaultInstance())
            .culling(false)
            .build(engine, entity);
        // Engine::destroy(Entity) drops the component but not the buffers; keep them on the
        // preview scene so DestroyPreviewScene can retire them.
        m_preview->sphereVB = vb;
        m_preview->sphereIB = ib;
        return entity;
    }
} // namespace CrossEngineEditor
