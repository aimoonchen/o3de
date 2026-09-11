/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include "FilamentBackend.h"

#include <Framework/EngineNodeComponent.h>
#include <MaterialEditor/FilamentMaterialSource.h>

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/Math/Aabb.h>
#include <AzCore/Math/IntersectSegment.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/Math/Matrix3x3.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/functional.h>
#include <AzCore/std/sort.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

// cgltf_write implementation - define in exactly one TU. Its fopen() trips C4996 under
// /WX (third-party CRT choice), so park that warning for the include.
#pragma warning(push)
#pragma warning(disable : 4996)
#define CGLTF_WRITE_IMPLEMENTATION
#include <cgltf_write.h>
#pragma warning(pop)

// resgen-embedded blobs (CEE .mat pipeline in External/CMakeLists.txt; prefix cee_filamat).
#include <cee_filamat.h>             // CEE_FILAMAT_CEE_OVERLAY_DATA/SIZE etc.
#include <materials/uberarchive.h>   // UBERARCHIVE_DEFAULT_DATA/SIZE

#include <cmath>
#include <cstring>
#include <limits>

namespace CrossEngineEditor
{
    namespace
    {
        constexpr EngineSpace k_space = EngineSpace::Filament; //!< Y-up RH, -Z forward (as Godot).
        constexpr const char* k_logTag = "CrossEngineEditor";

        //! High bit marking a mirror node handle as a created-node id (not a cgltf index).
        constexpr AZ::u64 k_createdHandleBit = 0x8000'0000'0000'0000ull;

