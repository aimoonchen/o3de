/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/GenericDebugDisplay.h>
#include <BackendAPI/ISceneRenderer.h>

#include <AzCore/Math/MathUtils.h>
#include <AzCore/Math/Vector2.h>

#include <AzFramework/Viewport/CameraState.h>
#include <AzFramework/Viewport/ViewportScreen.h>

namespace CrossEngineEditor
{
    namespace
    {
        //! Blender-style axis palette: distinct, slightly desaturated R/G/B. The
        //! manipulators pick their own colors per axis; these are only fallbacks.
        constexpr float DegToRad(float degrees)
        {
            return degrees * (AZ::Constants::Pi / 180.0f);
        }

        AZ::Vector3 AxisVector(int axis)
        {
            switch (axis)
            {
            case 0:
                return AZ::Vector3::CreateAxisX();
            case 1:
                return AZ::Vector3::CreateAxisY();
            default:
                return AZ::Vector3::CreateAxisZ();
            }
        }
    } // namespace

    GenericDebugDisplay::GenericDebugDisplay()
    {
        m_transformStack.push_back(AZ::Matrix3x4::CreateIdentity());
    }

    void GenericDebugDisplay::ClearFrame()
    {
        m_depthLines.clear();
        m_depthTriangles.clear();
        m_overlayLines.clear();
        m_overlayTriangles.clear();
        m_textLabels.clear();

        m_color = AZ::Colors::White;
        m_lineWidth = 1.0f;
        m_depthTest = true;
        m_nextLabelBackgroundBlock = false;
        m_headerText.clear();
        m_transformStack.clear();
        m_transformStack.push_back(AZ::Matrix3x4::CreateIdentity());
    }

    void GenericDebugDisplay::Flush(ISceneRenderer& renderer, const AzFramework::CameraState& cameraState)
    {
        // Expand wide line segments into camera-facing quads (constant on-screen width) and keep
        // the thin 1px ones as a cheap LINE_LIST. Wide-quad triangles are appended to the matching
        // triangle batch so they share the triangle PSO. Local copies keep ClearFrame simple.
        AZStd::vector<DebugVertex> depthThinLines;
        AZStd::vector<DebugVertex> depthTriangles = m_depthTriangles;
        ExpandLineSegments(m_depthLines, cameraState, depthThinLines, depthTriangles);

        AZStd::vector<DebugVertex> overlayThinLines;
        AZStd::vector<DebugVertex> overlayTriangles = m_overlayTriangles;
        ExpandLineSegments(m_overlayLines, cameraState, overlayThinLines, overlayTriangles);

        // Depth-tested pass first (occluded by the scene), then the drawn-in-front
        // overlay pass so gizmos stay visible - matching Blender's gizmo compositing.
        renderer.SetDepthTest(true);
        if (!depthTriangles.empty())
        {
            renderer.SubmitTriangles(depthTriangles);
        }
        if (!depthThinLines.empty())
        {
            renderer.SubmitLines(depthThinLines);
        }

        renderer.SetDepthTest(false);
        if (!overlayTriangles.empty())
        {
            renderer.SubmitTriangles(overlayTriangles);
        }
        if (!overlayThinLines.empty())
        {
            renderer.SubmitLines(overlayThinLines);
        }
    }

