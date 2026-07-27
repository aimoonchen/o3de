/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/GizmoViews.h>

#include <AzToolsFramework/Picking/ContextBoundAPI.h>
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>

#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Transform.h>

#include <cmath>

namespace CrossEngineEditor
{
    using AzToolsFramework::ManipulatorManagerId;
    using AzToolsFramework::ManipulatorManagerState;
    using AzToolsFramework::ManipulatorId;
    using AzToolsFramework::ManipulatorState;
    namespace Picking = AzToolsFramework::Picking;
    namespace VI = AzToolsFramework::ViewportInteraction;

    namespace
    {
        AZ::Color AxisColor(const GizmoTheme& theme, GizmoAxis axis)
        {
            return GizmoAxisColor(theme, axis);
        }
    } // namespace

    AZ::Color GizmoAxisColor(const GizmoTheme& theme, GizmoAxis axis)
    {
        switch (axis)
        {
        case GizmoAxis::X:
            return theme.m_axisXColor;
        case GizmoAxis::Y:
            return theme.m_axisYColor;
        case GizmoAxis::Z:
        default:
            return theme.m_axisZColor;
        }
    }

    AZ::Vector3 GizmoAxisToVector(GizmoAxis axis)
    {
        switch (axis)
        {
        case GizmoAxis::X:
            return AZ::Vector3::CreateAxisX();
        case GizmoAxis::Y:
            return AZ::Vector3::CreateAxisY();
        case GizmoAxis::Z:
        default:
            return AZ::Vector3::CreateAxisZ();
        }
    }

    AZ::Vector3 ThemedGizmoView::AxisVector() const
    {
        return GizmoAxisToVector(m_axis);
    }

    AZ::Color ThemedGizmoView::ResolvedColor(bool mouseOver) const
    {
        if (mouseOver)
        {
            AZ::Color c = m_theme.m_highlightColor;
            c.SetA(m_theme.m_highlightAlpha);
            return c;
        }
        AZ::Color c = AxisColor(m_theme, m_axis);
        c.SetA(m_theme.m_normalAlpha);
        return c;
    }

    void ArrowGizmoView::Draw(
        const ManipulatorManagerId managerId,
        const ManipulatorManagerState& /*managerState*/,
        const ManipulatorId manipulatorId,
        const ManipulatorState& manipulatorState,
        AzFramework::DebugDisplayRequests& debugDisplay,
        const AzFramework::CameraState& cameraState,
        const VI::MouseInteraction& /*mouseInteraction*/)
    {
        const AZ::Vector3 worldPivot = manipulatorState.m_worldFromLocal.TransformPoint(manipulatorState.m_localPosition);
        const float scale = ManipulatorViewScaleMultiplier(worldPivot, cameraState);

        const AZ::Vector3 axis = AxisVector();
        const float lineLen = m_theme.m_arrowAxisLength * scale;
        const float coneLen = m_theme.m_arrowConeLength * scale;
        const float coneRadius = m_theme.m_arrowConeRadius * scale;
        const AZ::Color color = ResolvedColor(manipulatorState.m_mouseOver);

        const AZ::Vector3 lineStart = manipulatorState.m_localPosition;
        const AZ::Vector3 lineEnd = lineStart + axis * lineLen;
        const AZ::Vector3 coneBase = lineEnd + axis * (m_theme.m_arrowConeGap * scale);

        debugDisplay.PushMatrix(manipulatorState.m_worldFromLocal);
        debugDisplay.SetColor(color.GetAsVector4());
        debugDisplay.SetLineWidth(m_theme.m_axisLineWidth);
        debugDisplay.DrawLine(lineStart, lineEnd);
        debugDisplay.DrawSolidCone(coneBase, axis, coneRadius, coneLen, false);
        debugDisplay.PopMatrix();

        // Picking bound: line segment along the axis (width == cone radius for easy grabbing).
        Picking::BoundShapeLineSegment lineBound;
        lineBound.m_start = manipulatorState.TransformPoint(lineStart);
        lineBound.m_end = manipulatorState.TransformPoint(coneBase + axis * coneLen);
        lineBound.m_width = coneRadius;
        RefreshBoundInternal(managerId, manipulatorId, lineBound);
    }

