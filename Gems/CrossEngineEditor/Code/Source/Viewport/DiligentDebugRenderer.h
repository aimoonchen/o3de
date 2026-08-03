/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Diligent implementation of the 3 IDebugRenderDevice primitives (plan §3).
//!
//! Owns two graphics PSOs (line-list + triangle-list) that share one MVP vertex shader, a
//! single dynamic vertex buffer, and a dynamic constant buffer holding the world->clip matrix.
//! Overlay geometry (gizmos/grid/selection) tessellated by GenericDebugDisplay is submitted as
//! world-space line/triangle batches and drawn straight into the swapchain's back buffer.
//!
//! All calls run on whichever thread owns the Diligent immediate context (the render thread in
//! DiligentBackend). It draws into the RTV/DSV handed to BeginFrame; it does NOT own the swapchain.

#include <BackendAPI/BackendTypes.h>

#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/std/containers/span.h>

// Diligent smart pointer + interfaces (facade target adds DiligentCore to the include path).
AZ_PUSH_DISABLE_WARNING(4251 4244 4267, "-Wunknown-warning-option")
#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceBinding.h"
#include "Graphics/GraphicsEngine/interface/GraphicsTypes.h"
AZ_POP_DISABLE_WARNING

#include <cstdint>

namespace CrossEngineEditor
{
    class DiligentDebugRenderer final
    {
    public:
        DiligentDebugRenderer() = default;
        ~DiligentDebugRenderer() = default;

        DiligentDebugRenderer(const DiligentDebugRenderer&) = delete;
        DiligentDebugRenderer& operator=(const DiligentDebugRenderer&) = delete;

        //! Create the PSOs, shaders, vertex/constant buffers. rtvFormat/dsvFormat must match the
        //! render targets the caller will draw into, and sampleCount their MSAA sample count (1 =
        //! no MSAA, 4 = the editor's 4x MSAA offscreen target). Returns false on shader/PSO failure.
        bool Initialize(
            Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
            Diligent::TEXTURE_FORMAT rtvFormat, Diligent::TEXTURE_FORMAT dsvFormat, uint8_t sampleCount = 1);

        //! Release all GPU resources (call with the render thread's context idle).
        void Shutdown();

        [[nodiscard]] bool IsInitialized() const { return m_initialized; }

        //! Set the world->clip transform used for subsequent Submit* calls this frame.
        void SetViewProjection(const AZ::Matrix4x4& worldToClip);

        //! Upload the current world->clip transform to the GPU once per frame. Call this after
        //! SetViewProjection and before the Submit* calls (see DiligentBackend::BeginOverlayFrame),
        //! so the constant buffer is Mapped once instead of on every Draw.
        void UploadViewProjection();

        //! enabled == true  -> depth-tested pass (occluded by scene geometry in front).
        //! enabled == false -> depth-off "drawn-in-front" overlay pass (Blender-style gizmos).
        void SetDepthTest(bool enabled) { m_depthTestEnabled = enabled; }
        void SubmitLines(AZStd::span<const DebugVertex> vertices);
        void SubmitTriangles(AZStd::span<const DebugVertex> vertices);

    private:
        //! Upload the world->clip matrix into the constant buffer (MAP_WRITE + DISCARD).
        void UpdateConstantBuffer();
        //! Stream vertices into the dynamic VB (growing it if needed) and draw with the given PSO/SRB.
        void Draw(AZStd::span<const DebugVertex> vertices,
                  Diligent::IPipelineState* pso, Diligent::IShaderResourceBinding* srb);
        //! Ensure the dynamic vertex buffer holds at least vertexCount DebugVertex slots.
        bool EnsureVertexCapacity(uint32_t vertexCount);

        bool m_initialized = false;
        bool m_depthTestEnabled = true;

        // Not owned - the backend owns device + immediate context lifetime.
        Diligent::IRenderDevice* m_device = nullptr;
        Diligent::IDeviceContext* m_context = nullptr;

        // Index [1] = depth-tested variant, index [0] = depth-off overlay variant (matches
        // SetDepthTest(bool) -> size_t indexing in the .cpp).
        Diligent::RefCntAutoPtr<Diligent::IPipelineState> m_linePSO[2];
        Diligent::RefCntAutoPtr<Diligent::IPipelineState> m_trianglePSO[2];
        Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> m_lineSRB[2];
        Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> m_triangleSRB[2];
        Diligent::RefCntAutoPtr<Diligent::IBuffer> m_vertexBuffer;
        Diligent::RefCntAutoPtr<Diligent::IBuffer> m_constantBuffer;

        uint32_t m_vertexCapacity = 0; //!< Current VB capacity in DebugVertex units.
        AZ::Matrix4x4 m_worldToClip = AZ::Matrix4x4::CreateIdentity();
    };
} // namespace CrossEngineEditor