    void GenericDebugDisplay::ExpandLineSegments(
        const AZStd::vector<DebugLineSegment>& segments, const AzFramework::CameraState& cameraState,
        AZStd::vector<DebugVertex>& outThinLines, AZStd::vector<DebugVertex>& outTriangles) const
    {
        // A segment wider than this is drawn as a quad; at or below it stays a hardware 1px line.
        constexpr float k_thinThresholdPx = 1.5f;
        // Wide segments are projected to screen and unprojected back, which is only valid in front of
        // the camera. Clip each wide segment to the near plane first: WorldToScreen divides by w with
        // no guard (ViewportScreen.cpp), so a point behind the camera folds/explodes and paints a
        // full-screen quad. Blender's constraint line runs pivot+/-1000 so one end is always behind.
        const AZ::Vector3 forward = cameraState.m_forward.GetNormalizedSafe();
        const float nearZ = cameraState.m_nearClip + 1e-3f;

        for (const DebugLineSegment& seg : segments)
        {
            if (seg.m_widthPx <= k_thinThresholdPx)
            {
                outThinLines.push_back(DebugVertex{ seg.m_start, seg.m_startColor });
                outThinLines.push_back(DebugVertex{ seg.m_end, seg.m_endColor });
                continue;
            }

            // Near-plane clip (camera-space depth d = (p - camPos).forward).
            AZ::Vector3 start = seg.m_start;
            AZ::Vector3 end = seg.m_end;
            AZ::Color startColor = seg.m_startColor;
            AZ::Color endColor = seg.m_endColor;
            const float d0 = (start - cameraState.m_position).Dot(forward);
            const float d1 = (end - cameraState.m_position).Dot(forward);
            if (d0 < nearZ && d1 < nearZ)
            {
                continue; // wholly behind the near plane.
            }
            if (d0 < nearZ || d1 < nearZ)
            {
                const float t = (nearZ - d0) / (d1 - d0);
                const AZ::Vector3 hit = start + (end - start) * t;
                const AZ::Color hitColor = startColor.Lerp(endColor, t);
                if (d0 < nearZ)
                {
                    start = hit;
                    startColor = hitColor;
                }
                else
                {
                    end = hit;
                    endColor = hitColor;
                }
            }

            // Project both endpoints to screen, offset perpendicular to the screen-space segment
            // direction by half the width (in pixels), then unproject each of the four quad corners
            // back to world at the endpoint's own depth so the ribbon keeps a constant pixel width
            // regardless of distance/projection (Blender polyline shader approach).
            const AzFramework::ScreenPoint s0 = AzFramework::WorldToScreen(start, cameraState);
            const AzFramework::ScreenPoint s1 = AzFramework::WorldToScreen(end, cameraState);
            const AZ::Vector2 p0(static_cast<float>(s0.m_x), static_cast<float>(s0.m_y));
            const AZ::Vector2 p1(static_cast<float>(s1.m_x), static_cast<float>(s1.m_y));

            AZ::Vector2 dir = p1 - p0;
            const float len = dir.GetLength();
            if (len < AZ::Constants::FloatEpsilon)
            {
                continue;
            }
            dir = dir / len;
            const AZ::Vector2 normal(-dir.GetY(), dir.GetX());
            const float half = 0.5f * seg.m_widthPx;
            const AZ::Vector2 off = normal * half;

            // Unproject a pixel offset around a screen point back to world at the given reference
            // world point's depth (keeps the ribbon co-planar with the segment).
            auto cornerWorld = [&](const AZ::Vector2& screenPt, const AZ::Vector3& refWorld)
            {
                const AzFramework::ScreenPoint sp(
                    static_cast<int>(screenPt.GetX()), static_cast<int>(screenPt.GetY()));
                // ScreenToWorld yields a ray point on the near plane; project it so it sits at the
                // reference world point's distance along the view direction for stable depth.
                const AZ::Vector3 nearPt = AzFramework::ScreenToWorld(sp, cameraState);
                const AZ::Vector3 rayDir = (nearPt - cameraState.m_position).GetNormalizedSafe();
                const float refDist = (refWorld - cameraState.m_position).Dot(forward);
                const float cosTheta = rayDir.Dot(forward);
                const float t = (AZ::GetAbs(cosTheta) > AZ::Constants::FloatEpsilon) ? (refDist / cosTheta) : refDist;
                return cameraState.m_position + rayDir * t;
            };

            const AZ::Vector3 c0 = cornerWorld(p0 + off, start);
            const AZ::Vector3 c1 = cornerWorld(p0 - off, start);
            const AZ::Vector3 c2 = cornerWorld(p1 - off, end);
            const AZ::Vector3 c3 = cornerWorld(p1 + off, end);

            outTriangles.push_back(DebugVertex{ c0, startColor });
            outTriangles.push_back(DebugVertex{ c1, startColor });
            outTriangles.push_back(DebugVertex{ c2, endColor });
            outTriangles.push_back(DebugVertex{ c0, startColor });
            outTriangles.push_back(DebugVertex{ c2, endColor });
            outTriangles.push_back(DebugVertex{ c3, endColor });
        }
    }

    AZ::Color GenericDebugDisplay::CurrentColor() const
    {
        // Route B: the self-drawn gizmo views (GizmoViews.cpp) already emit final themed colours, so
        // this display is a dumb pass-through - no palette remap (that would repaint e.g. Unreal's
        // blue Z axis and EditorHelpers icons).
        return m_color;
    }

