/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Do-nothing engine backend (Plan §B1).
//!
//! Lets the editor shell run end-to-end with no engine attached: every contract is
//! satisfied with an empty implementation. It is also the reference every real backend
//! (rbfx / Godot / in-house) is measured against - if the editor works on NullBackend, the
//! decoupling holds (Plan §A6 C2 removability verification).

#include <BackendAPI/IAssetSource.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>
#include <BackendAPI/IMaterialSource.h>
#include <BackendAPI/ISceneRenderer.h>
#include <MaterialEditor/NullMaterialSource.h>

namespace CrossEngineEditor
{
    class NullBackend final : public IEngineBackend
    {
    public:
        AZ_RTTI(NullBackend, "{1D9A6C77-4E2B-4C0F-8B3A-9C6F2E4A7B10}", IEngineBackend);

        std::expected<void, BackendError> Initialize(const BackendInitParams& params) override;
        void Shutdown() override;
        void Tick(float deltaSeconds) override;

        ISceneRenderer& GetSceneRenderer() override { return m_sceneRenderer; }
        IEntityMirror& GetEntityMirror() override { return m_entityMirror; }
        IAssetSource& GetAssetSource() override { return m_assetSource; }
        IMaterialSource& GetMaterialSource() override { return m_materialSource; }

    private:
        class NullSceneRenderer final : public ISceneRenderer
        {
        public:
            // Native surface lifecycle. The Null backend has no RHI/swapchain, so it never
            // becomes "surface ready" and draws nothing. Two uses: the fall-back when the
            // Diligent backend is not built (CrossEngineEditorApplication::CreateBackendFromCommandLine),
            // and the entity-mirror/asset-source delegate embedded in DiligentBackend (no engine
            // scene yet). The editor logic (camera/picking) still runs.
            void OnSurfaceCreated(void* nativeWindowHandle, uint32_t width, uint32_t height) override;
            void OnSurfaceResized(uint32_t width, uint32_t height) override;
            void OnSurfaceAboutToBeDestroyed() override;
            bool IsSurfaceReady() const override { return false; }

            void BeginOverlayFrame(const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip) override;
            void SubmitLines(AZStd::span<const DebugVertex> vertices) override;
            void SubmitTriangles(AZStd::span<const DebugVertex> vertices) override;
            void SetDepthTest(bool enabled) override;
            void EndOverlayFrame() override;

        private:
            uint32_t m_width = 1;
            uint32_t m_height = 1;
        };

        class NullEntityMirror final : public IEntityMirror
        {
        public:
            void SyncToEditor(AZStd::vector<AZ::Entity*>& outEntities) override;
            void EnumerateObjectTypes(AZStd::vector<ObjectTypeInfo>& out) override;
            void OnEditorTransformChanged(AZ::EntityId entityId, const AZ::Transform& worldTm) override;
            void OnEditorPropertyChanged(AZ::EntityId entityId) override;
            AZ::EntityId CreateObject(const ObjectSpec& spec) override;
            void DestroyObject(AZ::EntityId entityId) override;
            bool RaycastScene(
                const AZ::Vector3& rayOrigin,
                const AZ::Vector3& rayDirection,
                AZ::Vector3& outHitPoint,
                AZ::Vector3& outHitNormal) const override;
            bool CreatePrefabFromNodes(
                const AZStd::vector<AZ::EntityId>& entityIds,
                const AZStd::string& path) override;
            bool AssignMaterial(AZ::EntityId entityId, const AZStd::string& assetPath, int slot) override;
            bool AssignAnimation(AZ::EntityId entityId, const AZStd::string& assetPath) override;
            AZStd::vector<AZ::u8> SerializeNodes(const AZStd::vector<AZ::EntityId>& entityIds) override;
            bool PasteNodes(const AZStd::vector<AZ::u8>& data, AZ::EntityId parentId) override;
        };

        class NullAssetSource final : public IAssetSource
        {
        public:
            void EnumerateRoot(AZStd::vector<AssetEntryInfo>& out) override;
            void EnumerateChildren(const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out) override;
        };

        NullSceneRenderer m_sceneRenderer;
        NullEntityMirror m_entityMirror;
        NullAssetSource m_assetSource;
        NullMaterialSource m_materialSource;
    };
} // namespace CrossEngineEditor
