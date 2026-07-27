/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Minimal modern-GL implementation of the 3 IDebugRenderDevice primitives (plan §2.2/§3.2).
//!
//! Owns a single shader + a dynamic vertex buffer and draws world-space line/triangle
//! batches directly into the current OpenGL context (the editor viewport's QOpenGLWidget
//! surface). It is deliberately tiny: one MVP uniform, per-vertex color, GL_LINES /
//! GL_TRIANGLES. Blender-grade look comes from the surrounding GL state the viewport sets
//! up (MSAA + line smoothing + alpha blend), not from this class. Per-vertex colors are
//! written to the framebuffer as-is (linear, no sRGB encode) to match how AzToolsFramework
//! manipulators author their colors.
//!
//! All calls must happen with the owning GL context current.

#include <BackendAPI/BackendTypes.h>

#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/std/containers/span.h>

#if !defined(Q_MOC_RUN)
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#endif

#include <cstdint>
#include <memory>

namespace CrossEngineEditor
{
    class GLDebugRenderer final : protected QOpenGLFunctions
    {
    public:
        GLDebugRenderer() = default;
        ~GLDebugRenderer();

        //! One-time GL resource creation. Requires a current context. Safe to call again.
        bool Initialize();
        //! Release GL resources. Requires the owning context current.
        void Shutdown();

        //! Forget all GL object handles WITHOUT issuing GL calls. Call this when the owning
        //! QOpenGLWidget rebuilds its context (e.g. on reparent by the docking system): the
        //! old VAO/VBO/shader handles belong to a destroyed context and must not be touched.
        //! The next Initialize() then recreates everything against the new context.
        void InvalidateContext();

        //! Set the world->clip transform used for subsequent Submit* calls this frame.
        void SetViewProjection(const AZ::Matrix4x4& worldToClip);

        void SetDepthTest(bool enabled);
        void SubmitLines(AZStd::span<const DebugVertex> vertices);
        void SubmitTriangles(AZStd::span<const DebugVertex> vertices);

    private:
        void Draw(AZStd::span<const DebugVertex> vertices, uint32_t glPrimitive);

        bool m_initialized = false;
        // Heap-owned so InvalidateContext can drop the handles for a dead context without
        // issuing GL calls, and Initialize can build fresh objects for the new context.
        std::unique_ptr<QOpenGLShaderProgram> m_program;
        std::unique_ptr<QOpenGLBuffer> m_vbo;
        std::unique_ptr<QOpenGLVertexArrayObject> m_vao;

        int m_mvpUniformLocation = -1;
        AZ::Matrix4x4 m_worldToClip = AZ::Matrix4x4::CreateIdentity();
    };
} // namespace CrossEngineEditor