    void GenericDebugDisplay::EmitLine(const AZ::Vector3& worldStart, const AZ::Vector3& worldEnd, const AZ::Color& color)
    {
        EmitLine(worldStart, worldEnd, color, color);
    }

    void GenericDebugDisplay::EmitLine(
        const AZ::Vector3& worldStart, const AZ::Vector3& worldEnd, const AZ::Color& startColor, const AZ::Color& endColor)
    {
        auto& batch = m_depthTest ? m_depthLines : m_overlayLines;
        batch.push_back(DebugLineSegment{ worldStart, worldEnd, startColor, endColor, m_lineWidth });
    }

    void GenericDebugDisplay::EmitTriangle(
        const AZ::Vector3& w0, const AZ::Vector3& w1, const AZ::Vector3& w2, const AZ::Color& color)
    {
        auto& batch = m_depthTest ? m_depthTriangles : m_overlayTriangles;
        batch.push_back(DebugVertex{ w0, color });
        batch.push_back(DebugVertex{ w1, color });
        batch.push_back(DebugVertex{ w2, color });
    }

    void GenericDebugDisplay::SetColor(const AZ::Color& color)
    {
        m_color = color;
    }

    void GenericDebugDisplay::SetAlpha(float a)
    {
        m_color.SetA(a);
    }

    void GenericDebugDisplay::SetLineWidth(float width)
    {
        m_lineWidth = width;
    }

    float GenericDebugDisplay::GetLineWidth()
    {
        return m_lineWidth;
    }

    void GenericDebugDisplay::DepthTestOn()
    {
        m_depthTest = true;
    }

    void GenericDebugDisplay::DepthTestOff()
    {
        m_depthTest = false;
    }

    void GenericDebugDisplay::PushMatrix(const AZ::Transform& tm)
    {
        m_transformStack.push_back(CurrentTransform() * AZ::Matrix3x4::CreateFromTransform(tm));
    }

    void GenericDebugDisplay::PopMatrix()
    {
        if (m_transformStack.size() > 1)
        {
            m_transformStack.pop_back();
        }
        else
        {
            AZ_Error("GenericDebugDisplay", false, "PopMatrix called with an empty transform stack.");
        }
    }

    void GenericDebugDisplay::PushPremultipliedMatrix(const AZ::Matrix3x4& matrix)
    {
        m_transformStack.push_back(matrix);
    }

    AZ::Matrix3x4 GenericDebugDisplay::PopPremultipliedMatrix()
    {
        if (m_transformStack.size() > 1)
        {
            const AZ::Matrix3x4 top = CurrentTransform();
            m_transformStack.pop_back();
            return top;
        }
        AZ_Error("GenericDebugDisplay", false, "PopPremultipliedMatrix called with an empty transform stack.");
        return AZ::Matrix3x4::CreateIdentity();
    }

    void GenericDebugDisplay::DrawLine(const AZ::Vector3& p1, const AZ::Vector3& p2)
    {
        EmitLine(ToWorld(p1), ToWorld(p2), CurrentColor());
    }

    void GenericDebugDisplay::DrawLine(
        const AZ::Vector3& p1, const AZ::Vector3& p2, const AZ::Vector4& col1, const AZ::Vector4& col2)
    {
        EmitLine(
            ToWorld(p1), ToWorld(p2), AZ::Color::CreateFromVector3AndFloat(col1.GetAsVector3(), col1.GetW()),
            AZ::Color::CreateFromVector3AndFloat(col2.GetAsVector3(), col2.GetW()));
    }

    void GenericDebugDisplay::DrawLines(const AZStd::vector<AZ::Vector3>& lines, const AZ::Color& color)
    {
        const size_t count = lines.size() & ~size_t(1);
        for (size_t i = 0; i < count; i += 2)
        {
            EmitLine(ToWorld(lines[i]), ToWorld(lines[i + 1]), color);
        }
    }

    void GenericDebugDisplay::DrawPolyLine(AZStd::span<const AZ::Vector3> points, bool cycled)
    {
        const size_t numPoints = points.size();
        if (numPoints < 2)
        {
            return;
        }
        for (size_t i = 0; i < numPoints - 1; ++i)
        {
            EmitLine(ToWorld(points[i]), ToWorld(points[i + 1]), CurrentColor());
        }
        if (cycled)
        {
            EmitLine(ToWorld(points[numPoints - 1]), ToWorld(points[0]), CurrentColor());
        }
    }