    void BoxGizmoView::Draw(
        const ManipulatorManagerId managerId,
        const ManipulatorManagerState& /*managerState*/,
        const ManipulatorId manipulatorId,
        const ManipulatorState& manipulatorState,
        AzFramework::DebugDisplayRequests& debugDisplay,
        const AzFramework::CameraState& cameraState,
        const VI::MouseInteraction& /*mouseInteraction*/)
    {
        const AZ::Vector3 worldPivot = manipulatorState.m_worldFromLocal.TransformPoint(manipulatorState.m_localPosition);
        const float scale = ManipulatorViewScaleMultiplier(worldPivot, cameraState);

        const AZ::Vector3 axis = AxisVector();
        const float lineLen = m_theme.m_scaleAxisLength * scale;
        const float halfExtent = m_theme.m_scaleBoxHalfExtent * scale;
        const AZ::Color color = ResolvedColor(manipulatorState.m_mouseOver);

        const AZ::Vector3 lineStart = manipulatorState.m_localPosition;
        const AZ::Vector3 lineEnd = lineStart + axis * lineLen;
        const AZ::Vector3 boxCenter = lineEnd + axis * (m_theme.m_scaleBoxGap * scale + halfExtent);
        const AZ::Vector3 halfExtents(halfExtent, halfExtent, halfExtent);

        debugDisplay.PushMatrix(manipulatorState.m_worldFromLocal);
        debugDisplay.SetColor(color.GetAsVector4());
        debugDisplay.SetLineWidth(m_theme.m_axisLineWidth);
        debugDisplay.DrawLine(lineStart, lineEnd);
        debugDisplay.DrawSolidBox(boxCenter - halfExtents, boxCenter + halfExtents);
        debugDisplay.PopMatrix();

        Picking::BoundShapeBox boxBound;
        boxBound.m_center = manipulatorState.TransformPoint(boxCenter);
        boxBound.m_orientation = AZ::Quaternion::CreateIdentity();
        boxBound.m_halfExtents = halfExtents * 1.5f; // modest grab margin, avoids inter-axis overlap.
        RefreshBoundInternal(managerId, manipulatorId, boxBound);
    }

    PlaneGizmoView::PlaneGizmoView(const GizmoTheme& theme, GizmoAxis axis1, GizmoAxis axis2)
        : ManipulatorView(/*screenSizeFixed=*/true)
        , m_theme(theme)
        , m_axis1(axis1)
        , m_axis2(axis2)
    {
    }

