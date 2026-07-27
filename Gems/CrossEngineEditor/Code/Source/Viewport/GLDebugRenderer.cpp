/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/GLDebugRenderer.h>

#include <AzCore/std/containers/vector.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QMatrix4x4>
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    namespace
    {
        // Interleaved layout matches DebugVertex: 3 floats position + 4 floats color.
        constexpr int PositionComponents = 3;
        constexpr int ColorComponents = 4;
        constexpr int FloatsPerVertex = PositionComponents + ColorComponents;

        const char* const VertexShaderSource = R"(
            #version 330 core
            layout(location = 0) in vec3 in_position;
            layout(location = 1) in vec4 in_color;
            uniform mat4 u_worldToClip;
            out vec4 v_color;
            void main()
            {
                v_color = in_color;
                gl_Position = u_worldToClip * vec4(in_position, 1.0);
            }
        )";

        const char* const FragmentShaderSource = R"(
            #version 330 core
            in vec4 v_color;
            out vec4 out_color;
            void main()
            {
                out_color = v_color;
            }
        )";
    } // namespace

    GLDebugRenderer::~GLDebugRenderer()
    {
        // Shutdown must be driven explicitly while the context is current; nothing to do here.
    }

    bool GLDebugRenderer::Initialize()
    {
        if (m_initialized)
        {
            return true;
        }

        initializeOpenGLFunctions();

        // Build fresh GL objects for the current context.
        m_program = std::make_unique<QOpenGLShaderProgram>();
        m_vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        m_vao = std::make_unique<QOpenGLVertexArrayObject>();

        if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, VertexShaderSource) ||
            !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, FragmentShaderSource) ||
            !m_program->link())
        {
            return false;
        }

        m_mvpUniformLocation = m_program->uniformLocation("u_worldToClip");

        if (!m_vao->create())
        {
            return false;
        }
        if (!m_vbo->create())
        {
            return false;
        }

        m_vao->bind();
        m_vbo->bind();
        m_vbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);

        const int stride = FloatsPerVertex * static_cast<int>(sizeof(float));
        m_program->enableAttributeArray(0);
        m_program->setAttributeBuffer(0, GL_FLOAT, 0, PositionComponents, stride);
        m_program->enableAttributeArray(1);
        m_program->setAttributeBuffer(1, GL_FLOAT, PositionComponents * static_cast<int>(sizeof(float)), ColorComponents, stride);

        m_vbo->release();
        m_vao->release();

        m_initialized = true;
        return true;
    }

    void GLDebugRenderer::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }
        m_vbo->destroy();
        m_vao->destroy();
        m_program->removeAllShaders();
        m_program.reset();
        m_vbo.reset();
        m_vao.reset();
        m_initialized = false;
    }

    void GLDebugRenderer::InvalidateContext()
    {
        // The old context is gone; drop the wrapper objects WITHOUT calling destroy()
        // (which would issue GL on a dead context). Leaked GPU handles die with the context.
        (void)m_program.release();
        (void)m_vbo.release();
        (void)m_vao.release();
        m_program.reset();
        m_vbo.reset();
        m_vao.reset();
        m_mvpUniformLocation = -1;
        m_initialized = false;
    }

    void GLDebugRenderer::SetViewProjection(const AZ::Matrix4x4& worldToClip)
    {
        m_worldToClip = worldToClip;
    }

    void GLDebugRenderer::SetDepthTest(bool enabled)
    {
        if (enabled)
        {
            glEnable(GL_DEPTH_TEST);
        }
        else
        {
            glDisable(GL_DEPTH_TEST);
        }
    }

    void GLDebugRenderer::SubmitLines(AZStd::span<const DebugVertex> vertices)
    {
        Draw(vertices, GL_LINES);
    }

    void GLDebugRenderer::SubmitTriangles(AZStd::span<const DebugVertex> vertices)
    {
        Draw(vertices, GL_TRIANGLES);
    }

    void GLDebugRenderer::Draw(AZStd::span<const DebugVertex> vertices, uint32_t glPrimitive)
    {
        if (!m_initialized || vertices.empty())
        {
            return;
        }

        AZStd::vector<float> interleaved;
        interleaved.reserve(vertices.size() * FloatsPerVertex);
        for (const DebugVertex& vertex : vertices)
        {
            interleaved.push_back(vertex.m_position.GetX());
            interleaved.push_back(vertex.m_position.GetY());
            interleaved.push_back(vertex.m_position.GetZ());
            interleaved.push_back(vertex.m_color.GetR());
            interleaved.push_back(vertex.m_color.GetG());
            interleaved.push_back(vertex.m_color.GetB());
            interleaved.push_back(vertex.m_color.GetA());
        }

        // QMatrix4x4(const float*) reads row-major; AZ::Matrix4x4 stores row-major to match.
        float clipRowMajor[16];
        m_worldToClip.StoreToRowMajorFloat16(clipRowMajor);

        m_program->bind();
        m_program->setUniformValue(m_mvpUniformLocation, QMatrix4x4(clipRowMajor));

        m_vao->bind();
        m_vbo->bind();
        m_vbo->allocate(interleaved.data(), static_cast<int>(interleaved.size() * sizeof(float)));

        glDrawArrays(glPrimitive, 0, static_cast<GLsizei>(vertices.size()));

        m_vbo->release();
        m_vao->release();
        m_program->release();
    }
} // namespace CrossEngineEditor