    void GenericDebugDisplay::DrawQuad(
        const AZ::Vector3& p1, const AZ::Vector3& p2, const AZ::Vector3& p3, const AZ::Vector3& p4)
    {
        const AZ::Vector3 w1 = ToWorld(p1);
        const AZ::Vector3 w2 = ToWorld(p2);
        const AZ::Vector3 w3 = ToWorld(p3);
        const AZ::Vector3 w4 = ToWorld(p4);
        const AZ::Color color = CurrentColor();
        EmitTriangle(w1, w2, w3, color);
        EmitTriangle(w3, w4, w1, color);
    }

    void GenericDebugDisplay::DrawTri(const AZ::Vector3& p1, const AZ::Vector3& p2, const AZ::Vector3& p3)
    {
        EmitTriangle(ToWorld(p1), ToWorld(p2), ToWorld(p3), CurrentColor());
    }

    void GenericDebugDisplay::DrawTriangles(const AZStd::vector<AZ::Vector3>& vertices, const AZ::Color& color)
    {
        const size_t count = vertices.size() - (vertices.size() % 3);
        for (size_t i = 0; i < count; i += 3)
        {
            EmitTriangle(ToWorld(vertices[i]), ToWorld(vertices[i + 1]), ToWorld(vertices[i + 2]), color);
        }
    }

    void GenericDebugDisplay::DrawTrianglesIndexed(
        const AZStd::vector<AZ::Vector3>& vertices, const AZStd::vector<AZ::u32>& indices, const AZ::Color& color)
    {
        const size_t count = indices.size() - (indices.size() % 3);
        for (size_t i = 0; i < count; i += 3)
        {
            if (indices[i] < vertices.size() && indices[i + 1] < vertices.size() && indices[i + 2] < vertices.size())
            {
                EmitTriangle(
                    ToWorld(vertices[indices[i]]), ToWorld(vertices[indices[i + 1]]), ToWorld(vertices[indices[i + 2]]), color);
            }
        }
    }

