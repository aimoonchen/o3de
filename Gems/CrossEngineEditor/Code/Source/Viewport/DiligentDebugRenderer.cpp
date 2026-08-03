/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/DiligentDebugRenderer.h>

#include <AzCore/std/containers/vector.h>

#include <cstring>

namespace CrossEngineEditor
{
    namespace
    {
        using namespace Diligent;

        // Interleaved layout: 3 floats position + 4 floats color (matches the input layout below).
        constexpr uint32_t k_positionComponents = 3;
        constexpr uint32_t k_colorComponents = 4;
        constexpr uint32_t k_floatsPerVertex = k_positionComponents + k_colorComponents;
        constexpr uint32_t k_vertexStride = k_floatsPerVertex * sizeof(float);

        //! Minimal MVP shader. Per-vertex color written to the target as-is (linear, no sRGB
        //! encode) to match how AzToolsFramework manipulators author their colors, exactly like
        //! the old GL renderer. HLSL so it compiles for D3D12/D3D11/Vulkan through Diligent.
        const char* const k_vertexShaderHLSL = R"(
cbuffer Constants
{
    float4x4 g_WorldToClip;
};
struct VSInput
{
    float3 Pos   : ATTRIB0;
    float4 Color : ATTRIB1;
};
struct PSInput
{
    float4 Pos   : SV_POSITION;
    float4 Color : COLOR0;
};
void main(in VSInput VSIn, out PSInput PSIn)
{
    PSIn.Pos   = mul(float4(VSIn.Pos, 1.0), g_WorldToClip);
    PSIn.Color = VSIn.Color;
}
)";

