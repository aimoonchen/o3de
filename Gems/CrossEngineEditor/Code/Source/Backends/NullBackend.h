/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Do-nothing engine backend (plan §6 阶段1).
//!
//! Lets the editor shell run end-to-end with no engine attached: every contract is
//! satisfied with an empty implementation. It is also the reference every real backend
//! (Godot / in-house) is measured against - if the editor works on NullBackend, the
//! decoupling holds.

#include <BackendAPI/IAssetSource.h>
#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/IEntityMirror.h>
#include <BackendAPI/ISceneRenderer.h>

#include <Viewport/GLDebugRenderer.h>

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

    private:
        class NullSceneRenderer final : public ISceneRenderer
        {
        public:
            void AttachToWindow(void* nativeWindowHandle, uint32_t width, uint32_t height) override;
            void Resize(uint32_t width, uint32_t height) override;
            void RenderFrame(const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip) override;
            void SubmitLines(AZStd::span<const DebugVertex> vertices) override;
            void SubmitTriangles(AZStd::span<const DebugVertex> vertices) override;
            void SetDepthTest(bool enabled) override;

            //! Release GL resources (viewport's GL context must be current).
            void ReleaseGraphics() override;
            //! Drop stale GL handles after the viewport rebuilt its context.
            void InvalidateGraphics() override;

        private:
            //! Lazily create GL resources on the viewport's current context.
            bool EnsureInitialized();

            GLDebugRenderer m_debugRenderer;
            uint32_t m_width = 1;
            uint32_t m_height = 1;
        };

        class NullEntityMirror final : public IEntityMirror
        {
        public:
            void SyncToEditor(AZStd::vector<AZ::Entity*>& outEntities) override;
            void OnEditorTransformChanged(AZ::EntityId entityId, const AZ::Transform& worldTm) override;
            void OnEditorPropertyChanged(AZ::EntityId entityId, const PropertyChange& change) override;
            AZ::EntityId CreateObject(const ObjectSpec& spec) override;
            void DestroyObject(AZ::EntityId entityId) override;
        };

        class NullAssetSource final : public IAssetSource
        {
        public:
            void EnumerateRoot(AZStd::vector<AssetEntryInfo>& out) override;
            void EnumerateChildren(const AssetEntryInfo& parent, AZStd::vector<AssetEntryInfo>& out) override;
            QIcon GetThumbnail(const AssetEntryInfo& entry) override;
        };

        NullSceneRenderer m_sceneRenderer;
        NullEntityMirror m_entityMirror;
        NullAssetSource m_assetSource;
    };
} // namespace CrossEngineEditor