    void GenericDebugDisplay::DrawWireBox(const AZ::Vector3& min, const AZ::Vector3& max)
    {
        const AZ::Vector3 corners[8] = {
            AZ::Vector3(min.GetX(), min.GetY(), min.GetZ()), AZ::Vector3(max.GetX(), min.GetY(), min.GetZ()),
            AZ::Vector3(max.GetX(), max.GetY(), min.GetZ()), AZ::Vector3(min.GetX(), max.GetY(), min.GetZ()),
            AZ::Vector3(min.GetX(), min.GetY(), max.GetZ()), AZ::Vector3(max.GetX(), min.GetY(), max.GetZ()),
            AZ::Vector3(max.GetX(), max.GetY(), max.GetZ()), AZ::Vector3(min.GetX(), max.GetY(), max.GetZ()),
        };
        constexpr int edges[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 },
                                       { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        const AZ::Color color = CurrentColor();
        for (const auto& e : edges)
        {
            EmitLine(ToWorld(corners[e[0]]), ToWorld(corners[e[1]]), color);
        }
    }

    void GenericDebugDisplay::DrawSolidBox(const AZ::Vector3& min, const AZ::Vector3& max)
    {
        const AZ::Vector3 c[8] = {
            AZ::Vector3(min.GetX(), min.GetY(), min.GetZ()), AZ::Vector3(max.GetX(), min.GetY(), min.GetZ()),
            AZ::Vector3(max.GetX(), max.GetY(), min.GetZ()), AZ::Vector3(min.GetX(), max.GetY(), min.GetZ()),
            AZ::Vector3(min.GetX(), min.GetY(), max.GetZ()), AZ::Vector3(max.GetX(), min.GetY(), max.GetZ()),
            AZ::Vector3(max.GetX(), max.GetY(), max.GetZ()), AZ::Vector3(min.GetX(), max.GetY(), max.GetZ()),
        };
        constexpr int faces[6][4] = { { 0, 1, 2, 3 }, { 4, 7, 6, 5 }, { 0, 4, 5, 1 },
                                      { 1, 5, 6, 2 }, { 2, 6, 7, 3 }, { 3, 7, 4, 0 } };
        const AZ::Color color = CurrentColor();
        for (const auto& f : faces)
        {
            const AZ::Vector3 w0 = ToWorld(c[f[0]]);
            const AZ::Vector3 w1 = ToWorld(c[f[1]]);
            const AZ::Vector3 w2 = ToWorld(c[f[2]]);
            const AZ::Vector3 w3 = ToWorld(c[f[3]]);
            EmitTriangle(w0, w1, w2, color);
            EmitTriangle(w2, w3, w0, color);
        }
    }

    void GenericDebugDisplay::DrawSolidOBB(
        const AZ::Vector3& center, const AZ::Vector3& axisX, const AZ::Vector3& axisY, const AZ::Vector3& axisZ,
        const AZ::Vector3& halfExtents)
    {
        const AZ::Vector3 ex = axisX * halfExtents.GetX();
        const AZ::Vector3 ey = axisY * halfExtents.GetY();
        const AZ::Vector3 ez = axisZ * halfExtents.GetZ();
        AZ::Vector3 c[8];
        for (int i = 0; i < 8; ++i)
        {
            const float sx = (i & 1) ? 1.0f : -1.0f;
            const float sy = (i & 2) ? 1.0f : -1.0f;
            const float sz = (i & 4) ? 1.0f : -1.0f;
            c[i] = ToWorld(center + ex * sx + ey * sy + ez * sz);
        }
        constexpr int faces[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 },
                                      { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
        const AZ::Color color = CurrentColor();
        for (const auto& f : faces)
        {
            EmitTriangle(c[f[0]], c[f[1]], c[f[2]], color);
            EmitTriangle(c[f[2]], c[f[3]], c[f[0]], color);
        }
    }

    void GenericDebugDisplay::DrawPoint(const AZ::Vector3& p, int nSize)
    {
        // Represent a point as a small camera-agnostic cross so it survives line-only backends.
        const float halfSize = 0.5f * AZStd::max(1, nSize) * 0.01f;
        const AZ::Vector3 w = ToWorld(p);
        const AZ::Color color = CurrentColor();
        EmitLine(w - AZ::Vector3::CreateAxisX(halfSize), w + AZ::Vector3::CreateAxisX(halfSize), color);
        EmitLine(w - AZ::Vector3::CreateAxisY(halfSize), w + AZ::Vector3::CreateAxisY(halfSize), color);
        EmitLine(w - AZ::Vector3::CreateAxisZ(halfSize), w + AZ::Vector3::CreateAxisZ(halfSize), color);
    }

    void GenericDebugDisplay::BasisFromAxis(const AZ::Vector3& axis, AZ::Vector3& outU, AZ::Vector3& outV)
    {
        const AZ::Vector3 n = axis.GetNormalizedSafe();
        // Duplicated from O3DE's GetBasisVectors so curve orientation matches the original editor.
        if (n.GetZ() < AZ::Constants::FloatEpsilon - 1.0f)
        {
            outU = AZ::Vector3(0.0f, -1.0f, 0.0f);
            outV = AZ::Vector3(-1.0f, 0.0f, 0.0f);
            return;
        }
        const float a = 1.0f / (1.0f + n.GetZ());
        const float b = -n.GetX() * n.GetY() * a;
        outU = AZ::Vector3(1.0f - n.GetX() * n.GetX() * a, b, -n.GetX());
        outV = AZ::Vector3(b, 1.0f - n.GetY() * n.GetY() * a, -n.GetY());
    }

    void GenericDebugDisplay::BuildRing(
        const AZ::Vector3& worldCenter, const AZ::Vector3& worldAxis, float radius, int segments,
        AZStd::vector<AZ::Vector3>& outRing) const
    {
        AZ::Vector3 u, v;
        BasisFromAxis(worldAxis, u, v);
        outRing.clear();
        outRing.reserve(segments);
        const float step = AZ::Constants::TwoPi / static_cast<float>(segments);
        for (int i = 0; i < segments; ++i)
        {
            const float angle = step * static_cast<float>(i);
            outRing.push_back(worldCenter + (u * cosf(angle) + v * sinf(angle)) * radius);
        }
    }

    void GenericDebugDisplay::DrawCircle(const AZ::Vector3& pos, float radius, int nUnchangedAxis)
    {
        const int axis = AZ::GetClamp(nUnchangedAxis, 0, 2);
        const AZ::Vector3 worldAxis = ToWorldVector(AxisVector(axis));
        AZStd::vector<AZ::Vector3> ring;
        BuildRing(ToWorld(pos), worldAxis, radius, m_style.m_circleSegments, ring);
        const AZ::Color color = CurrentColor();
        const int count = static_cast<int>(ring.size());
        for (int i = 0; i < count; ++i)
        {
            EmitLine(ring[i], ring[(i + 1) % count], color);
        }
    }

    void GenericDebugDisplay::DrawHalfDottedCircle(
        const AZ::Vector3& pos, float radius, const AZ::Vector3& viewPos, int nUnchangedAxis)
    {
        const int axis = AZ::GetClamp(nUnchangedAxis, 0, 2);
        const AZ::Vector3 worldCenter = ToWorld(pos);
        const AZ::Vector3 worldView = ToWorld(viewPos);
        const AZ::Vector3 worldDir = worldView - worldCenter;
        const AZ::Vector3 worldAxis = ToWorldVector(AxisVector(axis));
        AZStd::vector<AZ::Vector3> ring;
        BuildRing(worldCenter, worldAxis, radius, m_style.m_circleSegments, ring);

        const AZ::Color color = CurrentColor();
        const int count = static_cast<int>(ring.size());
        for (int i = 0; i < count; ++i)
        {
            const AZ::Vector3& a = ring[i];
            const AZ::Vector3& b = ring[(i + 1) % count];
            // Draw front-facing segments solid, back-facing dotted (skip every other) - Blender look.
            const bool facing = (a - worldCenter).Dot(worldDir) > 0.0f;
            if (facing || (i % 2 == 0))
            {
                EmitLine(a, b, color);
            }
        }
    }

    void GenericDebugDisplay::DrawArc(
        const AZ::Vector3& pos, float radius, float startAngleDegrees, float sweepAngleDegrees, float angularStepDegrees,
        int referenceAxis)
    {
        const int axis = AZ::GetClamp(referenceAxis, 0, 2);
        const int axis1 = (axis + 1) % 3;
        const int axis2 = (axis + 2) % 3;
        const float step = AZ::GetMax(DegToRad(AZ::GetAbs(angularStepDegrees)), DegToRad(1.0f));
        const float start = DegToRad(startAngleDegrees);
        const float end = start + DegToRad(sweepAngleDegrees);

        const AZ::Color color = CurrentColor();
        AZ::Vector3 sinCos = AZ::Vector3::CreateZero();
        auto pointAt = [&](float angle)
        {
            sinCos.SetElement(axis1, sinf(angle));
            sinCos.SetElement(axis2, cosf(angle));
            return ToWorld(pos + sinCos * radius);
        };
        AZ::Vector3 prev = pointAt(start);
        for (float angle = start + step; angle < end; angle += step)
        {
            const AZ::Vector3 cur = pointAt(angle);
            EmitLine(prev, cur, color);
            prev = cur;
        }
        EmitLine(prev, pointAt(end), color);
    }

    void GenericDebugDisplay::DrawWireCone(const AZ::Vector3& pos, const AZ::Vector3& dir, float radius, float height)
    {
        const AZ::Vector3 worldBase = ToWorld(pos);
        const AZ::Vector3 worldAxis = ToWorldVector(dir).GetNormalizedSafe();
        const AZ::Vector3 apex = worldBase + worldAxis * height;
        AZStd::vector<AZ::Vector3> ring;
        BuildRing(worldBase, worldAxis, radius, m_style.m_capSegments, ring);

        const AZ::Color color = CurrentColor();
        const int count = static_cast<int>(ring.size());
        for (int i = 0; i < count; ++i)
        {
            EmitLine(ring[i], ring[(i + 1) % count], color);
            EmitLine(ring[i], apex, color);
        }
    }

    void GenericDebugDisplay::DrawSolidCone(
        const AZ::Vector3& pos, const AZ::Vector3& dir, float radius, float height, [[maybe_unused]] bool drawShaded)
    {
        DrawSolidConeWithSegments(pos, dir, radius, height, m_style.m_capSegments);
    }

    void GenericDebugDisplay::DrawSolidConeWithSegments(
        const AZ::Vector3& pos, const AZ::Vector3& dir, float radius, float height, int segments)
    {
        const AZ::Vector3 worldBase = ToWorld(pos);
        const AZ::Vector3 worldAxis = ToWorldVector(dir).GetNormalizedSafe();
        const AZ::Vector3 apex = worldBase + worldAxis * height;
        AZStd::vector<AZ::Vector3> ring;
        BuildRing(worldBase, worldAxis, radius, AZStd::max(3, segments), ring);

        const AZ::Color color = CurrentColor();
        const int count = static_cast<int>(ring.size());
        for (int i = 0; i < count; ++i)
        {
            const AZ::Vector3& a = ring[i];
            const AZ::Vector3& b = ring[(i + 1) % count];
            EmitTriangle(a, b, apex, color);     // side
            EmitTriangle(b, a, worldBase, color); // base cap
        }
    }

    void GenericDebugDisplay::DrawWireCylinder(
        const AZ::Vector3& center, const AZ::Vector3& axis, float radius, float height)
    {
        const AZ::Vector3 worldAxis = ToWorldVector(axis).GetNormalizedSafe();
        const AZ::Vector3 worldCenter = ToWorld(center);
        const AZ::Vector3 half = worldAxis * (0.5f * height);
        AZStd::vector<AZ::Vector3> top, bottom;
        BuildRing(worldCenter + half, worldAxis, radius, m_style.m_capSegments, top);
        BuildRing(worldCenter - half, worldAxis, radius, m_style.m_capSegments, bottom);

        const AZ::Color color = CurrentColor();
        const int count = static_cast<int>(top.size());
        for (int i = 0; i < count; ++i)
        {
            EmitLine(top[i], top[(i + 1) % count], color);
            EmitLine(bottom[i], bottom[(i + 1) % count], color);
            EmitLine(bottom[i], top[i], color);
        }
    }

    void GenericDebugDisplay::DrawSolidCylinder(
        const AZ::Vector3& center, const AZ::Vector3& axis, float radius, float height, [[maybe_unused]] bool drawShaded)
    {
        const AZ::Vector3 worldAxis = ToWorldVector(axis).GetNormalizedSafe();
        const AZ::Vector3 worldCenter = ToWorld(center);
        const AZ::Vector3 half = worldAxis * (0.5f * height);
        const AZ::Vector3 topCenter = worldCenter + half;
        const AZ::Vector3 bottomCenter = worldCenter - half;
        AZStd::vector<AZ::Vector3> top, bottom;
        BuildRing(topCenter, worldAxis, radius, m_style.m_capSegments, top);
        BuildRing(bottomCenter, worldAxis, radius, m_style.m_capSegments, bottom);

        const AZ::Color color = CurrentColor();
        const int count = static_cast<int>(top.size());
        for (int i = 0; i < count; ++i)
        {
            const int j = (i + 1) % count;
            EmitTriangle(bottom[i], bottom[j], top[j], color); // side
            EmitTriangle(top[j], top[i], bottom[i], color);
            EmitTriangle(topCenter, top[i], top[j], color);       // top cap
            EmitTriangle(bottomCenter, bottom[j], bottom[i], color); // bottom cap
        }
    }

    void GenericDebugDisplay::DrawWireSphere(const AZ::Vector3& pos, float radius)
    {
        const AZ::Vector3 worldCenter = ToWorld(pos);
        const AZ::Color color = CurrentColor();
        // Three great circles - the classic, cheap wire-sphere used by editors.
        for (int axis = 0; axis < 3; ++axis)
        {
            AZStd::vector<AZ::Vector3> ring;
            BuildRing(worldCenter, ToWorldVector(AxisVector(axis)), radius, m_style.m_circleSegments, ring);
            const int count = static_cast<int>(ring.size());
            for (int i = 0; i < count; ++i)
            {
                EmitLine(ring[i], ring[(i + 1) % count], color);
            }
        }
    }

    void GenericDebugDisplay::DrawBall(const AZ::Vector3& pos, float radius, [[maybe_unused]] bool drawShaded)
    {
        const AZ::Vector3 worldCenter = ToWorld(pos);
        const AZ::Color color = CurrentColor();
        const int rings = AZ::GetMax(4, m_style.m_capSegments / 2);
        const int sectors = m_style.m_capSegments;

        auto vertexAt = [&](int ring, int sector)
        {
            const float phi = AZ::Constants::Pi * static_cast<float>(ring) / static_cast<float>(rings);
            const float theta = AZ::Constants::TwoPi * static_cast<float>(sector) / static_cast<float>(sectors);
            const AZ::Vector3 unit(sinf(phi) * cosf(theta), sinf(phi) * sinf(theta), cosf(phi));
            return worldCenter + unit * radius;
        };

        for (int ring = 0; ring < rings; ++ring)
        {
            for (int sector = 0; sector < sectors; ++sector)
            {
                const AZ::Vector3 a = vertexAt(ring, sector);
                const AZ::Vector3 b = vertexAt(ring + 1, sector);
                const AZ::Vector3 c = vertexAt(ring, (sector + 1) % sectors);
                const AZ::Vector3 d = vertexAt(ring + 1, (sector + 1) % sectors);
                EmitTriangle(a, b, c, color);
                EmitTriangle(c, b, d, color);
            }
        }
    }

    void GenericDebugDisplay::DrawWireDisk(const AZ::Vector3& pos, const AZ::Vector3& dir, float radius)
    {
        const AZ::Vector3 worldCenter = ToWorld(pos);
        const AZ::Vector3 worldAxis = ToWorldVector(dir);
        AZStd::vector<AZ::Vector3> ring;
        BuildRing(worldCenter, worldAxis, radius, m_style.m_circleSegments, ring);
        const AZ::Color color = CurrentColor();
        const int count = static_cast<int>(ring.size());
        for (int i = 0; i < count; ++i)
        {
            EmitLine(ring[i], ring[(i + 1) % count], color);
        }
        // Short normal stub, matching O3DE's DrawWireDisk cue.
        EmitLine(worldCenter, worldCenter + worldAxis.GetNormalizedSafe() * (radius * 0.2f), color);
    }

    void GenericDebugDisplay::DrawDisk(
        const AZ::Vector3& pos, const AZ::Vector3& dir, float radius, [[maybe_unused]] bool drawShaded)
    {
        const AZ::Vector3 worldCenter = ToWorld(pos);
        AZStd::vector<AZ::Vector3> ring;
        BuildRing(worldCenter, ToWorldVector(dir), radius, m_style.m_capSegments, ring);
        const AZ::Color color = CurrentColor();
        const int count = static_cast<int>(ring.size());
        for (int i = 0; i < count; ++i)
        {
            EmitTriangle(worldCenter, ring[i], ring[(i + 1) % count], color);
        }
    }

    void GenericDebugDisplay::DrawArrow(const AZ::Vector3& src, const AZ::Vector3& trg, float headScale, bool twoSided)
    {
        const AZ::Vector3 worldSrc = ToWorld(src);
        const AZ::Vector3 worldTrg = ToWorld(trg);
        const AZ::Color color = CurrentColor();
        EmitLine(worldSrc, worldTrg, color);

        const AZ::Vector3 dir = (worldTrg - worldSrc);
        const float length = dir.GetLength();
        if (length < AZ::Constants::FloatEpsilon)
        {
            return;
        }
        const AZ::Vector3 unit = dir / length;
        AZ::Vector3 u, v;
        BasisFromAxis(unit, u, v);
        const float headLength = 0.2f * length * headScale;
        const float headRadius = 0.1f * length * headScale;

        auto drawHead = [&](const AZ::Vector3& tip, const AZ::Vector3& axis)
        {
            const AZ::Vector3 base = tip - axis * headLength;
            EmitLine(tip, base + u * headRadius, color);
            EmitLine(tip, base - u * headRadius, color);
            EmitLine(tip, base + v * headRadius, color);
            EmitLine(tip, base - v * headRadius, color);
        };
        drawHead(worldTrg, unit);
        if (twoSided)
        {
            drawHead(worldSrc, -unit);
        }
    }

    void GenericDebugDisplay::DrawTextLabel(
        const AZ::Vector3& pos, float size, const char* text, bool bCenter, int srcOffsetX, int srcOffsetY)
    {
        if (text == nullptr || text[0] == '\0')
        {
            return;
        }
        AZ::Vector3 world = ToWorld(pos);
        // Pixel nudges are applied in screen space by the painter; fold them into the label so the
        // Qt overlay stays a dumb consumer.
        m_textLabels.push_back(DebugTextLabel{ world, CurrentColor(), size, bCenter, text, m_nextLabelBackgroundBlock });
        m_nextLabelBackgroundBlock = false;
        (void)srcOffsetX;
        (void)srcOffsetY;
    }
} // namespace CrossEngineEditor