    void PlaneGizmoView::Draw(
        const ManipulatorManagerId managerId,
        const ManipulatorManagerState& /*managerState*/,
        const ManipulatorId manipulatorId,
        const ManipulatorState& manipulatorState,
        AzFramework::DebugDisplayRequests& debugDisplay,
        const AzFramework::CameraState& cameraState,
        const VI::MouseInteraction& /*mouseInteraction*/)
    {
        const AZ::Vector3 worldPivot = manipulatorState.m_worldFromLocal.TransformPoint(manipulatorState.m_localPosition);
        const float scale = ManipulatorViewScaleMultiplier(worldPivot, cameraState);

        const AZ::Vector3 a1 = GizmoAxisToVector(m_axis1);
        const AZ::Vector3 a2 = GizmoAxisToVector(m_axis2);
        const float offset = m_theme.m_planeOffset * scale;
        const float size = m_theme.m_planeSize * scale;

        const bool mouseOver = manipulatorState.m_mouseOver;

        debugDisplay.PushMatrix(manipulatorState.m_worldFromLocal);
        debugDisplay.SetLineWidth(m_theme.m_axisLineWidth);

        if (m_theme.m_planeGeometry == PlaneHandleGeometry::UnrealCornerBracket)
        {
            // Unreal DrawDualAxis: an L-shaped corner bracket at (offset, offset). Two arms, each
            // tinted by its own axis (or the highlight colour when hovered).
            const AZ::Vector3 corner = manipulatorState.m_localPosition + (a1 + a2) * offset;
            AZ::Color c1 = mouseOver ? m_theme.m_highlightColor : AxisColor(m_theme, m_axis1);
            AZ::Color c2 = mouseOver ? m_theme.m_highlightColor : AxisColor(m_theme, m_axis2);
            c1.SetA(mouseOver ? m_theme.m_highlightAlpha : m_theme.m_normalAlpha);
            c2.SetA(mouseOver ? m_theme.m_highlightAlpha : m_theme.m_normalAlpha);

            debugDisplay.SetColor(c1.GetAsVector4());
            debugDisplay.DrawLine(corner, corner - a1 * size);
            debugDisplay.SetColor(c2.GetAsVector4());
            debugDisplay.DrawLine(corner, corner - a2 * size);

            debugDisplay.PopMatrix();

            // Picking bound: quad spanning the bracket's inner square.
            const AZ::Vector3 inner = corner - (a1 + a2) * size;
            Picking::BoundShapeQuad quadBound;
            quadBound.m_corner1 = manipulatorState.TransformPoint(corner);
            quadBound.m_corner2 = manipulatorState.TransformPoint(corner - a1 * size);
            quadBound.m_corner3 = manipulatorState.TransformPoint(inner);
            quadBound.m_corner4 = manipulatorState.TransformPoint(corner - a2 * size);
            RefreshBoundInternal(managerId, manipulatorId, quadBound);
            return;
        }

        // Blender diamond: centred on the axis line end at (offset, offset), spanning +/-size along
        // the two in-plane axes (verts {s,s},{-s,s},{-s,-s},{s,-s} in that plane). Filled + outline.
        const AZ::Vector3 centre = manipulatorState.m_localPosition + (a1 + a2) * offset;
        const AZ::Vector3 d0 = centre + (a1 + a2) * size;
        const AZ::Vector3 d1 = centre + (-a1 + a2) * size;
        const AZ::Vector3 d2 = centre + (-a1 - a2) * size;
        const AZ::Vector3 d3 = centre + (a1 - a2) * size;

        AZ::Color fill = AxisColor(m_theme, m_axis1);
        AZ::Color outline = mouseOver ? m_theme.m_highlightColor : fill;
        outline.SetA(mouseOver ? m_theme.m_highlightAlpha : m_theme.m_normalAlpha);
        // Blender color_inner = {color, color[3] * 0.5} where color[3] is the resting handle alpha.
        fill.SetA(m_theme.m_normalAlpha * m_theme.m_planeFillAlpha);

        debugDisplay.SetColor(fill.GetAsVector4());
        debugDisplay.DrawQuad(d0, d1, d2, d3);
        debugDisplay.SetColor(outline.GetAsVector4());
        debugDisplay.DrawLine(d0, d1);
        debugDisplay.DrawLine(d1, d2);
        debugDisplay.DrawLine(d2, d3);
        debugDisplay.DrawLine(d3, d0);
        debugDisplay.PopMatrix();

        Picking::BoundShapeQuad quadBound;
        quadBound.m_corner1 = manipulatorState.TransformPoint(d0);
        quadBound.m_corner2 = manipulatorState.TransformPoint(d1);
        quadBound.m_corner3 = manipulatorState.TransformPoint(d2);
        quadBound.m_corner4 = manipulatorState.TransformPoint(d3);
        RefreshBoundInternal(managerId, manipulatorId, quadBound);
    }

