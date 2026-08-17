/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Backends/DiligentBackend.h>

AZ_PUSH_DISABLE_WARNING(4251 4244 4267, "-Wunknown-warning-option")
#include "Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "Platforms/interface/NativeWindow.h"
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    using namespace Diligent;

    namespace
    {
        //! Blender-inspired viewport clear color (neutral warm grey), sRGB-encoded target expects
        //! linear input; keep the same value the GL path used for visual parity.
        constexpr float k_clearColor[4] = { 0.231f, 0.231f, 0.231f, 1.0f };

        constexpr TEXTURE_FORMAT k_colorFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
        constexpr TEXTURE_FORMAT k_depthFormat = TEX_FORMAT_D32_FLOAT;

        //! 4x MSAA for the whole viewport (grid + gizmos). 4 is the universally-supported sweet spot
        //! for editor overlays (D3D/Vulkan guarantee 4x); higher yields diminishing returns for lines.
        constexpr uint8_t k_msaaSampleCount = 4;
    } // namespace

    // =====================================================================================
    // DiligentBackend
    // =====================================================================================

    std::expected<void, BackendError> DiligentBackend::Initialize(const BackendInitParams& params)
    {
        // Device + swapchain are created lazily on first surface expose (Plan §B2); the entity /
        // asset delegates just need their own init.
        return m_null.Initialize(params);
    }

    void DiligentBackend::Shutdown()
    {
        m_null.Shutdown();
    }

    void DiligentBackend::Tick(float deltaSeconds)
    {
        m_null.Tick(deltaSeconds);
    }

    // =====================================================================================
    // DiligentSceneRenderer
    // =====================================================================================

    DiligentBackend::DiligentSceneRenderer::~DiligentSceneRenderer()
    {
        OnSurfaceAboutToBeDestroyed();
    }

    bool DiligentBackend::DiligentSceneRenderer::CreateDeviceAndSwapChain(
        void* nativeWindowHandle, uint32_t width, uint32_t height)
    {
        if (nativeWindowHandle == nullptr)
        {
            return false;
        }

        // --- Device + immediate context (created once, reused across surfaces) ---
        if (!m_device)
        {
#if defined(_WIN32)
            auto* factory = GetEngineFactoryD3D12();
            if (factory == nullptr)
            {
                return false;
            }
            // The D3D12 dll must be loadable; LoadD3D12 is invoked internally by the factory.
            EngineD3D12CreateInfo engineCI;
            factory->CreateDeviceAndContextsD3D12(engineCI, &m_device, &m_context);
            if (!m_device || !m_context)
            {
                return false;
            }
#else
            // Non-Windows RHIs are wired the same way (Vulkan/Metal factory) - out of scope for the
            // first D3D12 bring-up. Leave the device null so IsSurfaceReady stays false.
            return false;
#endif
        }

        // --- Swapchain bound to the native window surface ---
        SwapChainDesc scDesc;
        scDesc.Width = AZStd::max(1u, width);
        scDesc.Height = AZStd::max(1u, height);
        scDesc.ColorBufferFormat = k_colorFormat;
        scDesc.DepthBufferFormat = k_depthFormat;
        scDesc.BufferCount = 3; // FLIP_DISCARD triple buffering (Plan §B2)

#if defined(_WIN32)
        auto* factory = GetEngineFactoryD3D12();
        Win32NativeWindow win32Window{ nativeWindowHandle };
        NativeWindow nativeWindow{ win32Window };
        factory->CreateSwapChainD3D12(
            m_device, m_context, scDesc, FullScreenModeDesc{}, nativeWindow, &m_swapChain);
#endif
        if (!m_swapChain)
        {
            return false;
        }

        // Build the overlay renderer's PSOs against the MSAA target formats (SampleCount 4).
        if (!m_debugRenderer.IsInitialized())
        {
            if (!m_debugRenderer.Initialize(m_device, m_context, k_colorFormat, k_depthFormat, k_msaaSampleCount))
            {
                ReleaseSwapChain();
                return false;
            }
        }

        // Offscreen 4x MSAA colour + depth the whole viewport renders into (resolved before Present).
        CreateMsaaTargets(scDesc.Width, scDesc.Height);
        return true;
    }

    void DiligentBackend::DiligentSceneRenderer::CreateMsaaTargets(uint32_t width, uint32_t height)
    {
        m_msaaColorRTV.Release();
        m_msaaDepthDSV.Release();
        m_msaaColor.Release();
        m_msaaDepth.Release();
        if (!m_device)
        {
            return;
        }

        TextureDesc colorDesc;
        colorDesc.Name = "CEE MSAA colour";
        colorDesc.Type = RESOURCE_DIM_TEX_2D;
        colorDesc.Width = AZStd::max(1u, width);
        colorDesc.Height = AZStd::max(1u, height);
        colorDesc.Format = k_colorFormat;
        colorDesc.SampleCount = k_msaaSampleCount;
        colorDesc.BindFlags = BIND_RENDER_TARGET;
        colorDesc.Usage = USAGE_DEFAULT;
        // Optimised clear value must match the per-frame clear so the driver can fast-clear.
        colorDesc.ClearValue.Format = k_colorFormat;
        colorDesc.ClearValue.Color[0] = k_clearColor[0];
        colorDesc.ClearValue.Color[1] = k_clearColor[1];
        colorDesc.ClearValue.Color[2] = k_clearColor[2];
        colorDesc.ClearValue.Color[3] = k_clearColor[3];
        m_device->CreateTexture(colorDesc, nullptr, &m_msaaColor);

        TextureDesc depthDesc;
        depthDesc.Name = "CEE MSAA depth";
        depthDesc.Type = RESOURCE_DIM_TEX_2D;
        depthDesc.Width = colorDesc.Width;
        depthDesc.Height = colorDesc.Height;
        depthDesc.Format = k_depthFormat;
        depthDesc.SampleCount = k_msaaSampleCount;
        depthDesc.BindFlags = BIND_DEPTH_STENCIL;
        depthDesc.Usage = USAGE_DEFAULT;
        depthDesc.ClearValue.Format = k_depthFormat;
        depthDesc.ClearValue.DepthStencil.Depth = 1.0f;
        m_device->CreateTexture(depthDesc, nullptr, &m_msaaDepth);

        if (m_msaaColor && m_msaaDepth)
        {
            m_msaaColorRTV = m_msaaColor->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
            m_msaaDepthDSV = m_msaaDepth->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);
        }
    }

    void DiligentBackend::DiligentSceneRenderer::OnSurfaceCreated(
        void* nativeWindowHandle, uint32_t width, uint32_t height)
    {
        if (m_swapChain)
        {
            // Re-expose after a hide/reparent: just resize the existing swapchain.
            m_pendingResize.store(PackViewportSize(width, height), std::memory_order_release);
            return;
        }
        CreateDeviceAndSwapChain(nativeWindowHandle, width, height);
    }

    void DiligentBackend::DiligentSceneRenderer::OnSurfaceResized(uint32_t width, uint32_t height)
    {
        // Non-blocking: stash the packed size; ApplyPendingResize consumes it on the next frame
        // (this is exactly the hand-off point a dedicated render thread would use, Plan §B8 B1).
        m_pendingResize.store(PackViewportSize(width, height), std::memory_order_release);
    }

    void DiligentBackend::DiligentSceneRenderer::ApplyPendingResize()
    {
        const uint64_t packed = m_pendingResize.exchange(0, std::memory_order_acq_rel);
        if (packed == 0 || !m_swapChain)
        {
            return;
        }
        const uint32_t w = AZStd::max(1u, UnpackViewportWidth(packed));
        const uint32_t h = AZStd::max(1u, UnpackViewportHeight(packed));
        m_swapChain->Resize(w, h);
        // MSAA target must track the window size or the resolve blit mismatches.
        CreateMsaaTargets(w, h);
    }

    void DiligentBackend::DiligentSceneRenderer::ReleaseSwapChain()
    {
        if (m_context)
        {
            m_context->Flush();
            m_context->WaitForIdle();
        }
        m_msaaColorRTV.Release();
        m_msaaDepthDSV.Release();
        m_msaaColor.Release();
        m_msaaDepth.Release();
        m_swapChain.Release();
    }

    void DiligentBackend::DiligentSceneRenderer::OnSurfaceAboutToBeDestroyed()
    {
        // Called on the Qt main thread via DirectConnection: release the swapchain synchronously
        // before the native surface is gone so D3D12 raises no validation error (Plan §B2).
        if (m_debugRenderer.IsInitialized())
        {
            m_debugRenderer.Shutdown();
        }
        ReleaseSwapChain();
        // Keep the device/context so a later re-expose can rebuild cheaply; drop them only if we
        // are being fully destroyed (the RefCntAutoPtr members release in the destructor chain).
    }

    void DiligentBackend::DiligentSceneRenderer::BeginOverlayFrame(
        const AZ::Matrix4x4& worldToView, const AZ::Matrix4x4& viewToClip)
    {
        if (!m_swapChain || !m_context)
        {
            return;
        }
        ApplyPendingResize();

        // The whole viewport renders into the 4x MSAA offscreen targets (resolved to the back
        // buffer in EndOverlayFrame). The overlay PSOs are compiled for SampleCount=4, so there is
        // NO valid single-sample fallback: drawing them into the sample=1 back buffer is a
        // validation error. If the MSAA target is missing, skip this frame rather than draw broken.
        if (!m_msaaColorRTV || !m_msaaDepthDSV)
        {
            return;
        }
        ITextureView* rtv = m_msaaColorRTV.RawPtr();
        ITextureView* dsv = m_msaaDepthDSV.RawPtr();
        ITextureView* rtvs[] = { rtv };
        m_context->SetRenderTargets(1, rtvs, dsv, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_context->ClearRenderTarget(rtv, k_clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_context->ClearDepthStencil(dsv, CLEAR_DEPTH_FLAG, 1.0f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // world->clip = viewToClip * worldToView (row-vector convention: v * M).
        m_debugRenderer.SetViewProjection(viewToClip * worldToView);
        m_debugRenderer.UploadViewProjection(); // upload once here, not per-Draw
        m_frameOpen = true;
    }

    void DiligentBackend::DiligentSceneRenderer::SubmitLines(AZStd::span<const DebugVertex> vertices)
    {
        if (m_frameOpen)
        {
            m_debugRenderer.SubmitLines(vertices);
        }
    }

    void DiligentBackend::DiligentSceneRenderer::SubmitTriangles(AZStd::span<const DebugVertex> vertices)
    {
        if (m_frameOpen)
        {
            m_debugRenderer.SubmitTriangles(vertices);
        }
    }

    void DiligentBackend::DiligentSceneRenderer::SetDepthTest(bool enabled)
    {
        m_debugRenderer.SetDepthTest(enabled);
    }

    void DiligentBackend::DiligentSceneRenderer::EndOverlayFrame()
    {
        if (!m_frameOpen || !m_swapChain)
        {
            m_frameOpen = false;
            return;
        }
        // Resolve the 4x MSAA colour into the swapchain back buffer, then present. m_msaaColor is
        // always valid here: BeginOverlayFrame returns early (frame skipped) when it is not.
        if (m_msaaColor)
        {
            ITexture* backBuffer = m_swapChain->GetCurrentBackBufferRTV()->GetTexture();
            ResolveTextureSubresourceAttribs resolve;
            resolve.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            resolve.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            m_context->ResolveTextureSubresource(m_msaaColor, backBuffer, resolve);
        }

        // Present with vsync. On a dedicated render thread this Present + the Submit* draws move
        // off the Qt thread; here they run inline on the idle tick (confirmed model).
        m_swapChain->Present(1);
        m_frameOpen = false;
    }
} // namespace CrossEngineEditor
