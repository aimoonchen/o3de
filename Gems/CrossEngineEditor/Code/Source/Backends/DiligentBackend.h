/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Diligent/D3D12 engine backend (plan §3, D3,阶段3).
//!
//! Owns a single Diligent IRenderDevice + immediate IDeviceContext and one ISwapChain bound to
//! the editor viewport's native window surface (HWND on Windows). Each editor frame it clears the
//! back buffer to the Blender-grey viewport color and draws the overlay geometry (gizmos / grid /
//! selection) produced by GenericDebugDisplay through DiligentDebugRenderer, then presents.
//!
//! Threading (see new_editor_plan §3 + O3DE AuxGeom double-buffer precedent):
//!   This first iteration presents on the Qt main thread (the idle-tick that also generates the
//!   overlay), so there is no cross-thread synchronization to get wrong while D3D12-direct-present
//!   is brought up. The ISceneRenderer surface contract (OnSurfaceCreated/Resized/AboutToBeDestroyed
//!   + PackViewportSize) and the BeginOverlayFrame/EndOverlayFrame snapshot bracket are already
//!   shaped so a dedicated render thread + lock-free input queue can be dropped in later without
//!   touching the viewport controller. The pending-resize is stored as a uint64 (w<<32|h) so that
//!   move is trivial.
//!
//! Entity-mirror and asset-source are delegated to an embedded NullBackend (no engine scene yet);
//! only the scene renderer is Diligent-specific here.

#include <BackendAPI/IEngineBackend.h>
#include <BackendAPI/ISceneRenderer.h>

#include <Backends/NullBackend.h>
#include <Viewport/DiligentDebugRenderer.h>

AZ_PUSH_DISABLE_WARNING(4251 4244 4267, "-Wunknown-warning-option")
#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/SwapChain.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
AZ_POP_DISABLE_WARNING

#include <atomic>
#include <cstdint>

namespace CrossEngineEditor
{
    class DiligentBackend final : public IEngineBackend
    {
    public:
        AZ_RTTI(DiligentBackend, "{6C4E1A22-3F7D-4B90-9E1C-2A8F0D5B6E30}", IEngineBackend);

        DiligentBackend() = default;
        ~DiligentBackend() override = default;

        std::expected<void, BackendError> Initialize(const BackendInitParams& params) override;
        void Shutdown() override;
        void Tick(float deltaSeconds) override;

        ISceneRenderer& GetSceneRenderer() override { return m_sceneRenderer; }
        IEntityMirror& GetEntityMirror() override { return m_null.GetEntityMirror(); }
        IAssetSource& GetAssetSource() override { return m_null.GetAssetSource(); }

    private:
        //! Diligent scene renderer bound to the viewport native surface.
        class DiligentSceneRenderer final : public ISceneRenderer
        {
        public:
            DiligentSceneRenderer() = default;
            ~DiligentSceneRenderer() override;

            void OnSurfaceCreated(void* nativeWindowHandle, uint32_t width, uint32_t height) override;
            void OnSurfaceResized(uint32_t width, uint32_t height) override;
            void OnSurfaceAboutToBeDestroyed() override;
            bool IsSurfaceReady() const override { return m_swapChain != nullptr; }

            void BeginOverlayFrame(const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip) override;
            void SubmitLines(AZStd::span<const DebugVertex> vertices) override;
            void SubmitTriangles(AZStd::span<const DebugVertex> vertices) override;
            void SetDepthTest(bool enabled) override;
            void EndOverlayFrame() override;

        private:
            //! Create device + immediate context (once) and the swapchain for this surface.
            bool CreateDeviceAndSwapChain(void* nativeWindowHandle, uint32_t width, uint32_t height);
            //! Apply a pending resize (packed w<<32|h) to the swapchain before rendering.
            void ApplyPendingResize();
            //! Release the swapchain (surface teardown) while keeping the device alive for reuse.
            void ReleaseSwapChain();
            //! (Re)create the 4x MSAA offscreen colour + depth textures at the given size. Called on
            //! surface create and after every swapchain resize so the MSAA target tracks the window.
            void CreateMsaaTargets(uint32_t width, uint32_t height);

            Diligent::RefCntAutoPtr<Diligent::IRenderDevice> m_device;
            Diligent::RefCntAutoPtr<Diligent::IDeviceContext> m_context;
            Diligent::RefCntAutoPtr<Diligent::ISwapChain> m_swapChain;

            //! 4x MSAA offscreen targets. The whole viewport (grid + gizmos + future scene) is drawn
            //! into these for hardware edge antialiasing, then resolved into the swapchain back buffer
            //! before Present - the standard editor-overlay AA path (UE/Unity/Godot all do this).
            Diligent::RefCntAutoPtr<Diligent::ITexture> m_msaaColor;
            Diligent::RefCntAutoPtr<Diligent::ITexture> m_msaaDepth;
            Diligent::RefCntAutoPtr<Diligent::ITextureView> m_msaaColorRTV;
            Diligent::RefCntAutoPtr<Diligent::ITextureView> m_msaaDepthDSV;

            DiligentDebugRenderer m_debugRenderer;

            //! Pending physical size packed as (w<<32)|h; 0 = none. Atomic so a future render
            //! thread can consume it lock-free (plan §4). Written by the main thread on resize.
            std::atomic<uint64_t> m_pendingResize{ 0 };

            bool m_frameOpen = false;
        };

        DiligentSceneRenderer m_sceneRenderer;
        //! Provides the entity-mirror + asset-source contracts until a real engine scene exists.
        NullBackend m_null;
    };
} // namespace CrossEngineEditor