    void DialGizmoView::Draw(
        const ManipulatorManagerId managerId,
        const ManipulatorManagerState& /*managerState*/,
        const ManipulatorId manipulatorId,
        const ManipulatorState& manipulatorState,
        AzFramework::DebugDisplayRequests& debugDisplay,
        const AzFramework::CameraState& cameraState,
        const VI::MouseInteraction& /*mouseInteraction*/)
    {
        const AZ::Vector3 worldPivot = manipulatorState.m_worldFromLocal.TransformPoint(manipulatorState.m_localPosition);
        const float scale = ManipulatorViewScaleMultiplier(worldPivot, cameraState);

        const AZ::Vector3 axis = AxisVector();
        const float radius = m_theme.m_ringRadius * scale;
        const AZ::Color color = ResolvedColor(manipulatorState.m_mouseOver);

        // Two axes spanning the plane perpendicular to the ring axis.
        const AZ::Vector3 seed =
            (fabsf(axis.GetX()) < 0.9f) ? AZ::Vector3::CreateAxisX() : AZ::Vector3::CreateAxisY();
        const AZ::Vector3 uN = axis.Cross(seed).GetNormalized();
        const AZ::Vector3 vN = axis.Cross(uN).GetNormalized();

        const int segments = AZStd::max(8, static_cast<int>(m_theme.m_ringSegments));
        const float twoPi = 6.2831853071795864769f;

        // Blender dial3d_gizmo.cc DIAL_CLIP: only the half of the ring facing the camera is drawn.
        // Blender clips against the view direction, not the vector to the camera position, so the
        // half-ring boundary stays stable at any distance and under orthographic projection. We
        // transform the camera view direction into the manipulator's local frame and keep segments
        // whose outward normal opposes the view direction (i.e. face the viewer).
        const AZ::Transform localFromWorld = manipulatorState.m_worldFromLocal.GetInverse();
        const AZ::Vector3 localViewDir = localFromWorld.TransformVector(cameraState.m_forward).GetNormalized();

        debugDisplay.PushMatrix(manipulatorState.m_worldFromLocal);
        debugDisplay.SetLineWidth(m_theme.m_ringLineWidth);

        auto ringPoint = [&](int i)
        {
            const float t = (static_cast<float>(i) / static_cast<float>(segments)) * twoPi;
            return manipulatorState.m_localPosition + (uN * cosf(t) + vN * sinf(t)) * radius;
        };
        auto facesCamera = [&](const AZ::Vector3& pt)
        {
            // The ring point's outward normal (from ring centre) faces the viewer when it opposes
            // the view direction, i.e. their dot product is negative.
            const AZ::Vector3 outward = (pt - manipulatorState.m_localPosition);
            return outward.Dot(localViewDir) < 0.0f;
        };

        if (m_theme.m_rotateGeometry == RotationRingGeometry::UnrealCameraFacingQuarter)
        {
            // Unreal (UnrealWidgetRender.cpp DrawRotationArc idle branch -> DrawPartialRotationArc ->
            // DrawThickArc): draw a THICK FILLED annulus band spanning the PI/2 quarter arc in the
            // camera-facing quadrant. The quadrant is chosen by mirroring each plane axis toward the
            // camera, exactly as UE picks RenderAxis0/RenderAxis1. The band is filled between an
            // inner and outer radius as a triangle strip, plus bright border ring lines at both
            // edges (DrawThickArc RingColor = opaque axis colour on SDPG_Foreground).
            const float su = (uN.Dot(localViewDir) <= 0.0f) ? 1.0f : -1.0f;
            const float sv = (vN.Dot(localViewDir) <= 0.0f) ? 1.0f : -1.0f;
            const AZ::Vector3 a0 = uN * su;
            const AZ::Vector3 a1 = vN * sv;
            const int quarterSegments = AZStd::max(6, segments / 4);
            const float halfPi = 1.5707963267948966f;

            const float rInner = m_theme.m_ringInnerRadius * scale;
            const float rOuter = m_theme.m_ringOuterRadius * scale;
            const AZ::Vector3 centre = manipulatorState.m_localPosition;

            // Filled band: quad strip between the inner and outer arcs at LargeOuterAlpha.
            AZ::Color fill = color;
            debugDisplay.SetColor(fill.GetAsVector4());
            AZ::Vector3 prevIn = centre + a0 * rInner;
            AZ::Vector3 prevOut = centre + a0 * rOuter;
            for (int i = 1; i <= quarterSegments; ++i)
            {
                const float t = (static_cast<float>(i) / static_cast<float>(quarterSegments)) * halfPi;
                const AZ::Vector3 dir = a0 * cosf(t) + a1 * sinf(t);
                const AZ::Vector3 curIn = centre + dir * rInner;
                const AZ::Vector3 curOut = centre + dir * rOuter;
                // Two triangles per band segment (quad strip).
                debugDisplay.DrawTri(prevIn, prevOut, curOut);
                debugDisplay.DrawTri(prevIn, curOut, curIn);
                prevIn = curIn;
                prevOut = curOut;
            }

            // Bright opaque border ring lines at the outer and inner edges (RingColor).
            AZ::Color ring = color;
            ring.SetA(1.0f);
            debugDisplay.SetColor(ring.GetAsVector4());
            debugDisplay.SetLineWidth(m_theme.m_ringLineWidth);
            AZ::Vector3 prevBorderOut = centre + a0 * rOuter;
            AZ::Vector3 prevBorderIn = centre + a0 * rInner;
            for (int i = 1; i <= quarterSegments; ++i)
            {
                const float t = (static_cast<float>(i) / static_cast<float>(quarterSegments)) * halfPi;
                const AZ::Vector3 dir = a0 * cosf(t) + a1 * sinf(t);
                const AZ::Vector3 curBorderOut = centre + dir * rOuter;
                const AZ::Vector3 curBorderIn = centre + dir * rInner;
                debugDisplay.DrawLine(prevBorderOut, curBorderOut);
                debugDisplay.DrawLine(prevBorderIn, curBorderIn);
                prevBorderOut = curBorderOut;
                prevBorderIn = curBorderIn;
            }
        }
        else
        {
            // Blender dial3d_gizmo.cc: the ring is drawn with a camera-facing clip plane
            // (ED_GIZMO_DIAL_DRAW_FLAG_CLIP), so the half of the ring behind the pivot relative
            // to the view direction is hard-clipped (not drawn at all), single solid colour.
            debugDisplay.SetColor(color.GetAsVector4());
            for (int i = 0; i < segments; ++i)
            {
                const AZ::Vector3 p0 = ringPoint(i);
                const AZ::Vector3 p1 = ringPoint(i + 1);
                if (facesCamera(p0) || facesCamera(p1))
                {
                    debugDisplay.DrawLine(p0, p1);
                }
            }
        }
        debugDisplay.PopMatrix();

        // Picking bound: torus around the ring axis in world space. For the Unreal filled band the
        // torus is centred on the band mid-radius with a minor radius covering the band half-width
        // so the whole annulus is grabbable; for Blender's thin ring it hugs the line.
        Picking::BoundShapeTorus torusBound;
        torusBound.m_center = manipulatorState.TransformPoint(manipulatorState.m_localPosition);
        torusBound.m_axis = manipulatorState.m_worldFromLocal.TransformVector(axis).GetNormalized();
        if (m_theme.m_rotateBand)
        {
            const float rInner = m_theme.m_ringInnerRadius * scale;
            const float rOuter = m_theme.m_ringOuterRadius * scale;
            torusBound.m_majorRadius = 0.5f * (rInner + rOuter);
            torusBound.m_minorRadius = 0.5f * (rOuter - rInner);
        }
        else
        {
            torusBound.m_majorRadius = radius;
            torusBound.m_minorRadius = m_theme.m_ringLineWidth * scale * 0.05f + radius * 0.05f;
        }
        RefreshBoundInternal(managerId, manipulatorId, torusBound);
    }

