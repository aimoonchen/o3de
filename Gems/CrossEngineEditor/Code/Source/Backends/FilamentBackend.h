/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Filament PBR rendering engine backend (filament_migration.md).
//!
//! Filament is a renderer, not a game engine: Scene is a flat entity container with no
//! serialization or scene graph. CEE provides all editing value; Filament provides
//! rendering + glTF I/O + material runtime.
//!
//! Scene truth = the live cgltf document from gltfio (never call releaseSourceData);
//! save = cgltf_write_file. Viewport: Engine::createSwapChain(HWND), frame driven by
//! EndOverlayFrame. Coordinates: Y-up RH (-Z forward), same as Godot - EngineSpace::Filament
//! shares the Godot branch constants.
//!
//! Compiled only when CEE_HAVE_FILAMENT is defined. Filament builds with -fno-rtti; no
//! dynamic_cast on engine types. CRT must be /MD (USE_STATIC_CRT=OFF, B3).

#include <BackendAPI/IAssetSource.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>
#include <BackendAPI/IMaterialSource.h>
#include <BackendAPI/ISceneRenderer.h>
#include <Framework/EngineTransformConverter.h>
#include <Framework/EngineProperty.h>

#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/string/string.h>

// Filament headers - macro-heavy, excluded from unity builds.
// rbfx publicly defines DESKTOP=1 (a Urho3D platform define), which rewrites filament's
// ShaderModel::DESKTOP enumerator into "1 = 2". Park the macro for the island.
#pragma push_macro("DESKTOP")
#undef DESKTOP
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
AZ_PUSH_DISABLE_WARNING(4100 4244 4267 4819, "-Wunknown-warning-option")

#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <filament/Camera.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>
#include <filament/LightManager.h>
#include <filament/TransformManager.h>
#include <utils/Entity.h>

#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/TextureProvider.h>
#include <gltfio/ResourceLoader.h>
#include <utils/NameComponentManager.h>

#include <cgltf.h>

AZ_POP_DISABLE_WARNING
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#pragma pop_macro("DESKTOP")

#include <cstdint>

namespace filament::gltfio
{
    class ResourceLoader;
}

namespace CrossEngineEditor
{
    class FilamentMaterialSource;

    class FilamentBackend final : public IEngineBackend
    {
    public:
        AZ_RTTI(FilamentBackend, "{A3B5C7D9-1E2F-3A4B-5C6D-7E8F9A0B1C2D}", IEngineBackend);

        FilamentBackend();
        ~FilamentBackend() override;

        std::expected<void, BackendError> Initialize(const BackendInitParams& params) override;
        void Shutdown() override;
        void Tick(float deltaSeconds) override;

        ISceneRenderer& GetSceneRenderer() override { return m_sceneRenderer; }
        IEntityMirror& GetEntityMirror() override { return m_entityMirror; }
        IAssetSource& GetAssetSource() override { return m_assetSource; }
        IMaterialSource& GetMaterialSource() override;

    private:
        //! Shared engine state referenced by all inner classes.
        struct EngineState
        {
            filament::Engine* engine = nullptr;
            filament::Renderer* renderer = nullptr;
            filament::SwapChain* swapChain = nullptr;
            filament::View* view = nullptr;
            filament::Scene* scene = nullptr;
            filament::Camera* camera = nullptr;
            utils::Entity cameraEntity;
            bool ready = false;
            uint32_t width = 1;
            uint32_t height = 1;
            void* nativeWindow = nullptr;
            AZStd::string scenePath; //!< Absolute path of the loaded .gltf/.glb ("" = empty scene).

            // Overlay (one unlit material, two instances = depth test on/off, plan §9).
            // Depth state is a submission-time property (the shell calls SetDepthTest, then
            // Submit*), so staging and renderables are bucketed by it. Each batch owns its
            // buffers: filament's setBufferAt is a deferred command that lands before any
            // draw, so two batches sharing one VB would both draw the last upload.
            struct OverlayBatch
            {
                filament::VertexBuffer* vb = nullptr;
                filament::IndexBuffer* ib = nullptr;
                utils::Entity entity;
                // CPU staging; copied to per-frame heap blocks on upload (the GPU consumes
                // BufferDescriptors asynchronously, so the destination must outlive the frame).
                AZStd::vector<float> positions;
                AZStd::vector<uint32_t> colors;
                AZStd::vector<uint16_t> indices;
            };
            OverlayBatch overlayLines[2];     //!< [0] = depth-tested, [1] = always-on-top.
            OverlayBatch overlayTriangles[2]; //!< [0] = depth-tested, [1] = always-on-top.
            filament::Material* overlayMaterial = nullptr;
            filament::MaterialInstance* overlayMIOn = nullptr;
            filament::MaterialInstance* overlayMIOff = nullptr;

            // gltfio (asset owns the live cgltf document - our single scene truth).
            filament::gltfio::AssetLoader* assetLoader = nullptr;
            filament::gltfio::ResourceLoader* resourceLoader = nullptr;
            filament::gltfio::MaterialProvider* materialProvider = nullptr;
            filament::gltfio::TextureProvider* stbProvider = nullptr;
            filament::gltfio::TextureProvider* ktx2Provider = nullptr;
            utils::NameComponentManager* nameManager = nullptr;
            filament::gltfio::FilamentAsset* asset = nullptr;
            cgltf_data* cgltfData = nullptr;

            //! cgltf node index -> its Filament entity (null when deleted/unmapped). Built once
            //! per load; created nodes are tracked separately below.
            AZStd::vector<utils::Entity> nodeEntities;

