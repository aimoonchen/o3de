/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! rbfx (Urho3D fork) engine backend (final plan phase R1).
//!
//! Unified render model A: the editor owns the native window (HWND) and rbfx attaches to
//! it. On OnSurfaceCreated the backend brings the engine up with the engine parameter
//! EP_EXTERNAL_WINDOW = HWND, so SDL creates its window from the editor surface and rbfx
//! presents straight into the viewport. Overlay (gizmos/grid) is drawn with a scene-root
//! DebugRenderer's 3 primitives (AddLine/AddTriangle), which the render pipeline draws
//! automatically with the scene camera.
//!
//! The object mirror (RbfxEntityMirror) walks the rbfx Scene into AZ::Entity mirror
//! entities and reflects each node's Serializable attributes into the typed PropertyBag
//! shown in the Inspector. Coordinates cross the boundary in O3DE convention and are
//! converted at the edge by EngineTransformConverter (rbfx = Y-up / left-handed / +Z).
//!
//! Compiled only when the rbfx submodule is available (CEE_HAVE_RBFX). rbfx uses EASTL
//! (ea::) containers internally; those never leak across the BackendAPI - values are
//! converted to AZStd/AZ types at the mirror edge.

#include <BackendAPI/IAssetSource.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>
#include <BackendAPI/ISceneRenderer.h>
#include <Framework/EngineTransformConverter.h>
#include <Framework/EngineProperty.h>

#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/vector.h>

// rbfx/EASTL/rapidjson headers trip many MSVC warnings that O3DE promotes to errors (/WX):
// virtual-hides (C4263/4264/4265/4266), conversions, unused params, etc. These are third-party
// headers, so fully silence warnings across the whole Urho3D include island (push,0) rather than
// chase individual codes. First-party code below the pop is unaffected.
#if defined(_MSC_VER)
#    pragma warning(push, 0)
// O3DE promotes some warnings to errors via /weNNNN, which warning(push,0) does NOT undo.
// rapidjson (pulled transitively) trips C5054 (operator '|' between different enum types).
#    pragma warning(disable : 5054)
#endif
AZ_PUSH_DISABLE_WARNING(4251 4244 4245 4267 4100 4263 4264 4265 4266, "-Wunknown-warning-option")
#include <Urho3D/Core/Context.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Scene/Scene.h>
AZ_POP_DISABLE_WARNING
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

#include <cstdint>

namespace Urho3D
{
    class Node;
    class Camera;
    class DebugRenderer;
    class Viewport;
}

namespace AZ
{
    class Entity;
}

namespace CrossEngineEditor
{
    class RbfxBackend final : public IEngineBackend
    {
    public:
        AZ_RTTI(RbfxBackend, "{9F2C4E51-7A38-4D06-8B12-3E5A9C7D1F42}", IEngineBackend);

        RbfxBackend();
        ~RbfxBackend() override;

        std::expected<void, BackendError> Initialize(const BackendInitParams& params) override;
        void Shutdown() override;
        void Tick(float deltaSeconds) override;

        ISceneRenderer& GetSceneRenderer() override { return m_sceneRenderer; }
        IEntityMirror& GetEntityMirror() override { return m_entityMirror; }
        IAssetSource& GetAssetSource() override { return m_assetSource; }

    private:
        //! Shared engine state, owned by the backend and referenced by the sub-contracts.
        struct EngineState
        {
            Urho3D::SharedPtr<Urho3D::Context> m_context;
            Urho3D::SharedPtr<Urho3D::Engine> m_engine;
            Urho3D::SharedPtr<Urho3D::Scene> m_scene;
            Urho3D::Node* m_cameraNode = nullptr; //!< Owned by the scene, not a SharedPtr.
            Urho3D::Camera* m_camera = nullptr;
            Urho3D::DebugRenderer* m_debug = nullptr;
            bool m_initialized = false;
            AZStd::string m_projectPath;
            AZStd::string m_scenePath;   //!< Resource-relative scene to open (e.g. "Scenes/X.xml").
        };

        //! rbfx scene renderer bound to the editor viewport HWND (render model A).
        class RbfxSceneRenderer final : public ISceneRenderer
        {
        public:
            explicit RbfxSceneRenderer(EngineState& state)
                : m_state(state)
            {
            }