    void DrawTrackballViewRing(
        AzFramework::DebugDisplayRequests& debugDisplay,
        const AzFramework::CameraState& cameraState,
        const AZ::Vector3& worldPivot,
        const GizmoTheme& theme)
    {
        // Screen-aligned circle centred on the pivot (Blender's view ring), drawn once per frame.
        // Use the same screen-to-world multiplier the manipulator views use so the ring keeps a
        // constant on-screen size and matches the per-axis rings exactly.
        const float scale = AzToolsFramework::CalculateScreenToWorldMultiplier(worldPivot, cameraState);

        const float viewRadius = theme.m_viewRingRadius * scale;
        const AZ::Vector3 side = cameraState.m_side.GetNormalized();
        const AZ::Vector3 up = cameraState.m_up.GetNormalized();
        const int segments = AZStd::max(8, static_cast<int>(theme.m_ringSegments));
        const float twoPi = 6.2831853071795864769f;

        AZ::Color viewColor = theme.m_highlightColor;
        // Blender transform_gizmo_3d.cc MAN_AXIS_ROT_C: TH_GIZMO_VIEW_ALIGN (white) at the base
        // gizmo alpha 0.6 (gizmo_get_axis_color, alpha_fac == 1.0 for the screen-aligned ring).
        viewColor.SetA(0.6f);

        debugDisplay.SetColor(viewColor.GetAsVector4());
        debugDisplay.SetLineWidth(theme.m_viewRingLineWidth);
        AZ::Vector3 prev = worldPivot + side * viewRadius;
        for (int i = 1; i <= segments; ++i)
        {
            const float t = (static_cast<float>(i) / static_cast<float>(segments)) * twoPi;
            const AZ::Vector3 pt = worldPivot + (side * cosf(t) + up * sinf(t)) * viewRadius;
            debugDisplay.DrawLine(prev, pt);
            prev = pt;
        }
    }
} // namespace CrossEngineEditor