        //! Column-major cgltf matrix -> AZ::Matrix4x4. Same memory interpretation, no transpose:
        //! a column-vector operator M equals the row-vector operator M^T, which is exactly the
        //! row-major read of the same 16 floats (translation ends up in the last AZ row).
        AZ::Matrix4x4 MatrixFromCgltf(const cgltf_float m[16])
        {
            AZ::Matrix4x4 out;
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    out.SetElement(r, c, m[r * 4 + c]);
                }
            }
            return out;
        }

        //! Row-vector point transform (v * M), matching MatrixFromCgltf's convention.
        AZ::Vector3 TransformPointRow(const AZ::Matrix4x4& m, const AZ::Vector3& p)
        {
            return AZ::Vector3(
                p.GetX() * m.GetElement(0, 0) + p.GetY() * m.GetElement(1, 0) + p.GetZ() * m.GetElement(2, 0) + m.GetElement(3, 0),
                p.GetX() * m.GetElement(0, 1) + p.GetY() * m.GetElement(1, 1) + p.GetZ() * m.GetElement(2, 1) + m.GetElement(3, 1),
                p.GetX() * m.GetElement(0, 2) + p.GetY() * m.GetElement(1, 2) + p.GetZ() * m.GetElement(2, 2) + m.GetElement(3, 2));
        }

        //! Extract rotation/scale from a MatrixFromCgltf result. Its AZ rows are the true
        //! matrix's basis vectors (the read transposes), and AZ stores basis images in
        //! COLUMNS (Matrix3x3: GetBasisX == GetColumn(0)) - so the basis must go into
        //! columns, else the extracted quaternion is the inverse rotation.
        AZ::Quaternion RotationFromCgltfMatrix(
            const AZ::Matrix4x4& m, float& outUniformScale)
        {
            const AZ::Vector3 basisX(m.GetElement(0, 0), m.GetElement(0, 1), m.GetElement(0, 2));
            const AZ::Vector3 basisY(m.GetElement(1, 0), m.GetElement(1, 1), m.GetElement(1, 2));
            const AZ::Vector3 basisZ(m.GetElement(2, 0), m.GetElement(2, 1), m.GetElement(2, 2));
            outUniformScale = AZStd::max(AZStd::max(basisX.GetLength(), basisY.GetLength()), basisZ.GetLength());
            AZ::Matrix3x3 rot = AZ::Matrix3x3::CreateIdentity();
            if (outUniformScale > 0.00001f)
            {
                rot.SetColumn(0, basisX / outUniformScale);
                rot.SetColumn(1, basisY / outUniformScale);
                rot.SetColumn(2, basisZ / outUniformScale);
            }
            return AZ::Quaternion::CreateFromMatrix3x3(rot).GetNormalized();
        }

        //! Engine-space local mat4 for a cgltf node, for TransformManager::setTransform.
        filament::math::mat4f EngineLocalMatrix(const cgltf_node* node)
        {
            cgltf_float lm[16];
            cgltf_node_transform_local(node, lm);
            using filament::math::float4;
            using filament::math::mat4f;
            return mat4f(
                float4(lm[0], lm[1], lm[2], lm[3]),
                float4(lm[4], lm[5], lm[6], lm[7]),
                float4(lm[8], lm[9], lm[10], lm[11]),
                float4(lm[12], lm[13], lm[14], lm[15]));
        }

        //! World AABB of one cgltf node's own mesh (accessor min/max x world matrix), O3DE space.
        AZ::Aabb WorldBoundsOfNode(const cgltf_node* node)
        {
            if (!node || !node->mesh)
            {
                return AZ::Aabb::CreateNull();
            }
            AZ::Aabb engineBox = AZ::Aabb::CreateNull();
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p)
            {
                for (cgltf_size a = 0; a < node->mesh->primitives[p].attributes_count; ++a)
                {
                    const cgltf_attribute& attr = node->mesh->primitives[p].attributes[a];
                    if (attr.type != cgltf_attribute_type_position)
                    {
                        continue;
                    }
                    const cgltf_accessor* acc = attr.data;
                    if (acc->has_min && acc->has_max)
                    {
                        engineBox.AddPoint(AZ::Vector3(acc->min[0], acc->min[1], acc->min[2]));
                        engineBox.AddPoint(AZ::Vector3(acc->max[0], acc->max[1], acc->max[2]));
                    }
                    break;
                }
            }
            if (!engineBox.IsValid())
            {
                return AZ::Aabb::CreateNull();
            }
            cgltf_float wm[16];
            cgltf_node_transform_world(node, wm);
            const AZ::Matrix4x4 world = MatrixFromCgltf(wm);
            const AZ::Vector3 lo = engineBox.GetMin();
            const AZ::Vector3 hi = engineBox.GetMax();
            AZ::Aabb out = AZ::Aabb::CreateNull();
            for (int corner = 0; corner < 8; ++corner)
            {
                out.AddPoint(TransformPointRow(world, AZ::Vector3(
                    (corner & 1) ? hi.GetX() : lo.GetX(),
                    (corner & 2) ? hi.GetY() : lo.GetY(),
                    (corner & 4) ? hi.GetZ() : lo.GetZ())));
            }
            return EngineTransformConverter::ConvertAabb(k_space, out);
        }

        //! Push curated light values to a render entity's LightManager component (shared by
        //! the document and created-node edit paths). Units follow glTF: directional intensity
        //! = lux, point/spot = candela; range 0 = glTF "infinite", for which gltfio substitutes
        //! the heuristic falloff (I/0.05)^(1/4) - mirrored from AssetLoader.cpp:1361-1387 so
        //! edit-time matches load-time.
        void ApplyLightToRender(filament::Engine& engine, utils::Entity entity,
            cgltf_light_type type, const float color[3], float intensity, float range,
            float innerCone, float outerCone)
        {
            auto& lm = engine.getLightManager();
            const auto inst = lm.getInstance(entity);
            if (!inst.isValid())
            {
                return;
            }
            lm.setColor(inst, filament::LinearColor{ color[0], color[1], color[2] });
            if (type == cgltf_light_type_directional)
            {
                lm.setIntensity(inst, intensity);
            }
            else
            {
                lm.setIntensityCandela(inst, intensity);
            }
            lm.setFalloff(inst, range > 0.0f ? range : std::pow(intensity / 0.05f, 0.25f));
            if (type == cgltf_light_type_spot)
            {
                lm.setSpotLightCone(inst, innerCone, outerCone);
            }
        }

        //! Copy bytes into a heap block released after the GPU consumed the descriptor
        //! (BufferDescriptor points at memory the caller must keep alive otherwise).
        filament::backend::BufferDescriptor HeapDescriptor(const void* data, size_t size)
        {
            auto* heap = new uint8_t[size];
            memcpy(heap, data, size);
            return filament::backend::BufferDescriptor(
                heap, size,
                [](void* buffer, size_t, void*) { delete[] static_cast<uint8_t*>(buffer); });
        }

        //! Curated light property names (plan §6.4) - shared by mirror read and edit write-back.
        constexpr const char* k_propColor = "Color";
        constexpr const char* k_propIntensity = "Intensity";
        constexpr const char* k_propRange = "Range";
        constexpr const char* k_propInnerCone = "Inner Cone";
        constexpr const char* k_propOuterCone = "Outer Cone";
    } // namespace

    // =====================================================================
    // FilamentBackend
    // =====================================================================

    // The material source is built here, not in Initialize: the shell keeps a failed-Init
    // backend alive on empty contracts, and GetMaterialSource() must never dangle. It only
    // holds pointers into m_state, so it needs no live engine.
    FilamentBackend::FilamentBackend()
        : m_materialSource(AZStd::make_unique<FilamentMaterialSource>(&m_state.engine, &m_state.ready))
    {
        m_state.materialSource = m_materialSource.get();
    }
    FilamentBackend::~FilamentBackend() { Shutdown(); }

    std::expected<void, BackendError> FilamentBackend::Initialize(const BackendInitParams& params)
    {
        m_state.scenePath = params.m_scenePath;
        // params.m_projectPath is intentionally not stored (empty is fine, same contract as
        // rbfx/Godot): the asset source resolves the project root from the settings registry
        // at enumeration time.

        // Create the engine up front: WGL builds its own hidden dummy window (E6), so this
        // needs no surface. The swapchain still waits for OnSurfaceCreated.
        m_state.engine = filament::Engine::create();
        if (!m_state.engine)
        {
            return std::unexpected(BackendError::RenderDeviceUnavailable);
        }
        return {};
    }

    void FilamentBackend::Shutdown()
    {
        m_sceneRenderer.Teardown();

        // gltfio teardown order: asset (frees the live cgltf doc) -> loaders -> providers
        // (docs: providers outlive the ResourceLoader that references them).
        if (m_state.assetLoader)
        {
            if (m_state.asset)
            {
                m_state.assetLoader->destroyAsset(m_state.asset);
                m_state.asset = nullptr;
            }
            m_state.cgltfData = nullptr;
            filament::gltfio::AssetLoader::destroy(&m_state.assetLoader);
        }
        delete m_state.resourceLoader;
        m_state.resourceLoader = nullptr;
        if (m_state.materialProvider)
        {
            m_state.materialProvider->destroyMaterials();
            delete m_state.materialProvider;
            m_state.materialProvider = nullptr;
        }
        delete m_state.stbProvider;
        m_state.stbProvider = nullptr;
        delete m_state.ktx2Provider;
        m_state.ktx2Provider = nullptr;
        delete m_state.nameManager;
        m_state.nameManager = nullptr;

        // The material source owns filament objects; drop it before the engine dies.
        m_state.materialSource = nullptr;
        m_materialSource.reset();

        if (m_state.engine)
        {
            filament::Engine::destroy(&m_state.engine);
        }
    }

    void FilamentBackend::Tick(float /*deltaSeconds*/)
    {
        // Rendering is driven by EndOverlayFrame (one present per overlay cycle).
    }

    IMaterialSource& FilamentBackend::GetMaterialSource()
    {
        return *m_materialSource;
    }

    // =====================================================================
    // FilamentSceneRenderer
    // =====================================================================

    void FilamentBackend::FilamentSceneRenderer::OnSurfaceCreated(
        void* nativeWindowHandle, uint32_t w, uint32_t h)
    {
        auto* e = m_s.engine;
        if (!e || m_s.ready || nativeWindowHandle == nullptr)
        {
            return;
        }

        m_s.nativeWindow = nativeWindowHandle;
        m_s.width = AZStd::max(1u, w);
        m_s.height = AZStd::max(1u, h);

        m_s.swapChain = e->createSwapChain(
            nativeWindowHandle, filament::SwapChain::CONFIG_HAS_STENCIL_BUFFER);
        if (m_s.view)
        {
            // Re-entry after a dock-float surface rebuild: only the swapchain binds the
            // HWND. View/scene/camera/renderer, the overlay and the whole gltfio stack
            // (asset + live document) survive untouched, so unsaved edits and created
            // nodes ride through - same scope as OnSurfaceResized (E4).
            m_s.view->setViewport(filament::Viewport(0, 0, m_s.width, m_s.height));
            m_s.ready = true;
            return;
        }

        m_s.renderer = e->createRenderer();
        m_s.scene = e->createScene();

        m_s.cameraEntity = utils::EntityManager::get().create();
        m_s.camera = e->createCamera(m_s.cameraEntity);
        // Filament cameras are physical; use the sample-default exposure so glTF scenes show
        // up at sensible brightness without an IBL (material_sandbox.cpp:187-189 defaults).
        m_s.camera->setExposure(16.0f, 1.0f / 125.0f, 100.0f);

        m_s.view = e->createView();
        m_s.view->setScene(m_s.scene);
        m_s.view->setCamera(m_s.camera);
        m_s.view->setViewport(filament::Viewport(0, 0, m_s.width, m_s.height));
        m_s.view->setBlendMode(filament::View::BlendMode::OPAQUE);

        // gltfio: ubershader provider (AssetLoader dereferences the material provider at
        // construction - F3) + stb/ktx2 texture decoders.
        m_s.materialProvider = filament::gltfio::createUbershaderProvider(
            e, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
        m_s.stbProvider = filament::gltfio::createStbProvider(e);
        m_s.ktx2Provider = filament::gltfio::createKtx2Provider(e);
        m_s.nameManager = new utils::NameComponentManager(utils::EntityManager::get());

        m_s.assetLoader = filament::gltfio::AssetLoader::create(
            { e, m_s.materialProvider, m_s.nameManager });
        // gltfPath is the deprecated-but-only way to resolve external .bin/image URIs
        // (ResourceLoader.cpp:953); it trips C4996 under /WX, so silence it for the field.
        filament::gltfio::ResourceConfiguration resourceConfig{};
        resourceConfig.engine = e;
        resourceConfig.normalizeSkinningWeights = true;
        AZ_PUSH_DISABLE_WARNING(4996, "-Wdeprecated-declarations")
        resourceConfig.gltfPath = m_s.scenePath.empty() ? "" : m_s.scenePath.c_str();
        AZ_POP_DISABLE_WARNING
        m_s.resourceLoader = new filament::gltfio::ResourceLoader(resourceConfig);
        if (m_s.stbProvider)
        {
            m_s.resourceLoader->addTextureProvider("image/png", m_s.stbProvider);
            m_s.resourceLoader->addTextureProvider("image/jpeg", m_s.stbProvider);
        }
        if (m_s.ktx2Provider)
        {
            m_s.resourceLoader->addTextureProvider("image/ktx2", m_s.ktx2Provider);
        }

        CreateOverlayResources();
        LoadSceneAsset();
        m_s.ready = true;
    }

    void FilamentBackend::FilamentSceneRenderer::OnSurfaceResized(uint32_t w, uint32_t h)
    {
        if (!m_s.ready)
        {
            return;
        }
        m_s.width = AZStd::max(1u, w);
        m_s.height = AZStd::max(1u, h);

        // Filament has no resize API - destroy + recreate the swapchain (E4), after pending
        // GPU work has drained (E6).
        auto* e = m_s.engine;
        e->flushAndWait();
        e->destroy(m_s.swapChain);
        m_s.swapChain = e->createSwapChain(
            m_s.nativeWindow, filament::SwapChain::CONFIG_HAS_STENCIL_BUFFER);
        m_s.view->setViewport(filament::Viewport(0, 0, m_s.width, m_s.height));
    }

    void FilamentBackend::FilamentSceneRenderer::OnSurfaceAboutToBeDestroyed()
    {
        if (!m_s.ready)
        {
            return;
        }
        m_s.ready = false;

        // Only the swapchain is surface-bound; everything else survives a dock-float
        // rebuild (see OnSurfaceCreated re-entry). Teardown() owns full destruction.
        m_s.engine->flushAndWait(); // Let in-flight frames finish touching the dying HWND.
        m_s.engine->destroy(m_s.swapChain);
        m_s.swapChain = nullptr;
    }

    void FilamentBackend::FilamentSceneRenderer::Teardown()
    {
        OnSurfaceAboutToBeDestroyed();
        auto* e = m_s.engine;
        if (!e)
        {
            return;
        }
        DestroyOverlayResources();
        if (m_s.view)
        {
            e->destroy(m_s.view);
            m_s.view = nullptr;
        }
        if (m_s.scene)
        {
            e->destroy(m_s.scene);
            m_s.scene = nullptr;
        }
        if (m_s.camera)
        {
            e->destroy(m_s.cameraEntity); // Destroys the Camera component too (Engine.h:1022).
            m_s.camera = nullptr;
            m_s.cameraEntity.clear();
        }
        if (m_s.renderer)
        {
            e->destroy(m_s.renderer);
            m_s.renderer = nullptr;
        }
    }

    void FilamentBackend::FilamentSceneRenderer::BeginOverlayFrame(
        const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip)
    {
        for (auto& batch : m_s.overlayLines)
        {
            batch.positions.clear();
            batch.colors.clear();
            batch.indices.clear();
        }
        for (auto& batch : m_s.overlayTriangles)
        {
            batch.positions.clear();
            batch.colors.clear();
            batch.indices.clear();
        }

        if (!m_s.camera)
        {
            return;
        }

        // Camera: rbfx/Godot pattern (RbfxBackend.cpp:523 / GodotBackend.cpp:644). The camera
        // world transform is the inverse view matrix; the forward fix aligns O3DE's +Y forward
        // with Filament's -Z (same quaternion as Godot - same space), then lookAt + setProjection
        // rebuild the matrices natively. Never memcpy an AZ matrix into a mat4f (row/column +
        // depth convention + basis all differ - plan §5).
        const AZ::Matrix4x4 viewToWorld = worldToView.GetInverseTransform();
        const AZ::Vector3 camPos = viewToWorld.GetTranslation();
        const AZ::Quaternion camRot = AZ::Quaternion::CreateFromMatrix4x4(viewToWorld);
        const AZ::Quaternion camForwardFix =
            AZ::Quaternion::CreateRotationX(AZ::Constants::HalfPi) *
            AZ::Quaternion::CreateRotationY(AZ::Constants::Pi);
        const AZ::Quaternion camRotFixed = camRot * camForwardFix;

        AZ::Vector3 pos;
        AZ::Quaternion rot;
        EngineTransformConverter::TransformToEngine(
            k_space, AZ::Transform::CreateFromQuaternionAndTranslation(camRotFixed, camPos), pos, rot);

        const AZ::Vector3 forward = rot.TransformVector(AZ::Vector3(0.0f, 0.0f, -1.0f));
        const AZ::Vector3 up = rot.TransformVector(AZ::Vector3(0.0f, 1.0f, 0.0f));
        m_s.camera->lookAt(
            filament::math::double3(pos.GetX(), pos.GetY(), pos.GetZ()),
            filament::math::double3(
                pos.GetX() + forward.GetX(), pos.GetY() + forward.GetY(), pos.GetZ() + forward.GetZ()),
            filament::math::double3(up.GetX(), up.GetY(), up.GetZ()));

        // Vertical FOV from the projection element [1][1] = 1/tan(fovY/2) (rbfx same recovery).
        const float clip11 = viewToClip.GetElement(1, 1);
        if (clip11 > 0.0001f)
        {
            const float fovYRadians = 2.0f * std::atan(1.0f / clip11);
            const float aspect = static_cast<float>(m_s.width) / static_cast<float>(AZStd::max(1u, m_s.height));
            m_s.camera->setProjection(AZ::RadToDeg(fovYRadians), aspect, 0.01f, 10000.0f);
        }
    }

    void FilamentBackend::FilamentSceneRenderer::SubmitLines(AZStd::span<const DebugVertex> vertices)
    {
        auto& batch = m_s.overlayLines[m_depthTest ? 0 : 1];
        constexpr size_t kMaxVerts = 16384; // Must fit the uint16 index space of the overlay IBs.
        if (batch.positions.size() / 3 + vertices.size() > kMaxVerts)
        {
            vertices = vertices.first(
                AZStd::min(vertices.size(), kMaxVerts - batch.positions.size() / 3));
        }
        const size_t base = batch.positions.size() / 3;
        for (const auto& v : vertices)
        {
            // Convert O3DE -> engine at the edge, per vertex (F4). AZ::Color::ToU32 packs
            // 0xAABBGGRR = R | G<<8 | B<<16 | A<<24, exactly filament's RGBA byte order.
            const AZ::Vector3 ep = EngineTransformConverter::PositionToEngine(k_space, v.m_position);
            batch.positions.push_back(ep.GetX());
            batch.positions.push_back(ep.GetY());
            batch.positions.push_back(ep.GetZ());
            batch.colors.push_back(v.m_color.ToU32());
        }
        for (size_t i = 0; i + 1 < vertices.size(); i += 2)
        {
            batch.indices.push_back(static_cast<uint16_t>(base + i));
            batch.indices.push_back(static_cast<uint16_t>(base + i + 1));
        }
    }

    void FilamentBackend::FilamentSceneRenderer::SubmitTriangles(AZStd::span<const DebugVertex> vertices)
    {
        auto& batch = m_s.overlayTriangles[m_depthTest ? 0 : 1];
        constexpr size_t kMaxVerts = 16384;
        if (batch.positions.size() / 3 + vertices.size() > kMaxVerts)
        {
            vertices = vertices.first(
                AZStd::min(vertices.size(), kMaxVerts - batch.positions.size() / 3));
        }
        const size_t base = batch.positions.size() / 3;
        for (const auto& v : vertices)
        {
            const AZ::Vector3 ep = EngineTransformConverter::PositionToEngine(k_space, v.m_position);
            batch.positions.push_back(ep.GetX());
            batch.positions.push_back(ep.GetY());
            batch.positions.push_back(ep.GetZ());
            batch.colors.push_back(v.m_color.ToU32());
        }
        for (size_t i = 0; i + 2 < vertices.size(); i += 3)
        {
            batch.indices.push_back(static_cast<uint16_t>(base + i));
            batch.indices.push_back(static_cast<uint16_t>(base + i + 1));
            batch.indices.push_back(static_cast<uint16_t>(base + i + 2));
        }
    }

    void FilamentBackend::FilamentSceneRenderer::SetDepthTest(bool enabled)
    {
        // Submission-time property: it selects the bucket subsequent Submit* calls append to
        // (GenericDebugDisplay submits a depth batch, then an always-on-top batch per frame;
        // rbfx/Diligent interpret the flag the same way).
        m_depthTest = enabled;
    }

    void FilamentBackend::FilamentSceneRenderer::EndOverlayFrame()
    {
        if (!m_s.ready || !m_s.renderer)
        {
            return;
        }
        auto* e = m_s.engine;
        auto& rm = e->getRenderableManager();

        // Upload each batch to its own buffers; an empty batch parks its primitive count at
        // zero so last frame's geometry does not linger (E6).
        auto uploadBatch = [&](EngineState::OverlayBatch& batch,
                               filament::RenderableManager::PrimitiveType type)
        {
            batch.vb->setBufferAt(*e, 0, HeapDescriptor(
                batch.positions.data(), batch.positions.size() * sizeof(float)));
            batch.vb->setBufferAt(*e, 1, HeapDescriptor(
                batch.colors.data(), batch.colors.size() * sizeof(uint32_t)));
            batch.ib->setBuffer(*e, HeapDescriptor(
                batch.indices.data(), batch.indices.size() * sizeof(uint16_t)));
            if (auto inst = rm.getInstance(batch.entity); inst.isValid())
            {
                rm.setGeometryAt(inst, 0, type, batch.vb, batch.ib, 0,
                    static_cast<uint32_t>(batch.indices.size()));
            }
        };
        for (auto& batch : m_s.overlayLines)
        {
            uploadBatch(batch, filament::RenderableManager::PrimitiveType::LINES);
        }
        for (auto& batch : m_s.overlayTriangles)
        {
            uploadBatch(batch, filament::RenderableManager::PrimitiveType::TRIANGLES);
        }

        if (m_s.renderer->beginFrame(m_s.swapChain))
        {
            m_s.renderer->render(m_s.view);
            // Offscreen material preview rides the same frame (plan §10.3) - zero extra
            // presents; its readPixels callback lands a few frames later.
            if (m_s.materialSource)
            {
                m_s.materialSource->RenderPreviewFrame(*m_s.renderer);
            }
            m_s.renderer->endFrame();
        }
    }

    void FilamentBackend::FilamentSceneRenderer::CreateOverlayResources()
    {
        auto* e = m_s.engine;

        // Unlit vertex-color material (Assets/Filament/cee_overlay.mat -> matc -> resgen).
        m_s.overlayMaterial = filament::Material::Builder()
            .package(CEE_FILAMAT_CEE_OVERLAY_DATA, CEE_FILAMAT_CEE_OVERLAY_SIZE)
            .build(*e);
        m_s.overlayMIOn = m_s.overlayMaterial->createInstance();
        m_s.overlayMIOff = m_s.overlayMaterial->createInstance();
        m_s.overlayMIOff->setDepthCulling(false); // Depth-test toggle = instance state (M4).

        constexpr uint32_t kMaxVerts = 16384;
        auto buildBatch = [&](EngineState::OverlayBatch& batch,
                              filament::RenderableManager::PrimitiveType type,
                              uint32_t indexCount, filament::MaterialInstance* material)
        {
            batch.vb = filament::VertexBuffer::Builder()
                .vertexCount(kMaxVerts)
                .bufferCount(2)
                .attribute(filament::VertexAttribute::POSITION, 0,
                    filament::VertexBuffer::AttributeType::FLOAT3)
                .attribute(filament::VertexAttribute::COLOR, 1,
                    filament::VertexBuffer::AttributeType::UBYTE4)
                .normalized(filament::VertexAttribute::COLOR)
                .build(*e);
            batch.ib = filament::IndexBuffer::Builder()
                .indexCount(indexCount)
                .bufferType(filament::IndexBuffer::IndexType::USHORT)
                .build(*e);
            batch.entity = utils::EntityManager::get().create();
            filament::RenderableManager::Builder(1)
                .boundingBox(filament::Box{
                    .center = {0.0f, 0.0f, 0.0f},
                    .halfExtent = {1e6f, 1e6f, 1e6f}}) // Editor overlay: never culled.
                .geometry(0, type, batch.vb, batch.ib, 0, 0)
                .material(0, material)
                .culling(false)
                .priority(7)
                .build(*e, batch.entity);
            m_s.scene->addEntity(batch.entity);
        };
        buildBatch(m_s.overlayLines[0], filament::RenderableManager::PrimitiveType::LINES,
            kMaxVerts * 2, m_s.overlayMIOn);
        buildBatch(m_s.overlayLines[1], filament::RenderableManager::PrimitiveType::LINES,
            kMaxVerts * 2, m_s.overlayMIOff);
        buildBatch(m_s.overlayTriangles[0], filament::RenderableManager::PrimitiveType::TRIANGLES,
            kMaxVerts * 3, m_s.overlayMIOn);
        buildBatch(m_s.overlayTriangles[1], filament::RenderableManager::PrimitiveType::TRIANGLES,
            kMaxVerts * 3, m_s.overlayMIOff);
    }

    void FilamentBackend::FilamentSceneRenderer::DestroyOverlayResources()
    {
        if (!m_s.engine)
        {
            return;
        }
        auto* e = m_s.engine;
        auto& rm = e->getRenderableManager();

        auto destroyBatch = [&](EngineState::OverlayBatch& batch)
        {
            if (rm.getInstance(batch.entity).isValid())
            {
                m_s.scene->remove(batch.entity);
                e->destroy(batch.entity);
            }
            batch.entity.clear();
            if (batch.vb) { e->destroy(batch.vb); batch.vb = nullptr; }
            if (batch.ib) { e->destroy(batch.ib); batch.ib = nullptr; }
        };
        for (auto& batch : m_s.overlayLines) { destroyBatch(batch); }
        for (auto& batch : m_s.overlayTriangles) { destroyBatch(batch); }
        if (m_s.overlayMIOn) { e->destroy(m_s.overlayMIOn); m_s.overlayMIOn = nullptr; }
        if (m_s.overlayMIOff) { e->destroy(m_s.overlayMIOff); m_s.overlayMIOff = nullptr; }
        if (m_s.overlayMaterial) { e->destroy(m_s.overlayMaterial); m_s.overlayMaterial = nullptr; }
    }

    void FilamentBackend::FilamentSceneRenderer::LoadSceneAsset()
    {
        if (m_s.scenePath.empty() || !m_s.assetLoader)
        {
            return;
        }
        AZ::IO::SystemFile file;
        if (!file.Open(m_s.scenePath.c_str(), AZ::IO::SystemFile::SF_OPEN_READ_ONLY))
        {
            AZ_Warning(k_logTag, false, "filament: could not open scene '%s'.", m_s.scenePath.c_str());
            return;
        }
        AZStd::vector<uint8_t> bytes(static_cast<size_t>(file.Length()));
        file.Read(bytes.size(), bytes.data());
        file.Close();

        m_s.asset = m_s.assetLoader->createAsset(bytes.data(), static_cast<uint32_t>(bytes.size()));
        if (!m_s.asset)
        {
            AZ_Warning(k_logTag, false, "filament: gltfio failed to parse '%s'.", m_s.scenePath.c_str());
            return;
        }
        // Synchronous load: an editor opening a level can block (rbfx/Godot do the same).
        if (!m_s.resourceLoader->loadResources(m_s.asset))
        {
            AZ_Warning(k_logTag, false, "filament: some resources of '%s' failed to load.",
                m_s.scenePath.c_str());
        }
        m_s.scene->addEntities(m_s.asset->getEntities(), m_s.asset->getEntityCount());

        // The live cgltf document is the scene truth (plan §6.1). Never call
        // releaseSourceData() - the doc stays resident for editing + saving.
        m_s.cgltfData = const_cast<cgltf_data*>(
    static_cast<const cgltf_data*>(m_s.asset->getSourceAsset()));
        AZ_Printf(k_logTag, "filament: loaded %s (%u nodes)\n", m_s.scenePath.c_str(),
            m_s.cgltfData ? static_cast<unsigned>(m_s.cgltfData->nodes_count) : 0u);
        BuildNodeEntityMap();
    }

    void FilamentBackend::FilamentSceneRenderer::BuildNodeEntityMap()
    {
        if (!m_s.cgltfData || !m_s.asset || !m_s.engine)
        {
            return;
        }
        cgltf_data* data = m_s.cgltfData;
        auto& tm = m_s.engine->getTransformManager();
        auto& rm = m_s.engine->getRenderableManager();
        auto& lm = m_s.engine->getLightManager();

        m_s.nodeEntities.assign(data->nodes_count, utils::Entity{});

        // Root set = default-scene roots + un-scened parentless nodes (the same set gltfio
        // recurses from, AssetLoader.cpp:517-536). Their entities are the children of the
        // asset's root, but gltfio iterates its root set through a robin_map, so the root
        // creation order is arbitrary: match roots to entities by (name, TRS, mesh, light
        // type) signature. Identical signatures are interchangeable nodes.
        AZStd::vector<cgltf_node*> roots;
        AZStd::vector<bool> isRoot(data->nodes_count, false);
        const cgltf_scene* scene = data->scene ? data->scene
            : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
        if (scene)
        {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            {
                const size_t idx = static_cast<size_t>(scene->nodes[i] - data->nodes);
                if (idx < data->nodes_count && !isRoot[idx])
                {
                    isRoot[idx] = true;
                    roots.push_back(scene->nodes[i]);
                }
            }
        }
        for (size_t i = 0; i < data->nodes_count; ++i)
        {
            if (data->nodes[i].parent == nullptr && !isRoot[i])
            {
                isRoot[i] = true;
                roots.push_back(&data->nodes[i]);
            }
        }

        const utils::Entity assetRoot = m_s.asset->getInstance()->getRoot();
        const auto rootInst = tm.getInstance(assetRoot);
        AZStd::vector<utils::Entity> rootEntities(tm.getChildCount(rootInst));
        tm.getChildren(rootInst, rootEntities.data(), rootEntities.size());
        AZStd::vector<bool> taken(rootEntities.size(), false);

        auto lightTypeOf = [&lm](utils::Entity entity) -> int
        {
            const auto inst = lm.getInstance(entity);
            if (!inst.isValid())
            {
                return -1;
            }
            switch (lm.getType(inst))
            {
            case filament::LightManager::Type::SUN:
            case filament::LightManager::Type::DIRECTIONAL: return 0;
            case filament::LightManager::Type::POINT: return 1;
            default: return 2; // spot flavours
            }
        };

        // Recursive association: TransformManager PREPENDS children, so each parent's child
        // list is the cgltf child order reversed (FTransformManager::insertNode).
        AZStd::function<void(cgltf_node*, utils::Entity)> associate =
            [&](cgltf_node* node, utils::Entity entity)
        {
            m_s.nodeEntities[static_cast<size_t>(node - data->nodes)] = entity;
            const size_t count = static_cast<size_t>(node->children_count);
            if (!count)
            {
                return;
            }
            AZStd::vector<utils::Entity> children(tm.getChildCount(tm.getInstance(entity)));
            tm.getChildren(tm.getInstance(entity), children.data(), children.size());
            const size_t n = AZStd::min(count, children.size());
            for (size_t i = 0; i < n; ++i)
            {
                associate(node->children[i], children[count - 1 - i]);
            }
        };

        int unmatched = 0;
        for (cgltf_node* root : roots)
        {
            cgltf_float rootLocal[16];
            cgltf_node_transform_local(root, rootLocal);
            const char* rootName = root->name ? root->name : "";

            bool matched = false;
            for (size_t i = 0; i < rootEntities.size() && !matched; ++i)
            {
                if (taken[i])
                {
                    continue;
                }
                const utils::Entity candidate = rootEntities[i];
                const char* candidateName = m_s.asset->getName(candidate);
                const bool nameOk = (candidateName ? strcmp(candidateName, rootName) == 0
                    : rootName[0] == '\0');
                if (!nameOk || rm.hasComponent(candidate) != (root->mesh != nullptr)
                    || lightTypeOf(candidate) != (root->light
                        ? (root->light->type == cgltf_light_type_directional ? 0
                            : root->light->type == cgltf_light_type_point ? 1 : 2)
                        : -1))
                {
                    continue;
                }
                const filament::math::mat4f& local = tm.getTransform(tm.getInstance(candidate));
                bool transformOk = true;
                for (int e16 = 0; e16 < 16 && transformOk; ++e16)
                {
                    transformOk = std::abs(local[e16 / 4][e16 % 4] - rootLocal[e16]) < 0.001f;
                }
                if (!transformOk)
                {
                    continue;
                }
                taken[i] = true;
                matched = true;
                associate(root, candidate);
            }
            if (!matched)
            {
                ++unmatched;
            }
        }
        AZ_Warning(k_logTag, unmatched == 0,
            "filament: %d root node(s) could not be matched to render entities "
            "(duplicate identical roots); their viewport edits will not drive the renderer.",
            unmatched);
    }

    // =====================================================================
    // FilamentEntityMirror
    // =====================================================================

    void FilamentBackend::FilamentEntityMirror::MirrorNodeRecursive(
        cgltf_node* node, AZ::EntityId parentId, AZStd::vector<AZ::Entity*>& out)
    {
        if (!node || !m_s.cgltfData)
        {
            return;
        }
        const size_t nodeIndex = static_cast<size_t>(node - m_s.cgltfData->nodes);

        // Deleted nodes stay in the doc until save; they are detached from the hierarchy, so
        // the walk cannot reach them. Un-scened bones would be walked as roots here - skip
        // them (they are not part of the default scene the editor edits).
        for (const auto& dead : m_s.deletedNodes)
        {
            if (dead.nodeIndex == nodeIndex)
            {
                return;
            }
        }

        PropertyBag bag;
        const char* className = "Node";
        if (node->light)
        {
            className = node->light->type == cgltf_light_type_directional ? "DirectionalLight"
                : node->light->type == cgltf_light_type_point ? "PointLight" : "SpotLight";
            auto* color = aznew EnginePropertyColor();
            color->m_name = k_propColor;
            color->m_category = "Light";
            color->m_value = AZ::Color(
                node->light->color[0], node->light->color[1], node->light->color[2], 1.0f);
            bag.m_items.push_back(color);

            auto* intensity = aznew EnginePropertyDouble();
            intensity->m_name = k_propIntensity;
            intensity->m_category = "Light";
            intensity->m_value = node->light->intensity;
            bag.m_items.push_back(intensity);

            auto* range = aznew EnginePropertyDouble();
            range->m_name = k_propRange;
            range->m_category = "Light";
            range->m_value = node->light->range;
            bag.m_items.push_back(range);

            if (node->light->type == cgltf_light_type_spot)
            {
                auto* inner = aznew EnginePropertyDouble();
                inner->m_name = k_propInnerCone;
                inner->m_category = "Light";
                inner->m_value = node->light->spot_inner_cone_angle;
                bag.m_items.push_back(inner);
                auto* outer = aznew EnginePropertyDouble();
                outer->m_name = k_propOuterCone;
                outer->m_category = "Light";
                outer->m_value = node->light->spot_outer_cone_angle;
                bag.m_items.push_back(outer);
            }
        }

        AZ::Entity* entity = aznew AZ::Entity(
            node->name && node->name[0] ? node->name : (node->light ? className : "Node"));
        auto* nodeComp = entity->CreateComponent<EngineNodeComponent>();
        nodeComp->SetMirrorData(className, AZStd::move(bag));
        nodeComp->SetNodeHandle(static_cast<AZ::u64>(nodeIndex));

        const AZ::EntityId entityId = entity->GetId();
        m_entityToNodeIndex[entityId] = nodeIndex;
        if (parentId.IsValid())
        {
            m_pendingParent[entityId] = parentId;
        }
        out.push_back(entity);

        for (cgltf_size i = 0; i < node->children_count; ++i)
        {
            MirrorNodeRecursive(node->children[i], entityId, out);
        }
    }

    void FilamentBackend::FilamentEntityMirror::MirrorCreatedNode(
        size_t createdIndex, AZ::EntityId parentId, AZStd::vector<AZ::Entity*>& out)
    {
        const EngineState::CreatedNode& created = m_s.createdNodes[createdIndex];
        const char* className = created.isLight
            ? (created.lightType == cgltf_light_type_directional ? "DirectionalLight"
                : created.lightType == cgltf_light_type_point ? "PointLight" : "SpotLight")
            : "Node";

        PropertyBag bag;
        if (created.isLight)
        {
            auto* color = aznew EnginePropertyColor();
            color->m_name = k_propColor;
            color->m_category = "Light";
            color->m_value = AZ::Color(created.color[0], created.color[1], created.color[2], 1.0f);
            bag.m_items.push_back(color);
            auto* intensity = aznew EnginePropertyDouble();
            intensity->m_name = k_propIntensity;
            intensity->m_category = "Light";
            intensity->m_value = created.intensity;
            bag.m_items.push_back(intensity);
            auto* range = aznew EnginePropertyDouble();
            range->m_name = k_propRange;
            range->m_category = "Light";
            range->m_value = created.range;
            bag.m_items.push_back(range);
            if (created.lightType == cgltf_light_type_spot)
            {
                auto* inner = aznew EnginePropertyDouble();
                inner->m_name = k_propInnerCone;
                inner->m_category = "Light";
                inner->m_value = created.spotInnerCone;
                bag.m_items.push_back(inner);
                auto* outer = aznew EnginePropertyDouble();
                outer->m_name = k_propOuterCone;
                outer->m_category = "Light";
                outer->m_value = created.spotOuterCone;
                bag.m_items.push_back(outer);
            }
        }

        AZ::Entity* entity = aznew AZ::Entity(
            created.name.empty() ? className : created.name.c_str());
        auto* nodeComp = entity->CreateComponent<EngineNodeComponent>();
        nodeComp->SetMirrorData(className, AZStd::move(bag));
        // Created-node handle: high bit marks "not a cgltf node index"; the payload is the
        // node's stable id (vector indices shift when DestroyObject erases).
        nodeComp->SetNodeHandle(k_createdHandleBit | created.id);

        const AZ::EntityId entityId = entity->GetId();
        m_entityToCreatedIndex[entityId] = createdIndex;
        if (parentId.IsValid())
        {
            m_pendingParent[entityId] = parentId;
        }
        out.push_back(entity);
    }

    void FilamentBackend::FilamentEntityMirror::SyncToEditor(AZStd::vector<AZ::Entity*>& out)
    {
        m_entityToNodeIndex.clear();
        m_entityToCreatedIndex.clear();
        m_pendingParent.clear();

        if (!m_s.cgltfData)
        {
            return;
        }
        cgltf_data* data = m_s.cgltfData;
        const cgltf_scene* scene = data->scene ? data->scene
            : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);

        AZStd::vector<AZ::Entity*> docEntities;
        if (scene)
        {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            {
                MirrorNodeRecursive(scene->nodes[i], AZ::EntityId(), docEntities);
            }
        }
        out.insert(out.end(), docEntities.begin(), docEntities.end());

        // Created nodes hang off their parent's mirror entity (or are top-level).
        for (size_t i = 0; i < m_s.createdNodes.size(); ++i)
        {
            AZ::EntityId parentMirror;
            if (m_s.createdNodes[i].parentNodeIndex != AZStd::numeric_limits<size_t>::max())
            {
                for (const auto& [id, idx] : m_entityToNodeIndex)
                {
                    if (idx == m_s.createdNodes[i].parentNodeIndex)
                    {
                        parentMirror = id;
                        break;
                    }
                }
            }
            MirrorCreatedNode(i, parentMirror, out);
        }
    }

    void FilamentBackend::FilamentEntityMirror::FinishSync()
    {
        for (const auto& [childId, parentId] : m_pendingParent)
        {
            AZ::TransformBus::Event(childId, &AZ::TransformBus::Events::SetParent, parentId);
        }
        m_pendingParent.clear();

        // Seed each mirror entity's world transform from the document, after activation, so
        // gizmo / Outliner show it in place (rbfx same pattern, RbfxBackend.cpp:877-905).
        for (const auto& [entityId, nodeIndex] : m_entityToNodeIndex)
        {
            if (nodeIndex >= m_s.cgltfData->nodes_count)
            {
                continue;
            }
            cgltf_float wm[16];
            cgltf_node_transform_world(&m_s.cgltfData->nodes[nodeIndex], wm);
            const AZ::Matrix4x4 world = MatrixFromCgltf(wm);
            float scale = 1.0f;
            const AZ::Quaternion engineRot = RotationFromCgltfMatrix(world, scale);
            AZ::TransformBus::Event(entityId, &AZ::TransformBus::Events::SetWorldTM,
                AZ::Transform::CreateFromQuaternionAndTranslation(
                    EngineTransformConverter::RotationFromEngine(k_space, engineRot),
                    EngineTransformConverter::PositionFromEngine(k_space,
                        AZ::Vector3(world.GetElement(3, 0), world.GetElement(3, 1), world.GetElement(3, 2))))
                * AZ::Transform::CreateUniformScale(scale));
        }

        // Created nodes: their stored TRS is already engine-space world (creation/edit writes
        // world directly - v1 created nodes are top level).
        for (const auto& [entityId, createdIndex] : m_entityToCreatedIndex)
        {
            if (createdIndex >= m_s.createdNodes.size())
            {
                continue;
            }
            const EngineState::CreatedNode& created = m_s.createdNodes[createdIndex];
            AZ::TransformBus::Event(entityId, &AZ::TransformBus::Events::SetWorldTM,
                AZ::Transform::CreateFromQuaternionAndTranslation(
                    EngineTransformConverter::RotationFromEngine(k_space,
                        AZ::Quaternion(created.rotation[0], created.rotation[1],
                            created.rotation[2], created.rotation[3])),
                    EngineTransformConverter::PositionFromEngine(k_space,
                        AZ::Vector3(created.translation[0], created.translation[1], created.translation[2])))
                * AZ::Transform::CreateUniformScale(created.scale[0]));
        }
    }

    void FilamentBackend::FilamentEntityMirror::EnumerateObjectTypes(AZStd::vector<ObjectTypeInfo>& out)
    {
        // Curated table: empty node + the three creatable glTF light types (plan §6.4;
        // cgltf has no "focused spot" distinction - that is a Filament runtime nuance).
        out.push_back({ "Filament", "empty_node", "Empty Node" });
        out.push_back({ "Filament/Lights", "directional", "Directional Light" });
        out.push_back({ "Filament/Lights", "point", "Point Light" });
        out.push_back({ "Filament/Lights", "spot", "Spot Light" });
    }

    void FilamentBackend::FilamentEntityMirror::OnEditorTransformChanged(
        AZ::EntityId entityId, const AZ::Transform& worldTm)
    {
        EngineState::CreatedNode* created = ResolveCreated(entityId);
        if (created)
        {
            // Created node: store engine-space world TRS directly (v1 created nodes are roots
            // or keep their own absolute placement) + drive the render entity.
            AZ::Vector3 pos;
            AZ::Quaternion rot;
            EngineTransformConverter::TransformToEngine(k_space, worldTm, pos, rot);
            const float scale = worldTm.GetUniformScale();
            created->translation[0] = pos.GetX();
            created->translation[1] = pos.GetY();
            created->translation[2] = pos.GetZ();
            created->rotation[0] = rot.GetX();
            created->rotation[1] = rot.GetY();
            created->rotation[2] = rot.GetZ();
            created->rotation[3] = rot.GetW();
            created->scale[0] = created->scale[1] = created->scale[2] = scale;
            SetCreatedEntityTransform(*created);
            return;
        }

        cgltf_node* node = ResolveNode(entityId);
        if (!node || !m_s.cgltfData)
        {
            return;
        }

        // Editor world -> engine local: convert the O3DE world to engine space, then divide
        // out the parent's world (plan §6.3 - never store world into local TRS).
        AZ::Vector3 pos;
        AZ::Quaternion rot;
        EngineTransformConverter::TransformToEngine(k_space, worldTm, pos, rot);
        const float scale = worldTm.GetUniformScale();

        // Engine-space world matrix T*R*S, assembled column-major by hand (row-vector AZ math
        // would need a transpose; this is 3 basis vectors + origin).
        AZ::Matrix3x3 rotM = AZ::Matrix3x3::CreateFromQuaternion(rot);
        cgltf_float world[16] = {};
        for (int c = 0; c < 3; ++c)
        {
            world[c * 4 + 0] = rotM.GetElement(0, c) * scale;
            world[c * 4 + 1] = rotM.GetElement(1, c) * scale;
            world[c * 4 + 2] = rotM.GetElement(2, c) * scale;
        }
        world[12] = pos.GetX();
        world[13] = pos.GetY();
        world[14] = pos.GetZ();
        world[15] = 1.0f;

        const AZ::Matrix4x4 worldAz = MatrixFromCgltf(world);
        AZ::Matrix4x4 localAz = worldAz;
        if (node->parent)
        {
            cgltf_float parentWorld[16];
            cgltf_node_transform_world(node->parent, parentWorld);
            localAz = worldAz * MatrixFromCgltf(parentWorld).GetInverseFull();
        }

        // Decompose the engine-space local matrix into TRS and write the document fields.
        float lscale = 1.0f;
        const AZ::Quaternion q = RotationFromCgltfMatrix(localAz, lscale);

        node->has_matrix = 0; // An edited matrix node becomes TRS (shear is not editable in v1).
        node->translation[0] = localAz.GetElement(3, 0);
        node->translation[1] = localAz.GetElement(3, 1);
        node->translation[2] = localAz.GetElement(3, 2);
        node->has_translation = 1;
        node->rotation[0] = q.GetX();
        node->rotation[1] = q.GetY();
        node->rotation[2] = q.GetZ();
        node->rotation[3] = q.GetW();
        node->has_rotation = 1;
        node->scale[0] = node->scale[1] = node->scale[2] = lscale;
        node->has_scale = 1;

        // Render layer dual write (M7): recompute the local mat4 from the edited TRS.
        if (m_s.engine)
        {
            auto& tm = m_s.engine->getTransformManager();
            const size_t idx = static_cast<size_t>(node - m_s.cgltfData->nodes);
            if (idx < m_s.nodeEntities.size())
            {
                if (auto inst = tm.getInstance(m_s.nodeEntities[idx]); inst.isValid())
                {
                    tm.setTransform(inst, EngineLocalMatrix(node));
                }
            }
        }
    }

    void FilamentBackend::FilamentEntityMirror::OnEditorPropertyChanged(AZ::EntityId entityId)
    {
        // Curated light table -> cgltf fields + LightManager setters (plan §6.3/§6.4).
        AZ::Entity* entity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(
            entity, &AZ::ComponentApplicationBus::Events::FindEntity, entityId);
        auto* nodeComp = entity ? entity->FindComponent<EngineNodeComponent>() : nullptr;
        if (!nodeComp)
        {
            return;
        }
        auto readDouble = [nodeComp](const char* name, float& out) -> bool
        {
            if (auto* prop = nodeComp->FindProperty(name))
            {
                if (auto* d = azrtti_cast<EnginePropertyDouble*>(prop))
                {
                    out = static_cast<float>(d->m_value);
                    return true;
                }
            }
            return false;
        };

        if (EngineState::CreatedNode* created = ResolveCreated(entityId))
        {
            if (!created->isLight)
            {
                return;
            }
            if (auto* prop = nodeComp->FindProperty(k_propColor))
            {
                if (auto* c = azrtti_cast<EnginePropertyColor*>(prop))
                {
                    created->color[0] = c->m_value.GetR();
                    created->color[1] = c->m_value.GetG();
                    created->color[2] = c->m_value.GetB();
                }
            }
            readDouble(k_propIntensity, created->intensity);
            readDouble(k_propRange, created->range);
            if (created->lightType == cgltf_light_type_spot)
            {
                readDouble(k_propInnerCone, created->spotInnerCone);
                readDouble(k_propOuterCone, created->spotOuterCone);
            }
            if (m_s.engine)
            {
                ApplyLightToRender(*m_s.engine, created->entity, created->lightType,
                    created->color, created->intensity, created->range,
                    created->spotInnerCone, created->spotOuterCone);
            }
            return;
        }

        cgltf_node* node = ResolveNode(entityId);
        cgltf_light* light = node ? node->light : nullptr;
        if (!light)
        {
            return;
        }
        if (auto* prop = nodeComp->FindProperty(k_propColor))
        {
            if (auto* c = azrtti_cast<EnginePropertyColor*>(prop))
            {
                light->color[0] = c->m_value.GetR();
                light->color[1] = c->m_value.GetG();
                light->color[2] = c->m_value.GetB();
            }
        }
        readDouble(k_propIntensity, light->intensity);
        readDouble(k_propRange, light->range);
        if (light->type == cgltf_light_type_spot)
        {
            readDouble(k_propInnerCone, light->spot_inner_cone_angle);
            readDouble(k_propOuterCone, light->spot_outer_cone_angle);
        }

        if (!m_s.engine)
        {
            return;
        }
        const size_t idx = static_cast<size_t>(node - m_s.cgltfData->nodes);
        if (idx >= m_s.nodeEntities.size())
        {
            return;
        }
        ApplyLightToRender(*m_s.engine, m_s.nodeEntities[idx], light->type,
            light->color, light->intensity, light->range,
            light->spot_inner_cone_angle, light->spot_outer_cone_angle);
    }

    AZ::EntityId FilamentBackend::FilamentEntityMirror::CreateObject(const ObjectSpec& spec)
    {
        if (!m_s.engine || !m_s.scene)
        {
            return AZ::EntityId();
        }
        auto* e = m_s.engine;

        EngineState::CreatedNode created;
        created.id = m_s.nextCreatedId++;
        created.parentNodeIndex = AZStd::numeric_limits<size_t>::max(); // v1: top-level.
        created.name = "Node";
        if (spec.m_typeId == "directional")
        {
            created.isLight = true;
            created.lightType = cgltf_light_type_directional;
            created.intensity = 1.0f; // Lux.
            created.name = "Directional Light";
        }
        else if (spec.m_typeId == "point")
        {
            created.isLight = true;
            created.lightType = cgltf_light_type_point;
            created.intensity = 10000.0f; // Candela; glTF sample-range default.
            created.name = "Point Light";
        }
        else if (spec.m_typeId == "spot")
        {
            created.isLight = true;
            created.lightType = cgltf_light_type_spot;
            created.intensity = 10000.0f;
            created.name = "Spot Light";
        }

        // Where: at the requested O3DE transform (converted to engine space).
        AZ::Vector3 pos;
        AZ::Quaternion rot;
        EngineTransformConverter::TransformToEngine(k_space, spec.m_transform, pos, rot);
        created.translation[0] = pos.GetX();
        created.translation[1] = pos.GetY();
        created.translation[2] = pos.GetZ();
        created.rotation[0] = rot.GetX();
        created.rotation[1] = rot.GetY();
        created.rotation[2] = rot.GetZ();
        created.rotation[3] = rot.GetW();

        // Live render entity immediately, so a light shows up before the re-mirror.
        created.entity = utils::EntityManager::get().create();
        auto& tm = e->getTransformManager();
        tm.create(created.entity);
        if (created.isLight)
        {
            filament::LightManager::Builder builder(
                created.lightType == cgltf_light_type_directional
                    ? filament::LightManager::Type::DIRECTIONAL
                    : created.lightType == cgltf_light_type_point
                        ? filament::LightManager::Type::POINT
                        : filament::LightManager::Type::FOCUSED_SPOT);
            builder.direction({0.0f, 0.0f, -1.0f}); // Node forward; the transform drives it.
            builder.color({created.color[0], created.color[1], created.color[2]});
            if (created.lightType == cgltf_light_type_directional)
            {
                builder.intensity(created.intensity);
            }
            else
            {
                builder.intensityCandela(created.intensity);
            }
            if (created.lightType == cgltf_light_type_spot)
            {
                builder.spotLightCone(created.spotInnerCone, created.spotOuterCone);
            }
            // range 0 = glTF "infinite"; match gltfio's load-time heuristic falloff
            // (AssetLoader.cpp:1383) so a created light renders like a reloaded one.
            builder.falloff(created.range > 0.0f ? created.range
                : std::pow(created.intensity / 0.05f, 0.25f));
            builder.build(*e, created.entity);
        }
        m_s.createdNodes.push_back(AZStd::move(created));
        EngineState::CreatedNode& stored = m_s.createdNodes.back();
        SetCreatedEntityTransform(stored);
        m_s.scene->addEntity(stored.entity);

        // The mirror entity appears on the next full re-sync (shell refreshes after create),
        // matching the rbfx v1 contract - return an invalid id.
        return AZ::EntityId();
    }

    auto FilamentBackend::FilamentEntityMirror::SetCreatedEntityTransform(
        const EngineState::CreatedNode& created) -> void
    {
        if (!m_s.engine || !created.entity)
        {
            return;
        }
        using filament::math::float3;
        using filament::math::mat4f;
        auto& tm = m_s.engine->getTransformManager();
        const auto inst = tm.getInstance(created.entity);
        if (!inst.isValid())
        {
            return;
        }
        // created.rotation is glTF/AZ order (x,y,z,w); filament's quatf ctor takes (w,x,y,z)
        // (math/quat.h:103).
        const mat4f trs =
            mat4f::translation(float3(created.translation[0], created.translation[1], created.translation[2]))
            * mat4f(filament::math::quatf(
                created.rotation[3], created.rotation[0], created.rotation[1], created.rotation[2]))
            * mat4f::scaling(float3(created.scale[0], created.scale[1], created.scale[2]));
        tm.setTransform(inst, trs);
    }

    void FilamentBackend::FilamentEntityMirror::DestroyObject(AZ::EntityId entityId)
    {
        if (EngineState::CreatedNode* created = ResolveCreated(entityId))
        {
            if (m_s.engine && m_s.scene && created->entity)
            {
                m_s.scene->remove(created->entity);
                m_s.engine->destroy(created->entity);
            }
            m_s.createdNodes.erase(m_s.createdNodes.begin() +
                static_cast<ptrdiff_t>(created - m_s.createdNodes.data()));
            return;
        }

        cgltf_node* node = ResolveNode(entityId);
        if (!node || !m_s.cgltfData)
        {
            return;
        }
        const size_t nodeIndex = static_cast<size_t>(node - m_s.cgltfData->nodes);

        // Detach from the parent's children / scene roots (memmove compress; the doc entry
        // itself stays until the save-time reachability merge prunes it - plan §6.3).
        auto removeFromList = [](cgltf_node** list, cgltf_size& count, cgltf_node* target)
        {
            for (cgltf_size i = 0; i < count; ++i)
            {
                if (list[i] == target)
                {
                    for (cgltf_size j = i; j + 1 < count; ++j)
                    {
                        list[j] = list[j + 1];
                    }
                    --count;
                    return;
                }
            }
        };
        if (node->parent)
        {
            removeFromList(node->parent->children, node->parent->children_count, node);
        }
        if (m_s.cgltfData->scene)
        {
            removeFromList(m_s.cgltfData->scene->nodes, m_s.cgltfData->scene->nodes_count, node);
        }

        // Collect the subtree's render entities and destroy them (scene + components + entity).
        EngineState::DeletedNode dead;
        dead.nodeIndex = nodeIndex;
        AZStd::function<void(cgltf_node*)> collect = [&](cgltf_node* n)
        {
            const size_t idx = static_cast<size_t>(n - m_s.cgltfData->nodes);
            if (idx < m_s.nodeEntities.size() && !m_s.nodeEntities[idx].isNull())
            {
                dead.entities.push_back(m_s.nodeEntities[idx]);
                m_s.nodeEntities[idx].clear();
            }
            for (cgltf_size i = 0; i < n->children_count; ++i)
            {
                collect(n->children[i]);
            }
        };
        collect(node);
        if (m_s.engine && m_s.scene)
        {
            for (utils::Entity entity : dead.entities)
            {
                m_s.scene->remove(entity);
                m_s.engine->destroy(entity);
            }
        }
        m_s.deletedNodes.push_back(AZStd::move(dead));
    }

    bool FilamentBackend::FilamentEntityMirror::SaveScene(const AZStd::string& path)
    {
        if (!m_s.cgltfData)
        {
            return false;
        }
        cgltf_data* data = m_s.cgltfData;

        AZStd::string targetPath = path.empty() ? m_s.scenePath : path;
        if (targetPath.empty())
        {
            AZ_Warning(k_logTag, false, "filament SaveScene: no path and no loaded scene file.");
            return false;
        }

        // ---- Save-time reachability merge (plan §6.3) -------------------------------
        // cgltf cross references are raw pointers into data->nodes, so runtime edits never
        // touch that array. Here we build the pruned + extended node list the file gets:
        // reachable = default-scene walk + skin joints + animation targets (closed under
        // ancestors), then the created nodes appended. Deleted nodes were detached at
        // destroy time, so the walk skips them naturally.
        AZStd::unordered_map<cgltf_node*, size_t> newIndex;
        AZStd::vector<cgltf_node*> order;

        auto includeWithAncestors = [&](cgltf_node* n)
        {
            AZStd::vector<cgltf_node*> chain;
            for (cgltf_node* p = n; p; p = p->parent)
            {
                chain.push_back(p);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                if (!newIndex.count(*it))
                {
                    newIndex[*it] = order.size();
                    order.push_back(*it);
                }
            }
        };
        if (data->scene)
        {
            AZStd::function<void(cgltf_node*)> walk = [&](cgltf_node* n)
            {
                includeWithAncestors(n); // no-op for already-included ancestors
                for (cgltf_size i = 0; i < n->children_count; ++i)
                {
                    walk(n->children[i]);
                }
            };
            for (cgltf_size i = 0; i < data->scene->nodes_count; ++i)
            {
                includeWithAncestors(data->scene->nodes[i]);
                for (cgltf_size c = 0; c < data->scene->nodes[i]->children_count; ++c)
                {
                    walk(data->scene->nodes[i]->children[c]);
                }
            }
        }
        for (cgltf_size s = 0; s < data->skins_count; ++s)
        {
            for (cgltf_size j = 0; j < data->skins[s].joints_count; ++j)
            {
                includeWithAncestors(data->skins[s].joints[j]);
            }
        }
        for (cgltf_size a = 0; a < data->animations_count; ++a)
        {
            for (cgltf_size c = 0; c < data->animations[a].channels_count; ++c)
            {
                if (data->animations[a].channels[c].target_node)
                {
                    includeWithAncestors(data->animations[a].channels[c].target_node);
                }
            }
        }
        const size_t docCount = order.size();
        const size_t createdCount = m_s.createdNodes.size();

        // Lights: old array + created light definitions (node->light must point into it).
        AZStd::vector<cgltf_light> lights(
            data->lights, data->lights + data->lights_count);
        for (const EngineState::CreatedNode& created : m_s.createdNodes)
        {
            if (!created.isLight)
            {
                continue;
            }
            cgltf_light light = {};
            light.color[0] = created.color[0];
            light.color[1] = created.color[1];
            light.color[2] = created.color[2];
            light.intensity = created.intensity;
            light.type = created.lightType;
            light.range = created.range;
            light.spot_inner_cone_angle = created.spotInnerCone;
            light.spot_outer_cone_angle = created.spotOuterCone;
            lights.push_back(light);
        }

        // Merged node array. Capacity is exact: entries must not move once referenced.
        AZStd::vector<cgltf_node> nodes;
        nodes.reserve(docCount + createdCount);
        AZStd::vector<AZStd::vector<cgltf_node*>> childLists(docCount);
        for (size_t i = 0; i < docCount; ++i)
        {
            cgltf_node copy = *order[i];
            if (copy.parent)
            {
                const auto it = newIndex.find(copy.parent);
                copy.parent = (it != newIndex.end()) ? &nodes[it->second] : nullptr;
            }
            // cgltf_write resolves node->light by pointer diff against data->lights, so
            // repoint at the extended lights array built above.
            if (copy.light)
            {
                copy.light = &lights[static_cast<size_t>(copy.light - data->lights)];
            }
            childLists[i].reserve(copy.children_count);
            for (cgltf_size c = 0; c < copy.children_count; ++c)
            {
                const auto it = newIndex.find(copy.children[c]);
                if (it != newIndex.end())
                {
                    childLists[i].push_back(&nodes[it->second]);
                }
            }
            nodes.push_back(copy);
        }
        for (size_t i = 0; i < createdCount; ++i)
        {
            const EngineState::CreatedNode& created = m_s.createdNodes[i];
            cgltf_node node = {};
            node.name = const_cast<char*>(created.name.c_str()); // Lives through the write call.
            node.has_translation = 1;
            memcpy(node.translation, created.translation, sizeof(node.translation));
            node.has_rotation = 1;
            memcpy(node.rotation, created.rotation, sizeof(node.rotation));
            node.has_scale = 1;
            memcpy(node.scale, created.scale, sizeof(node.scale));
            if (created.isLight)
            {
                // Index of this created light in the merged lights array.
                size_t lightIdx = data->lights_count;
                for (size_t j = 0; j < i; ++j)
                {
                    if (m_s.createdNodes[j].isLight)
                    {
                        ++lightIdx;
                    }
                }
                node.light = &lights[lightIdx];
            }
            if (created.parentNodeIndex != AZStd::numeric_limits<size_t>::max()
                && created.parentNodeIndex < m_s.cgltfData->nodes_count)
            {
                const auto it = newIndex.find(&m_s.cgltfData->nodes[created.parentNodeIndex]);
                if (it != newIndex.end())
                {
                    node.parent = &nodes[it->second];
                    childLists[it->second].push_back(nodes.data() + docCount + i);
                }
            }
            nodes.push_back(node);
        }
        for (size_t i = 0; i < docCount; ++i)
        {
            nodes[i].children = childLists[i].empty() ? nullptr : childLists[i].data();
            nodes[i].children_count = static_cast<cgltf_size>(childLists[i].size());
        }

        // Scenes: remap roots; created roots land in the default scene.
        AZStd::vector<AZStd::vector<cgltf_node*>> sceneRoots(data->scenes_count);
        AZStd::vector<cgltf_scene> scenes(data->scenes, data->scenes + data->scenes_count);
        for (cgltf_size s = 0; s < data->scenes_count; ++s)
        {
            for (cgltf_size n = 0; n < data->scenes[s].nodes_count; ++n)
            {
                const auto it = newIndex.find(data->scenes[s].nodes[n]);
                if (it != newIndex.end())
                {
                    sceneRoots[s].push_back(&nodes[it->second]);
                }
            }
        }
        cgltf_scene* defaultScene = data->scene
            ? &scenes[data->scene - data->scenes]
            : (data->scenes_count > 0 ? &scenes[0] : nullptr);
        if (defaultScene)
        {
            const size_t defaultIdx = defaultScene - scenes.data();
            for (size_t i = 0; i < createdCount; ++i)
            {
                const EngineState::CreatedNode& created = m_s.createdNodes[i];
                const bool isRoot = created.parentNodeIndex == AZStd::numeric_limits<size_t>::max()
                    || newIndex.find(&m_s.cgltfData->nodes[created.parentNodeIndex]) == newIndex.end();
                if (isRoot)
                {
                    sceneRoots[defaultIdx].push_back(nodes.data() + docCount + i);
                }
            }
        }
        else if (createdCount > 0)
        {
            AZ_Warning(k_logTag, false,
                "filament SaveScene: %u created node(s) dropped - the file declares no scene.",
                static_cast<unsigned>(createdCount));
        }
        for (size_t s = 0; s < scenes.size(); ++s)
        {
            scenes[s].nodes = sceneRoots[s].empty() ? nullptr : sceneRoots[s].data();
            scenes[s].nodes_count = static_cast<cgltf_size>(sceneRoots[s].size());
        }

        // Skins + animations: deep-copy their inner pointer arrays (a shallow struct copy
        // would share them with the live doc, and remapping in place would leave the live
        // doc pointing into this function's merged vector after it dies), then remap their
        // node pointers into the merged array.
        AZStd::vector<cgltf_skin> skins(data->skins, data->skins + data->skins_count);
        AZStd::vector<AZStd::vector<cgltf_node*>> jointStorage(skins.size());
        for (size_t s = 0; s < skins.size(); ++s)
        {
            jointStorage[s].assign(skins[s].joints, skins[s].joints + skins[s].joints_count);
            skins[s].joints = jointStorage[s].data();
            for (cgltf_node*& joint : jointStorage[s])
            {
                joint = &nodes[newIndex[joint]];
            }
            // cgltf_write resolves skin->skeleton by pointer diff against the merged nodes
            // (cgltf_write.h:889) - remap it too, or the written index is garbage. A skeleton
            // outside the joint ancestor closure degrades to null, which is honest.
            if (skins[s].skeleton)
            {
                const auto it = newIndex.find(skins[s].skeleton);
                skins[s].skeleton = (it != newIndex.end()) ? &nodes[it->second] : nullptr;
            }
        }
        // Node copies still point at the ORIGINAL skins array; cgltf_write resolves
        // node->skin by pointer diff against data->skins, so repoint them at the copy.
        for (auto& node : nodes)
        {
            if (node.skin)
            {
                node.skin = &skins[static_cast<size_t>(node.skin - data->skins)];
            }
        }
        AZStd::vector<cgltf_animation> animations(
            data->animations, data->animations + data->animations_count);
        AZStd::vector<AZStd::vector<cgltf_animation_channel>> channelStorage(animations.size());
        for (size_t a = 0; a < animations.size(); ++a)
        {
            channelStorage[a].assign(
                animations[a].channels, animations[a].channels + animations[a].channels_count);
            animations[a].channels = channelStorage[a].data();
            for (auto& channel : channelStorage[a])
            {
                if (channel.target_node)
                {
                    channel.target_node = &nodes[newIndex[channel.target_node]];
                }
            }
        }

        // Shallow doc copy pointing at the merged structures (cgltf_write only reads).
        cgltf_data copy = *data;
        // cgltf_write resolves the default scene by pointer arithmetic against data->scenes,
        // so the copy's scene pointer must point into the remapped scenes array.
        copy.scene = data->scene ? &scenes[static_cast<size_t>(data->scene - data->scenes)] : nullptr;
        copy.nodes = nodes.data();
        copy.nodes_count = static_cast<cgltf_size>(nodes.size());
        copy.scenes = scenes.data();
        copy.scenes_count = static_cast<cgltf_size>(scenes.size());
        copy.lights = lights.data();
        copy.lights_count = static_cast<cgltf_size>(lights.size());
        copy.skins = skins.data();
        copy.animations = animations.data();

        cgltf_options options = {};
        options.type = targetPath.ends_with(".glb") ? cgltf_file_type_glb : cgltf_file_type_gltf;

        // Crash-safe save (editor_polish.md P0-8 / M2, rbfx same): write .tmp, promote with .bak.
        const AZStd::string tmpPath = targetPath + ".cee.tmp";
        if (cgltf_write_file(&options, tmpPath.c_str(), &copy) != cgltf_result_success)
        {
            AZ_Warning(k_logTag, false, "filament SaveScene: cgltf_write_file failed for %s.",
                tmpPath.c_str());
            AZ::IO::SystemFile::Delete(tmpPath.c_str());
            return false;
        }
        const AZStd::string bakPath = targetPath + ".bak";
        if (AZ::IO::SystemFile::Exists(targetPath.c_str()))
        {
            if (!AZ::IO::SystemFile::Rename(targetPath.c_str(), bakPath.c_str(), /*overwrite=*/true))
            {
                AZ_Warning(k_logTag, false, "filament SaveScene: could not back up %s.", targetPath.c_str());
                AZ::IO::SystemFile::Delete(tmpPath.c_str());
                return false;
            }
        }
        if (!AZ::IO::SystemFile::Rename(tmpPath.c_str(), targetPath.c_str(), /*overwrite=*/true))
        {
            AZ::IO::SystemFile::Rename(bakPath.c_str(), targetPath.c_str(), /*overwrite=*/true);
            AZ::IO::SystemFile::Delete(tmpPath.c_str());
            return false;
        }

        // Saved: the merged state becomes the new document truth. Rebuild the runtime doc
        // in place by clearing the pending-delete bookkeeping (nodes were detached already).
        m_s.deletedNodes.clear();
        AZ_Printf(k_logTag, "filament SaveScene: wrote %s (%u nodes, backup: %s)\n",
            targetPath.c_str(), static_cast<unsigned>(nodes.size()), bakPath.c_str());
        return true;
    }

    AZ::Aabb FilamentBackend::FilamentEntityMirror::GetWorldBounds(AZ::EntityId id) const
    {
        cgltf_node* node = ResolveNode(id);
        if (!node)
        {
            // Created nodes have no mesh in v1 (lights/empties) - not ray-pickable.
            return AZ::Aabb::CreateNull();
        }
        return WorldBoundsOfNode(node);
    }

    bool FilamentBackend::FilamentEntityMirror::RaycastScene(
        const AZ::Vector3& origin, const AZ::Vector3& dir,
        AZ::Vector3& hitPoint, AZ::Vector3& hitNormal) const
    {
        // Walk the default scene like the mirror does (plan §7.2): deleted subtrees were
        // detached at destroy time and are unreachable here, so they can never intercept the
        // ray; the overlay lives outside the cgltf doc entirely.
        if (!m_s.cgltfData)
        {
            return false;
        }
        const cgltf_data* data = m_s.cgltfData;
        const cgltf_scene* scene = data->scene ? data->scene
            : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
        if (!scene)
        {
            return false;
        }
        // Guard the reciprocal against divide-by-zero on axis-aligned rays (AzCore has no
        // "safe" variant; a zero component becomes FLT_MAX like the usual slab-trick).
        AZ::Vector3 dirRcp = dir.GetReciprocal();
        constexpr float kRcpMax = 3.4e38f;
        dirRcp = AZ::Vector3(
            std::abs(dirRcp.GetX()) > kRcpMax ? kRcpMax : dirRcp.GetX(),
            std::abs(dirRcp.GetY()) > kRcpMax ? kRcpMax : dirRcp.GetY(),
            std::abs(dirRcp.GetZ()) > kRcpMax ? kRcpMax : dirRcp.GetZ());
        float bestT = std::numeric_limits<float>::max();
        AZ::Vector3 bestPoint = AZ::Vector3::CreateZero();
        AZ::Vector3 bestNormal = AZ::Vector3::CreateZero();

        AZStd::function<void(const cgltf_node*)> testNode = [&](const cgltf_node* node)
        {
            const AZ::Aabb box = WorldBoundsOfNode(node);
            if (box.IsValid())
            {
                // Slab test in O3DE space; tStart parametrises along the given (non-unit) dir.
                float tStart = 0.0f, tEnd = 0.0f;
                AZ::Vector3 normal = AZ::Vector3::CreateZero();
                const auto result = AZ::Intersect::IntersectRayAABB(
                    origin, dir, dirRcp, box, tStart, tEnd, normal);
                // SA_INSIDE = ray origin inside the box (tStart is 0) - accept it too,
                // else clicking while inside a large mesh misses.
                if (result == AZ::Intersect::ISECT_RAY_AABB_ISECT
                    || result == AZ::Intersect::ISECT_RAY_AABB_SA_INSIDE)
                {
                    const float t = (tStart > 0.0f) ? tStart : 0.0f; // Inside the box: report entry.
                    if (t < bestT)
                    {
                        bestT = t;
                        bestPoint = origin + dir * t;
                        bestNormal = normal;
                    }
                }
            }
            for (cgltf_size i = 0; i < node->children_count; ++i)
            {
                testNode(node->children[i]);
            }
        };
        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
        {
            testNode(scene->nodes[i]);
        }
        if (bestT == std::numeric_limits<float>::max())
        {
            return false;
        }
        hitPoint = bestPoint;
        hitNormal = bestNormal;
        return true;
    }

    cgltf_node* FilamentBackend::FilamentEntityMirror::ResolveNode(AZ::EntityId id) const
    {
        if (!m_s.cgltfData)
        {
            return nullptr;
        }
        AZ::Entity* entity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(
            entity, &AZ::ComponentApplicationBus::Events::FindEntity, id);
        auto* nodeComp = entity ? entity->FindComponent<EngineNodeComponent>() : nullptr;
        if (!nodeComp)
        {
            return nullptr;
        }
        const AZ::u64 handle = nodeComp->GetNodeHandle();
        if (handle & k_createdHandleBit)
        {
            return nullptr; // Created-node handle, not a doc node.
        }
        const size_t idx = static_cast<size_t>(handle);
        return (idx < m_s.cgltfData->nodes_count) ? &m_s.cgltfData->nodes[idx] : nullptr;
    }

    auto FilamentBackend::FilamentEntityMirror::ResolveCreated(AZ::EntityId id)
        -> EngineState::CreatedNode*
    {
        AZ::Entity* entity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(
            entity, &AZ::ComponentApplicationBus::Events::FindEntity, id);
        auto* nodeComp = entity ? entity->FindComponent<EngineNodeComponent>() : nullptr;
        if (!nodeComp)
        {
            return nullptr;
        }
        const AZ::u64 handle = nodeComp->GetNodeHandle();
        if (!(handle & k_createdHandleBit))
        {
            return nullptr;
        }
        const uint32_t createdId = static_cast<uint32_t>(handle & ~k_createdHandleBit);
        for (auto& created : m_s.createdNodes)
        {
            if (created.id == createdId)
            {
                return &created;
            }
        }
        return nullptr;
    }

    // =====================================================================
    // FilamentAssetSource - project directory walk (NullBackend pattern)
    // =====================================================================

    namespace
    {
        void EnumerateFolder(
            const AZ::IO::FixedMaxPath& folderPath, AZStd::vector<AssetEntryInfo>& out)
        {
            AZ::IO::FixedMaxPath filter{ folderPath };
            filter /= "*";
            AZ::IO::SystemFile::FindFiles(
                filter.c_str(),
                [&out, &folderPath](const char* name, bool isFile) -> bool
                {
                    if (azstricmp(name, ".") == 0 || azstricmp(name, "..") == 0)
                    {
                        return true;
                    }
                    AZ::IO::FixedMaxPath child{ folderPath };
                    child /= name;

                    AssetEntryInfo entry;
                    entry.m_path = child.String(); // Forward slashes, same as the Null backend.
                    entry.m_displayName = name;
                    entry.m_isFolder = !isFile;
                    out.push_back(AZStd::move(entry));
                    return true;
                });

            AZStd::sort(
                out.begin(), out.end(),
                [](const AssetEntryInfo& a, const AssetEntryInfo& b)
                {
                    if (a.m_isFolder != b.m_isFolder)
                    {
                        return a.m_isFolder;
                    }
                    return azstricmp(a.m_displayName.c_str(), b.m_displayName.c_str()) < 0;
                });
        }
    } // namespace

    void FilamentBackend::FilamentAssetSource::EnumerateRoot(AZStd::vector<AssetEntryInfo>& out)
    {
        if (auto* registry = AZ::SettingsRegistry::Get())
        {
            AZ::IO::FixedMaxPath projectRoot;
            registry->Get(
                projectRoot.Native(), AZ::SettingsRegistryMergeUtils::FilePathKey_ProjectPath);
            if (!projectRoot.empty())
            {
                EnumerateFolder(projectRoot, out);
            }
        }
    }

    void FilamentBackend::FilamentAssetSource::EnumerateChildren(
        const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out)
    {
        if (!parent.m_isFolder)
        {
            return;
        }
        EnumerateFolder(AZ::IO::FixedMaxPath(parent.m_path.c_str()), out);
    }
} // namespace CrossEngineEditor