        const char* const k_pixelShaderHLSL = R"(
struct PSInput
{
    float4 Pos   : SV_POSITION;
    float4 Color : COLOR0;
};
struct PSOutput
{
    float4 Color : SV_TARGET;
};
void main(in PSInput PSIn, out PSOutput PSOut)
{
    PSOut.Color = PSIn.Color;
}
)";

        constexpr uint32_t k_initialVertexCapacity = 4096;
    } // namespace

    bool DiligentDebugRenderer::Initialize(
        IRenderDevice* device, IDeviceContext* context, TEXTURE_FORMAT rtvFormat, TEXTURE_FORMAT dsvFormat,
        uint8_t sampleCount)
    {
        if (m_initialized)
        {
            return true;
        }
        if (device == nullptr || context == nullptr)
        {
            return false;
        }
        m_device = device;
        m_context = context;

        // --- Shaders ---
        RefCntAutoPtr<IShader> vs;
        RefCntAutoPtr<IShader> ps;
        {
            ShaderCreateInfo ci;
            ci.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
            ci.Desc.UseCombinedTextureSamplers = true;

            ci.Desc.ShaderType = SHADER_TYPE_VERTEX;
            ci.Desc.Name = "CEE debug VS";
            ci.EntryPoint = "main";
            ci.Source = k_vertexShaderHLSL;
            m_device->CreateShader(ci, &vs);

            ci.Desc.ShaderType = SHADER_TYPE_PIXEL;
            ci.Desc.Name = "CEE debug PS";
            ci.EntryPoint = "main";
            ci.Source = k_pixelShaderHLSL;
            m_device->CreateShader(ci, &ps);
        }
        if (!vs || !ps)
        {
            m_device = nullptr;
            m_context = nullptr;
            return false;
        }

        // --- Constant buffer (world->clip mat4, updated every frame) ---
        {
            BufferDesc cb;
            cb.Name = "CEE WorldToClip CB";
            cb.Size = sizeof(float) * 16;
            cb.Usage = USAGE_DYNAMIC;
            cb.BindFlags = BIND_UNIFORM_BUFFER;
            cb.CPUAccessFlags = CPU_ACCESS_WRITE;
            m_device->CreateBuffer(cb, nullptr, &m_constantBuffer);
        }

        // --- Input layout: float3 position (ATTRIB0), float4 color (ATTRIB1), interleaved ---
        const LayoutElement layoutElements[] = {
            LayoutElement{ 0, 0, k_positionComponents, VT_FLOAT32, False },
            LayoutElement{ 1, 0, k_colorComponents, VT_FLOAT32, False },
        };

        auto createPSO = [&](PRIMITIVE_TOPOLOGY topology, bool depthTest, const char* name,
                             RefCntAutoPtr<IPipelineState>& outPso, RefCntAutoPtr<IShaderResourceBinding>& outSrb) -> bool
        {
            GraphicsPipelineStateCreateInfo pci;
            pci.PSODesc.Name = name;
            pci.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

            auto& gp = pci.GraphicsPipeline;
            gp.NumRenderTargets = 1;
            gp.RTVFormats[0] = rtvFormat;
            gp.DSVFormat = dsvFormat;
            gp.PrimitiveTopology = topology;
            gp.RasterizerDesc.CullMode = CULL_MODE_NONE;
            // Edge smoothing comes from the offscreen MSAA target (SampleCount 4); when MSAA is on we
            // must NOT also set AntialiasedLineEnable (illegal with MultisampleEnable on D3D). Only
            // fall back to the legacy 1px line-AA raster feature when rendering without MSAA.
            gp.SmplDesc.Count = sampleCount;
            const bool msaa = sampleCount > 1;
            gp.RasterizerDesc.AntialiasedLineEnable =
                (!msaa && topology == PRIMITIVE_TOPOLOGY_LINE_LIST) ? True : False;
            // Overlays only ever TEST depth, never WRITE it: gizmo/grid geometry must not pollute the
            // depth buffer for later scene passes, and translucent handles blend over each other.
            //   depthTest == true  -> depth-LE against the scene (occluded by geometry in front).
            //   depthTest == false -> depth-off "drawn-in-front" pass (Blender-style gizmos, always
            //                         visible over the scene). GenericDebugDisplay::Flush routes its
            //                         overlay bucket here via SetDepthTest(false).
            gp.DepthStencilDesc.DepthEnable = depthTest ? True : False;
            gp.DepthStencilDesc.DepthWriteEnable = False;
            gp.DepthStencilDesc.DepthFunc = COMPARISON_FUNC_LESS_EQUAL;

            // Alpha blend so themed gizmo highlights and translucent handles composite correctly.
            auto& rt0 = gp.BlendDesc.RenderTargets[0];
            rt0.BlendEnable = True;
            rt0.SrcBlend = BLEND_FACTOR_SRC_ALPHA;
            rt0.DestBlend = BLEND_FACTOR_INV_SRC_ALPHA;
            rt0.BlendOp = BLEND_OPERATION_ADD;
            rt0.SrcBlendAlpha = BLEND_FACTOR_ONE;
            rt0.DestBlendAlpha = BLEND_FACTOR_INV_SRC_ALPHA;
            rt0.BlendOpAlpha = BLEND_OPERATION_ADD;

            gp.InputLayout.LayoutElements = layoutElements;
            gp.InputLayout.NumElements = static_cast<Uint32>(AZ_ARRAY_SIZE(layoutElements));

            pci.pVS = vs;
            pci.pPS = ps;

            // The constant buffer is a STATIC resource - set once on the PSO, shared by all draws.
            pci.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

            m_device->CreateGraphicsPipelineState(pci, &outPso);
            if (!outPso)
            {
                return false;
            }
            if (auto* var = outPso->GetStaticVariableByName(SHADER_TYPE_VERTEX, "Constants"))
            {
                var->Set(m_constantBuffer);
            }
            outPso->CreateShaderResourceBinding(&outSrb, true);
            return outSrb != nullptr;
        };

        if (!createPSO(PRIMITIVE_TOPOLOGY_LINE_LIST, true, "CEE line PSO (depth)", m_linePSO[1], m_lineSRB[1]) ||
            !createPSO(PRIMITIVE_TOPOLOGY_LINE_LIST, false, "CEE line PSO (overlay)", m_linePSO[0], m_lineSRB[0]) ||
            !createPSO(PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, "CEE tri PSO (depth)", m_trianglePSO[1], m_triangleSRB[1]) ||
            !createPSO(PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, "CEE tri PSO (overlay)", m_trianglePSO[0], m_triangleSRB[0]))
        {
            Shutdown();
            return false;
        }

        if (!EnsureVertexCapacity(k_initialVertexCapacity))
        {
            Shutdown();
            return false;
        }

        m_initialized = true;
        return true;
    }

    void DiligentDebugRenderer::Shutdown()
    {
        for (auto& srb : m_lineSRB) { srb.Release(); }
        for (auto& srb : m_triangleSRB) { srb.Release(); }
        for (auto& pso : m_linePSO) { pso.Release(); }
        for (auto& pso : m_trianglePSO) { pso.Release(); }
        m_vertexBuffer.Release();
        m_constantBuffer.Release();
        m_vertexCapacity = 0;
        m_device = nullptr;
        m_context = nullptr;
        m_initialized = false;
    }

    void DiligentDebugRenderer::SetViewProjection(const AZ::Matrix4x4& worldToClip)
    {
        m_worldToClip = worldToClip;
    }

    void DiligentDebugRenderer::UploadViewProjection()
    {
        // world->clip is constant for the whole overlay frame, so upload it exactly once per frame
        // here (called from BeginOverlayFrame) instead of on every Draw (up to 4 redundant Maps).
        if (m_initialized)
        {
            UpdateConstantBuffer();
        }
    }

    bool DiligentDebugRenderer::EnsureVertexCapacity(uint32_t vertexCount)
    {
        if (vertexCount <= m_vertexCapacity && m_vertexBuffer)
        {
            return true;
        }
        // Grow geometrically to avoid per-frame reallocation churn.
        uint32_t newCapacity = m_vertexCapacity ? m_vertexCapacity : k_initialVertexCapacity;
        while (newCapacity < vertexCount)
        {
            newCapacity *= 2;
        }

        m_vertexBuffer.Release();
        BufferDesc vb;
        vb.Name = "CEE debug dynamic VB";
        vb.Size = static_cast<Uint64>(newCapacity) * k_vertexStride;
        vb.Usage = USAGE_DYNAMIC;
        vb.BindFlags = BIND_VERTEX_BUFFER;
        vb.CPUAccessFlags = CPU_ACCESS_WRITE;
        m_device->CreateBuffer(vb, nullptr, &m_vertexBuffer);
        if (!m_vertexBuffer)
        {
            m_vertexCapacity = 0;
            return false;
        }
        m_vertexCapacity = newCapacity;
        return true;
    }

    void DiligentDebugRenderer::UpdateConstantBuffer()
    {
        void* mapped = nullptr;
        m_context->MapBuffer(m_constantBuffer, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped == nullptr)
        {
            return;
        }
        // HLSL cbuffer float4x4 is column-major by default; Diligent's convention with row-vector
        // math (mul(v, M)) expects the matrix stored row-major so rows map to the 4 registers.
        m_worldToClip.StoreToRowMajorFloat16(static_cast<float*>(mapped));
        m_context->UnmapBuffer(m_constantBuffer, MAP_WRITE);
    }

    void DiligentDebugRenderer::SubmitLines(AZStd::span<const DebugVertex> vertices)
    {
        const size_t i = m_depthTestEnabled ? 1 : 0;
        Draw(vertices, m_linePSO[i], m_lineSRB[i]);
    }

    void DiligentDebugRenderer::SubmitTriangles(AZStd::span<const DebugVertex> vertices)
    {
        const size_t i = m_depthTestEnabled ? 1 : 0;
        Draw(vertices, m_trianglePSO[i], m_triangleSRB[i]);
    }

    void DiligentDebugRenderer::Draw(
        AZStd::span<const DebugVertex> vertices, IPipelineState* pso, IShaderResourceBinding* srb)
    {
        if (!m_initialized || vertices.empty() || pso == nullptr || srb == nullptr)
        {
            return;
        }
        const auto vertexCount = static_cast<uint32_t>(vertices.size());
        if (!EnsureVertexCapacity(vertexCount))
        {
            return;
        }

        // Stream the interleaved (pos3, color4) data into the dynamic VB.
        {
            void* mapped = nullptr;
            m_context->MapBuffer(m_vertexBuffer, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
            if (mapped == nullptr)
            {
                return;
            }
            auto* dst = static_cast<float*>(mapped);
            for (const DebugVertex& v : vertices)
            {
                *dst++ = v.m_position.GetX();
                *dst++ = v.m_position.GetY();
                *dst++ = v.m_position.GetZ();
                *dst++ = v.m_color.GetR();
                *dst++ = v.m_color.GetG();
                *dst++ = v.m_color.GetB();
                *dst++ = v.m_color.GetA();
            }
            m_context->UnmapBuffer(m_vertexBuffer, MAP_WRITE);
        }

        IBuffer* vbs[] = { m_vertexBuffer };
        const Uint64 offsets[] = { 0 };
        m_context->SetVertexBuffers(
            0, 1, vbs, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);

        m_context->SetPipelineState(pso);
        m_context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        DrawAttribs draw;
        draw.NumVertices = vertexCount;
        draw.Flags = DRAW_FLAG_VERIFY_ALL;
        m_context->Draw(draw);
    }
} // namespace CrossEngineEditor