            void OnSurfaceCreated(void* nativeWindowHandle, uint32_t width, uint32_t height) override;
            void OnSurfaceResized(uint32_t width, uint32_t height) override;
            void OnSurfaceAboutToBeDestroyed() override;
            bool IsSurfaceReady() const override { return m_state.m_initialized; }

            void BeginOverlayFrame(const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip) override;
            void SubmitLines(AZStd::span<const DebugVertex> vertices) override;
            void SubmitTriangles(AZStd::span<const DebugVertex> vertices) override;
            void SetDepthTest(bool enabled) override;
            void EndOverlayFrame() override;

        private:
            EngineState& m_state;
            bool m_depthTest = true;
            bool m_frameOpen = false;
        };

        //! Mirrors the rbfx Scene into AZ::Entity + EngineNodeComponent, both directions.
        class RbfxEntityMirror final : public IEntityMirror
        {
        public:
            explicit RbfxEntityMirror(EngineState& state)
                : m_state(state)
            {
            }

            void SyncToEditor(AZStd::vector<AZ::Entity*>& outEntities) override;
            void FinishSync() override;
            void OnEditorTransformChanged(AZ::EntityId entityId, const AZ::Transform& worldTm) override;
            void OnEditorPropertyChanged(AZ::EntityId entityId, const PropertyChange& change) override;
            AZ::EntityId CreateObject(const ObjectSpec& spec) override;
            void DestroyObject(AZ::EntityId entityId) override;
            bool SaveScene(const AZStd::string& path) override;
            AZ::Aabb GetWorldBounds(AZ::EntityId entityId) const override;
            bool RaycastNode(
                AZ::EntityId entityId,
                const AZ::Vector3& rayOrigin,
                const AZ::Vector3& rayDirection,
                bool& outHit,
                float& outDistance) const override;

        private:
            //! Build (or reuse) a mirror entity for one rbfx node and recurse its children.
            //! parentId is the mirror entity of the rbfx parent (invalid for the root).
            void MirrorNodeRecursive(
                Urho3D::Node* node, AZ::EntityId parentId, AZStd::vector<AZ::Entity*>& outEntities);

            //! Read a node's Serializable attributes into a typed PropertyBag.
            void ReadProperties(Urho3D::Node* node, PropertyBag& outBag) const;

            //! Resolve the rbfx node for a mirror entity via the live EngineNodeComponent's
            //! reflected node handle, or nullptr (warns on miss). This is the single node
            //! resolution path for every post-sync operation (bounds / transform / property /
            //! destroy): the editor entity context re-homes mirror entities into its prefab when
            //! taking ownership, so an entity-id -> node-id map can go stale, whereas the reflected
            //! handle survives that re-home. One truth source, no silent no-ops.
            Urho3D::Node* ResolveNode(AZ::EntityId entityId) const;

            EngineState& m_state;

            //! Mirror entity id -> rbfx node id, populated during SyncToEditor and consumed only by
            //! the synchronous FinishSync transform seeding in the same batch (before the entities
            //! can be re-homed). NOT a resolution path for later edits - use ResolveNode for those.
            AZStd::unordered_map<AZ::EntityId, AZ::u32> m_entityToNode;

            //! Child mirror entity -> parent mirror entity, recorded during SyncToEditor and
            //! applied in FinishSync (once the entities are activated) via TransformBus.
            AZStd::unordered_map<AZ::EntityId, AZ::EntityId> m_pendingParent;
        };

        //! Enumerates the rbfx project resource tree for the AssetBrowser (R1.6).
        class RbfxAssetSource final : public IAssetSource
        {
        public:
            explicit RbfxAssetSource(EngineState& state)
                : m_state(state)
            {
            }

            void EnumerateRoot(AZStd::vector<AssetEntryInfo>& out) override;
            void EnumerateChildren(const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out) override;
            QIcon GetThumbnail(const AssetEntryInfo& entry) override;

        private:
            //! List one directory's immediate children as asset entries (folders first).
            void EnumerateDirectory(const AZStd::string& dirPath, AZStd::vector<AssetEntryInfo>& out) const;

            EngineState& m_state;
        };

        EngineState m_state;
        RbfxSceneRenderer m_sceneRenderer{ m_state };
        RbfxEntityMirror m_entityMirror{ m_state };
        RbfxAssetSource m_assetSource{ m_state };
    };
} // namespace CrossEngineEditor