            //! A node deleted in the editor. The cgltf cross references are raw pointers, so the
            //! entry stays in data->nodes until the save-time reachability merge prunes it.
            struct DeletedNode
            {
                size_t nodeIndex = 0;
                AZStd::vector<utils::Entity> entities; //!< This subtree's render entities.
            };
            AZStd::vector<DeletedNode> deletedNodes;

            //! Material source (owned by FilamentBackend); the scene renderer reaches it to
            //! render the material preview inside EndOverlayFrame.
            FilamentMaterialSource* materialSource = nullptr;

            //! A node created in the editor. Never enters cgltfData->nodes at runtime (the
            //! nodes array is file-sized and full of cross pointers); materialized into the
            //! document by the save-time merge (plan §6.3).
            struct CreatedNode
            {
                uint32_t id = 0; //!< Stable session id; mirror handles store this, not the vector index (erase shifts indices).
                size_t parentNodeIndex = 0; //!< Parent cgltf node index; SIZE_MAX = scene root.
                bool isLight = false;
                cgltf_light_type lightType = cgltf_light_type_invalid;
                float translation[3] = { 0.0f, 0.0f, 0.0f }; //!< Engine (glTF) space.
                float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                float scale[3] = { 1.0f, 1.0f, 1.0f };
                float color[3] = { 1.0f, 1.0f, 1.0f };
                float intensity = 1.0f;
                float range = 0.0f;
                float spotInnerCone = 0.0f;
                float spotOuterCone = 0.785398f; //!< ~45 deg, glTF spec default.
                AZStd::string name;
                utils::Entity entity; //!< Live render entity (created immediately).
            };
            AZStd::vector<CreatedNode> createdNodes;
            uint32_t nextCreatedId = 0;
        };

        class FilamentSceneRenderer final : public ISceneRenderer
        {
        public:
            explicit FilamentSceneRenderer(EngineState& s) : m_s(s) {}
            void OnSurfaceCreated(void* handle, uint32_t w, uint32_t h) override;
            void OnSurfaceResized(uint32_t w, uint32_t h) override;
            void OnSurfaceAboutToBeDestroyed() override;
            //! Destroy every engine-owned viewport object (swapchain + view/scene/camera/
            //! renderer + overlay). Called from FilamentBackend::Shutdown.
            void Teardown();
            bool IsSurfaceReady() const override { return m_s.ready; }
            void BeginOverlayFrame(const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip) override;
            void SubmitLines(AZStd::span<const DebugVertex> vertices) override;
            void SubmitTriangles(AZStd::span<const DebugVertex> vertices) override;
            void SetDepthTest(bool enabled) override;
            void EndOverlayFrame() override;
        private:
            void CreateOverlayResources();
            void DestroyOverlayResources();
            void LoadSceneAsset(); //!< Read m_s.scenePath -> gltfio asset + live cgltf doc.
            void BuildNodeEntityMap(); //!< cgltf node index -> Filament entity.
            EngineState& m_s;
            bool m_depthTest = true; //!< Submission-time flag for the next Submit* calls (rbfx same).
        };

        class FilamentEntityMirror final : public IEntityMirror
        {
        public:
            explicit FilamentEntityMirror(EngineState& s) : m_s(s) {}
            void SyncToEditor(AZStd::vector<AZ::Entity*>& out) override;
            void FinishSync() override;
            void EnumerateObjectTypes(AZStd::vector<ObjectTypeInfo>& out) override;
            void OnEditorTransformChanged(AZ::EntityId id, const AZ::Transform& tm) override;
            void OnEditorPropertyChanged(AZ::EntityId id) override;
            AZ::EntityId CreateObject(const ObjectSpec& spec) override;
            void DestroyObject(AZ::EntityId id) override;
            bool SaveScene(const AZStd::string& path) override;
            AZ::Aabb GetWorldBounds(AZ::EntityId id) const override;
            bool RaycastScene(const AZ::Vector3& origin, const AZ::Vector3& dir,
                AZ::Vector3& hitPoint, AZ::Vector3& hitNormal) const override;
        private:
            void MirrorNodeRecursive(cgltf_node* node, AZ::EntityId parentId,
                AZStd::vector<AZ::Entity*>& out);
            void MirrorCreatedNode(size_t createdIndex, AZ::EntityId parentId,
                AZStd::vector<AZ::Entity*>& out);
            cgltf_node* ResolveNode(AZ::EntityId id) const;
            EngineState::CreatedNode* ResolveCreated(AZ::EntityId id);
            //! Push a created node's stored TRS to its render entity's transform.
            void SetCreatedEntityTransform(const EngineState::CreatedNode& created);
            EngineState& m_s;
            AZStd::unordered_map<AZ::EntityId, size_t> m_entityToNodeIndex;
            AZStd::unordered_map<AZ::EntityId, size_t> m_entityToCreatedIndex;
            AZStd::unordered_map<AZ::EntityId, AZ::EntityId> m_pendingParent;
        };

        class FilamentAssetSource final : public IAssetSource
        {
        public:
            void EnumerateRoot(AZStd::vector<AssetEntryInfo>& out) override;
            void EnumerateChildren(const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out) override;
        };

        EngineState m_state;
        FilamentSceneRenderer m_sceneRenderer{ m_state };
        FilamentEntityMirror m_entityMirror{ m_state };
        FilamentAssetSource m_assetSource;
        AZStd::unique_ptr<FilamentMaterialSource> m_materialSource;
    };
} // namespace CrossEngineEditor
