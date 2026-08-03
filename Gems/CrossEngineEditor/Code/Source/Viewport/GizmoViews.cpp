/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/GizmoViews.h>
#include <Viewport/GenericDebugDisplay.h>

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

        //! The third axis not in {a, b} - i.e. the plane's normal axis. Blender colours a plane
        //! handle by its normal axis (transform_gizmo_3d.cc:361-389).
        GizmoAxis PlaneNormalAxis(GizmoAxis a, GizmoAxis b)
        {
            for (const GizmoAxis axis : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
            {
                if (axis != a && axis != b)
                {
                    return axis;
                }
            }
            return GizmoAxis::Z; // unreachable for distinct a,b.
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
            if (m_theme.m_highlightKeepsAxisHue)
            {
                // Blender: keep the axis hue, only raise alpha (gizmo_get_axis_color copies col->hi).
                AZ::Color c = AxisColor(m_theme, m_axis);
                c.SetA(m_theme.m_highlightAlpha);
                return c;
            }
            // Unreal: switch RGB to the highlight colour (yellow).
            AZ::Color c = m_theme.m_highlightColor;
            c.SetA(m_theme.m_highlightAlpha);
            return c;
        }
        AZ::Color c = AxisColor(m_theme, m_axis);
        c.SetA(m_theme.m_normalAlpha);
        return c;
    }

    float ThemedGizmoView::ViewFadeFactor(
        const AZ::Vector3& worldAxis, const AzFramework::CameraState& cameraState, bool isPlane) const
    {
        if (!m_theme.m_axisViewFade)
        {
            return 1.0f;
        }
        // Blender: idot = 1 - |dot(viewDir, axis)|; view direction is the camera forward.
        const AZ::Vector3 viewDir = cameraState.m_forward.GetNormalizedSafe();
        float idot = 1.0f - fabsf(viewDir.Dot(worldAxis.GetNormalizedSafe()));
        const float minV = isPlane ? m_theme.m_planeFadeMin : m_theme.m_axisFadeMin;
        const float maxV = isPlane ? m_theme.m_planeFadeMax : m_theme.m_axisFadeMax;
        if (isPlane)
        {
            idot = 1.0f - idot; // plane handles flip the dot product (g_tw_axis_range[1]).
        }
        if (idot > maxV)
        {
            return 1.0f;
        }
        if (idot < minV)
        {
            return 0.0f;
        }
        return (idot - minV) / (maxV - minV);
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
        const float lineStartDist = m_theme.m_axisLineStart * scale;
        const float lineLen = m_theme.m_arrowAxisLength * scale;
        const float coneLen = m_theme.m_arrowConeLength * scale;
        const float coneRadius = m_theme.m_arrowConeRadius * scale;
        // View-angle fade (Blender): dim the whole arrow as its axis points at the camera.
        const AZ::Vector3 worldAxis = manipulatorState.m_worldFromLocal.TransformVector(axis);
        const float fade = ViewFadeFactor(worldAxis, cameraState, /*isPlane=*/false);
        AZ::Color color = ResolvedColor(manipulatorState.m_mouseOver);
        color.SetA(color.GetA() * fade);
        if (fade <= 0.0f)
        {
            // Fully faded: still register the pick bound but draw nothing (matches Blender hiding).
            Picking::BoundShapeLineSegment hidden;
            hidden.m_start = manipulatorState.TransformPoint(manipulatorState.m_localPosition + axis * lineStartDist);
            hidden.m_end = manipulatorState.TransformPoint(manipulatorState.m_localPosition + axis * (lineLen + coneLen));
            hidden.m_width = coneRadius;
            RefreshBoundInternal(managerId, manipulatorId, hidden);
            return;
        }

        const AZ::Vector3 lineStart = manipulatorState.m_localPosition + axis * lineStartDist;
        const AZ::Vector3 lineEnd = manipulatorState.m_localPosition + axis * lineLen;
        const AZ::Vector3 coneBase = lineEnd + axis * (m_theme.m_arrowConeGap * scale);

        debugDisplay.PushMatrix(manipulatorState.m_worldFromLocal);
        debugDisplay.SetColor(color.GetAsVector4());
        if (m_theme.m_axisCylinderRadius > 0.0f)
        {
            // Unreal: the shaft is a 3D solid cylinder (DrawCylinder radius 1.2/35, 16 sides).
            const float cylRadius = m_theme.m_axisCylinderRadius * scale;
            const AZ::Vector3 cylCenter = (lineStart + lineEnd) * 0.5f;
            const float cylHeight = (lineEnd - lineStart).GetLength();
            debugDisplay.DrawSolidCylinder(cylCenter, axis, cylRadius, cylHeight, true);
        }
        else if (m_theme.m_drawArrowStem)
        {
            // Blender: the shaft is a thin 2px line. Hidden in the combined gizmo (draw only the
            // head) - but the picking bound below still spans the whole axis, so it stays grabbable.
            debugDisplay.SetLineWidth(m_theme.m_axisLineWidth);
            debugDisplay.DrawLine(lineStart, lineEnd);
        }
        // Arrowhead cone with the source editor's segment count (Blender 8 / Unreal 32). Our editor
        // only routes gizmo drawing through GenericDebugDisplay, so the cast is safe here.
        static_cast<GenericDebugDisplay&>(debugDisplay)
            .DrawSolidConeWithSegments(coneBase, axis, coneRadius, coneLen, m_theme.m_coneSegments);
        debugDisplay.PopMatrix();

        // Picking bound: a line segment from the PIVOT (not the shaft start) all the way to the cone
        // tip, so the handle is grabbable along its whole length regardless of where the visible stem
        // starts - this mirrors Blender's arrow test_select, which spans arrow_start..arrow_end with a
        // fixed pixel threshold (arrow3d_gizmo.cc:276-320). Width is padded well past the cone radius
        // so the thin line is easy to hit (Blender's ARROW_SELECT_THRESHOLD_PX equivalent).
        Picking::BoundShapeLineSegment lineBound;
        lineBound.m_start = manipulatorState.TransformPoint(manipulatorState.m_localPosition);
        lineBound.m_end = manipulatorState.TransformPoint(coneBase + axis * coneLen);
        lineBound.m_width = coneRadius * 2.0f;
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
        const float lineStartDist = m_theme.m_scaleLineStart * scale;
        const float lineLen = m_theme.m_scaleAxisLength * scale;
        const float halfExtent = m_theme.m_scaleBoxHalfExtent * scale;
        const AZ::Vector3 worldAxis = manipulatorState.m_worldFromLocal.TransformVector(axis);
        const float fade = ViewFadeFactor(worldAxis, cameraState, /*isPlane=*/false);
        AZ::Color color = ResolvedColor(manipulatorState.m_mouseOver);
        color.SetA(color.GetA() * fade);
        if (fade <= 0.0f)
        {
            // Fully faded: draw nothing but keep the whole-axis pick bound (same span as the visible
            // branch) so a stale/tiny bound cannot make the handle hard to grab.
            const AZ::Vector3 boxFar =
                manipulatorState.m_localPosition + axis * (m_theme.m_scaleBoxCenter * scale + halfExtent);
            Picking::BoundShapeLineSegment hidden;
            hidden.m_start = manipulatorState.TransformPoint(manipulatorState.m_localPosition);
            hidden.m_end = manipulatorState.TransformPoint(boxFar);
            hidden.m_width = halfExtent * 2.0f;
            RefreshBoundInternal(managerId, manipulatorId, hidden);
            return;
        }

        const AZ::Vector3 lineStart = manipulatorState.m_localPosition + axis * lineStartDist;
        const AZ::Vector3 lineEnd = manipulatorState.m_localPosition + axis * lineLen;
        const AZ::Vector3 boxCenter = manipulatorState.m_localPosition + axis * (m_theme.m_scaleBoxCenter * scale);
        const AZ::Vector3 halfExtents(halfExtent, halfExtent, halfExtent);

        debugDisplay.PushMatrix(manipulatorState.m_worldFromLocal);
        debugDisplay.SetColor(color.GetAsVector4());
        if (m_theme.m_axisCylinderRadius > 0.0f)
        {
            // Unreal: solid cylinder shaft (same as the move arrow) between the scale offsets.
            const float cylRadius = m_theme.m_axisCylinderRadius * scale;
            const AZ::Vector3 cylCenter = (lineStart + lineEnd) * 0.5f;
            const float cylHeight = (lineEnd - lineStart).GetLength();
            debugDisplay.DrawSolidCylinder(cylCenter, axis, cylRadius, cylHeight, true);
        }
        else
        {
            debugDisplay.SetLineWidth(m_theme.m_axisLineWidth);
            debugDisplay.DrawLine(lineStart, lineEnd);
        }
        debugDisplay.DrawSolidBox(boxCenter - halfExtents, boxCenter + halfExtents);
        debugDisplay.PopMatrix();

        // Picking bound: a line segment from the PIVOT along the axis out to the far face of the box,
        // so both the bar AND the box are grabbable along the whole handle (previously only the small
        // box had a bound, which is why the scale handles were hard to hit in the combined gizmo).
        // Mirrors Blender's arrow test_select spanning the whole axis with a pixel threshold width.
        const AZ::Vector3 boxFar = boxCenter + axis * halfExtent;
        Picking::BoundShapeLineSegment barBound;
        barBound.m_start = manipulatorState.TransformPoint(manipulatorState.m_localPosition);
        barBound.m_end = manipulatorState.TransformPoint(boxFar);
        barBound.m_width = halfExtent * 2.0f; // generous grab width (box half-extent doubled).
        RefreshBoundInternal(managerId, manipulatorId, barBound);
    }

    PlaneGizmoView::PlaneGizmoView(const GizmoTheme& theme, GizmoAxis axis1, GizmoAxis axis2, bool scaleMode)
        : ManipulatorView(/*screenSizeFixed=*/true)
        , m_theme(theme)
        , m_axis1(axis1)
        , m_axis2(axis2)
        , m_scaleMode(scaleMode)
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
        const AZ::Vector3 origin = manipulatorState.m_localPosition;
        const bool mouseOver = manipulatorState.m_mouseOver;

        // Plane handles fade with the plane normal (a1 x a2) as it turns edge-on to the view
        // (Blender g_tw_axis_range[1]); the plane view uses idot of the NORMAL axis handle.
        float fade = 1.0f;
        if (m_theme.m_axisViewFade)
        {
            const AZ::Vector3 worldNormal =
                manipulatorState.m_worldFromLocal.TransformVector(a1.Cross(a2)).GetNormalizedSafe();
            const AZ::Vector3 viewDir = cameraState.m_forward.GetNormalizedSafe();
            // Plane handles flip the dot product (idot_axis = 1 - idot) then ramp over [min,max].
            const float idot = fabsf(viewDir.Dot(worldNormal));
            const float minV = m_theme.m_planeFadeMin;
            const float maxV = m_theme.m_planeFadeMax;
            fade = (idot > maxV) ? 1.0f : (idot < minV) ? 0.0f : (idot - minV) / (maxV - minV);
        }
        if (fade <= 0.0f)
        {
            // Fully faded: draw nothing (Blender hides the plane handle) but still register the pick
            // bound so a stale bound cannot be hit, matching ArrowGizmoView's hidden-but-pickable path.
            const float ofs = m_theme.m_planeOffset * scale;
            const float span = m_theme.m_planeSize * scale;
            Picking::BoundShapeQuad hidden;
            hidden.m_corner1 = manipulatorState.TransformPoint(origin + (a1 + a2) * ofs);
            hidden.m_corner2 = manipulatorState.TransformPoint(origin + a1 * (ofs + span) + a2 * ofs);
            hidden.m_corner3 = manipulatorState.TransformPoint(origin + (a1 + a2) * (ofs + span));
            hidden.m_corner4 = manipulatorState.TransformPoint(origin + a1 * ofs + a2 * (ofs + span));
            RefreshBoundInternal(managerId, manipulatorId, hidden);
            return;
        }
        m_planeFade = fade;

        debugDisplay.PushMatrix(manipulatorState.m_worldFromLocal);

        if (m_theme.m_planeGeometry == PlaneHandleGeometry::UnrealCornerBracket)
        {
            DrawUnrealCorner(debugDisplay, managerId, manipulatorId, manipulatorState, origin, a1, a2, scale, mouseOver);
            debugDisplay.PopMatrix();
            return;
        }

        DrawBlenderDiamond(debugDisplay, managerId, manipulatorId, manipulatorState, origin, a1, a2, scale, mouseOver);
        debugDisplay.PopMatrix();
    }

    void PlaneGizmoView::DrawBlenderDiamond(
        AzFramework::DebugDisplayRequests& debugDisplay, const ManipulatorManagerId managerId,
        const ManipulatorId manipulatorId, const ManipulatorState& manipulatorState, const AZ::Vector3& origin,
        const AZ::Vector3& a1, const AZ::Vector3& a2, float scale, bool mouseOver)
    {
        // Blender ED_GIZMO_ARROW_STYLE_PLANE (arrow3d_gizmo.cc:124-147): the diamond verts
        // {0,0,0},{s,0,s},{0,0,2s},{-s,0,s} (s=0.1) live in the arrow's LOCAL x-z plane, translated
        // along local z (the arrow direction) by the plane offset 0.7. transform_gizmo_3d.cc:1396-1413
        // sets matrix_offset so that (for the XY plane) local z maps to the world diagonal (a1+a2)/sqrt2
        // and local x maps to the world perpendicular (a2-a1)/sqrt2. Substituting world =
        // colX*vx + colZ*(vz+0.7) with colX=(a2-a1)/sqrt2, colZ=(a1+a2)/sqrt2 gives the corners below.
        // The diamond's near corner therefore sits at diagonal distance 0.7*sqrt(1/2)~=0.495 per axis.
        const float s = m_theme.m_planeSize * scale;          // 0.1
        const float ofs = m_theme.m_planeOffset * scale;      // 0.7 (local z translation)
        const float t = 0.70710678f;                          // 1/sqrt(2) (M_SQRT1_2).
        const AZ::Vector3 colZ = (a1 + a2) * t;               // maps local +z (diagonal).
        const AZ::Vector3 colX = (a2 - a1) * t;               // maps local +x (across).
        const AZ::Vector3 d0 = origin + colZ * ofs;                    // {0,0,0}
        const AZ::Vector3 d1 = origin + colX * s + colZ * (ofs + s);   // {s,0,s}
        const AZ::Vector3 d2 = origin + colZ * (ofs + 2.0f * s);       // {0,0,2s}
        const AZ::Vector3 d3 = origin - colX * s + colZ * (ofs + s);   // {-s,0,s}

        // Plane handle colour is the NORMAL axis colour (transform_gizmo_3d.cc:361-389): the XY plane
        // uses TH_AXIS_Z, YZ uses TH_AXIS_X, ZX uses TH_AXIS_Y - i.e. the third axis not in {a1,a2}.
        AZ::Color axisCol = AxisColor(m_theme, PlaneNormalAxis(m_axis1, m_axis2));
        // Outline: hover keeps axis hue (alpha 1.0) per Blender; fill = same colour at alpha*0.5.
        AZ::Color outline = axisCol;
        outline.SetA((mouseOver ? m_theme.m_highlightAlpha : m_theme.m_normalAlpha) * m_planeFade);
        AZ::Color fill = axisCol;
        fill.SetA((mouseOver ? m_theme.m_highlightAlpha : m_theme.m_normalAlpha) * m_theme.m_planeFillAlpha * m_planeFade);

        debugDisplay.SetColor(fill.GetAsVector4());
        debugDisplay.DrawQuad(d0, d1, d2, d3);
        debugDisplay.SetColor(outline.GetAsVector4());
        debugDisplay.SetLineWidth(m_theme.m_planeLineWidth);
        debugDisplay.DrawLine(d0, d1);
        debugDisplay.DrawLine(d1, d2);
        debugDisplay.DrawLine(d2, d3);
        debugDisplay.DrawLine(d3, d0);

        Picking::BoundShapeQuad quadBound;
        quadBound.m_corner1 = manipulatorState.TransformPoint(d0);
        quadBound.m_corner2 = manipulatorState.TransformPoint(d1);
        quadBound.m_corner3 = manipulatorState.TransformPoint(d2);
        quadBound.m_corner4 = manipulatorState.TransformPoint(d3);
        RefreshBoundInternal(managerId, manipulatorId, quadBound);
    }

    void PlaneGizmoView::DrawUnrealCorner(
        AzFramework::DebugDisplayRequests& debugDisplay, const ManipulatorManagerId managerId,
        const ManipulatorId manipulatorId, const ManipulatorState& manipulatorState, const AZ::Vector3& origin,
        const AZ::Vector3& a1, const AZ::Vector3& a2, float scale, bool mouseOver)
    {
        // Both arms turn yellow when the plane is hovered (UnrealWidgetRender.cpp DrawDualAxis).
        AZ::Color c1 = mouseOver ? m_theme.m_highlightColor : AxisColor(m_theme, m_axis1);
        AZ::Color c2 = mouseOver ? m_theme.m_highlightColor : AxisColor(m_theme, m_axis2);
        c1.SetA(1.0f);
        c2.SetA(1.0f);

        if (m_scaleMode)
        {
            // Scale-mode plane handle: an L-shaped 2-segment line (24,0)->(12,12)->(0,24), relative
            // 0.686 / 0.343 (UnrealWidgetRender.cpp Render_Scale plane). Two axis-coloured segments.
            const float outer = 24.0f / 35.0f * scale;
            const float mid = 12.0f / 35.0f * scale;
            const AZ::Vector3 pA = origin + a1 * outer;
            const AZ::Vector3 pMid = origin + a1 * mid + a2 * mid;
            const AZ::Vector3 pB = origin + a2 * outer;
            debugDisplay.SetLineWidth(1.0f); // UnrealWidgetRender.cpp:811-812 PDI->DrawLine (hairline).
            debugDisplay.SetColor(c1.GetAsVector4());
            debugDisplay.DrawLine(pA, pMid);
            debugDisplay.SetColor(c2.GetAsVector4());
            debugDisplay.DrawLine(pMid, pB);

            Picking::BoundShapeQuad quadBound;
            quadBound.m_corner1 = manipulatorState.TransformPoint(pA);
            quadBound.m_corner2 = manipulatorState.TransformPoint(pMid);
            quadBound.m_corner3 = manipulatorState.TransformPoint(pB);
            quadBound.m_corner4 = manipulatorState.TransformPoint(origin + (a1 + a2) * mid);
            RefreshBoundInternal(managerId, manipulatorId, quadBound);
            return;
        }

        // Move-mode plane handle: two solid square-section arms forming a corner bracket. Each arm
        // spans axis 1..13 (0.029..0.371), section 0.034, meeting at (0.371,0.371) (DrawCornerHelper).
        const float armNear = 1.0f / 35.0f * scale;
        const float armFar = m_theme.m_planeOffset * scale; // 13/35 closed corner.
        const float t = m_theme.m_planeCornerThickness * scale;
        const AZ::Vector3 ringAxis = a1.Cross(a2).GetNormalized();
        const AZ::Vector3 half = ringAxis * (0.5f * t);

        // Arm 1: box along a1 from near to far, offset across a2 to sit at the far edge.
        auto drawArm = [&](const AZ::Vector3& along, const AZ::Vector3& across, const AZ::Color& col)
        {
            const AZ::Vector3 c0 = origin + along * armNear + across * (armFar - 0.5f * t);
            const AZ::Vector3 c1c = origin + along * armFar + across * (armFar - 0.5f * t);
            const AZ::Vector3 center = (c0 + c1c) * 0.5f;
            const AZ::Vector3 halfExtents = along * (0.5f * (armFar - armNear)) + across * (0.5f * t) + half;
            debugDisplay.SetColor(col.GetAsVector4());
            debugDisplay.DrawSolidBox(center - halfExtents, center + halfExtents);
        };
        drawArm(a1, a2, c1);
        drawArm(a2, a1, c2);

        // Picking bound: quad covering the corner's inner square.
        const AZ::Vector3 far1 = origin + a1 * armFar;
        const AZ::Vector3 far2 = origin + a2 * armFar;
        const AZ::Vector3 corner = origin + (a1 + a2) * armFar;
        Picking::BoundShapeQuad quadBound;
        quadBound.m_corner1 = manipulatorState.TransformPoint(origin + (a1 + a2) * armNear);
        quadBound.m_corner2 = manipulatorState.TransformPoint(far1);
        quadBound.m_corner3 = manipulatorState.TransformPoint(corner);
        quadBound.m_corner4 = manipulatorState.TransformPoint(far2);
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

        // While this axis is being rotated, the manager draws the full drag arc/ghost; suppress the
        // idle per-axis dial so it does not double-draw over it (Unreal replaces the idle quadrant
        // with the drag arc; Blender draws the full ghost). Other axes keep drawing their idle dial.
        const bool draggingThisAxis = m_dragState && m_dragState->m_rotating && m_dragState->m_axis == m_axis;
        if (draggingThisAxis)
        {
            // Still register a pick bound so hover/release stays responsive.
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
                torusBound.m_minorRadius = radius * 0.05f;
            }
            RefreshBoundInternal(managerId, manipulatorId, torusBound);
            return;
        }

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
            const bool hovered = manipulatorState.m_mouseOver;

            // Idle inner pie disc: a filled quarter disc 0..rInner in the axis colour at 0x0f alpha
            // (0x3f on hover), keeping the axis hue - not the yellow highlight (UnrealWidgetRender.cpp).
            if (m_theme.m_innerDiscAlpha > 0.0f)
            {
                AZ::Color disc = AxisColor(m_theme, m_axis);
                disc.SetA(hovered ? m_theme.m_innerDiscHoverAlpha : m_theme.m_innerDiscAlpha);
                debugDisplay.SetColor(disc.GetAsVector4());
                AZ::Vector3 prevDisc = centre + a0 * rInner;
                for (int i = 1; i <= quarterSegments; ++i)
                {
                    const float t = (static_cast<float>(i) / static_cast<float>(quarterSegments)) * halfPi;
                    const AZ::Vector3 dir = a0 * cosf(t) + a1 * sinf(t);
                    const AZ::Vector3 curDisc = centre + dir * rInner;
                    debugDisplay.DrawTri(centre, prevDisc, curDisc);
                    prevDisc = curDisc;
                }
            }

            // Filled band: quad strip between the inner and outer arcs. OuterColor.A always stays at
            // LargeOuterAlpha 0x7f; only the RGB turns yellow on hover (UnrealWidgetRender.cpp:1275).
            AZ::Color fill = hovered ? m_theme.m_highlightColor : AxisColor(m_theme, m_axis);
            fill.SetA(0x7f / 255.0f);
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

            // Bright opaque border ring lines at the outer and inner edges (RingColor = opaque axis
            // colour, yellow on hover, SDPG_Foreground).
            AZ::Color ring = hovered ? m_theme.m_highlightColor : AxisColor(m_theme, m_axis);
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
            // (ED_GIZMO_DIAL_DRAW_FLAG_CLIP), so the half of the ring behind the pivot relative to
            // the view direction is hard-clipped. GPU clipping is per-pixel; we clip each segment
            // precisely at the view plane (with DIAL_CLIP_BIAS 0.02 back-offset) so boundary segments
            // are truncated instead of over/under-drawn by up to a whole segment.
            debugDisplay.SetColor(color.GetAsVector4());
            const AZ::Vector3 centreLocal = manipulatorState.m_localPosition;
            // DIAL_CLIP_BIAS 0.02 * pixel_size (world units per screen pixel) - a sub-pixel back-off
            // that only prevents boundary jitter (dial3d_gizmo.cc:435-436). pixel_size at the pivot =
            // 2*dist*tan(fovY/2)/viewportH.
            const float dist = AZStd::max(cameraState.m_nearClip, (worldPivot - cameraState.m_position).Dot(cameraState.m_forward.GetNormalizedSafe()));
            const float viewportH = AZStd::max(1.0f, static_cast<float>(cameraState.m_viewportSize.m_height));
            const float pixelSize = 2.0f * dist * tanf(0.5f * cameraState.VerticalFovRadian()) / viewportH;
            const float clipBias = 0.02f * pixelSize;
            auto sideOf = [&](const AZ::Vector3& pt)
            {
                // Signed distance in front of (>0) / behind (<0) the camera-facing clip plane.
                return -(pt - centreLocal).Dot(localViewDir) - clipBias;
            };
            for (int i = 0; i < segments; ++i)
            {
                AZ::Vector3 p0 = ringPoint(i);
                AZ::Vector3 p1 = ringPoint(i + 1);
                const float d0 = sideOf(p0);
                const float d1 = sideOf(p1);
                if (d0 < 0.0f && d1 < 0.0f)
                {
                    continue; // wholly behind the clip plane.
                }
                if (d0 < 0.0f || d1 < 0.0f)
                {
                    // Straddles the plane: move the behind endpoint to the intersection point.
                    const float t = d0 / (d0 - d1);
                    const AZ::Vector3 hit = p0 + (p1 - p0) * t;
                    if (d0 < 0.0f)
                    {
                        p0 = hit;
                    }
                    else
                    {
                        p1 = hit;
                    }
                }
                debugDisplay.DrawLine(p0, p1);
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

    void DrawCenterHandle(
        AzFramework::DebugDisplayRequests& debugDisplay,
        const AzFramework::CameraState& cameraState,
        const AZ::Vector3& worldPivot,
        const GizmoTheme& theme)
    {
        if (theme.m_centerGeometry == CenterHandleGeometry::None)
        {
            return;
        }
        const float scale = AzToolsFramework::CalculateScreenToWorldMultiplier(worldPivot, cameraState);
        const float radius = theme.m_centerRadius * scale;
        const AZ::Color color = theme.m_centerColor;

        const AZ::Vector3 side = cameraState.m_side.GetNormalized();
        const AZ::Vector3 up = cameraState.m_up.GetNormalized();
        const int segments = AZStd::max(24, theme.m_ringSegments);
        const float twoPi = 6.2831853071795864769f;

        switch (theme.m_centerGeometry)
        {
        case CenterHandleGeometry::UnrealSolidSphere:
            debugDisplay.SetColor(color.GetAsVector4());
            debugDisplay.DrawBall(worldPivot, radius, true);
            break;
        case CenterHandleGeometry::UnrealSolidCube:
        {
            const AZ::Vector3 he(radius, radius, radius);
            debugDisplay.SetColor(color.GetAsVector4());
            debugDisplay.DrawSolidBox(worldPivot - he, worldPivot + he);
            break;
        }
        case CenterHandleGeometry::BlenderDoubleWireCircle:
        case CenterHandleGeometry::BlenderWireCircle:
        default:
        {
            debugDisplay.SetColor(color.GetAsVector4());
            debugDisplay.SetLineWidth(2.0f);
            auto drawScreenCircle = [&](float r)
            {
                AZ::Vector3 prev = worldPivot + side * r;
                for (int i = 1; i <= segments; ++i)
                {
                    const float t = (static_cast<float>(i) / static_cast<float>(segments)) * twoPi;
                    const AZ::Vector3 pt = worldPivot + (side * cosf(t) + up * sinf(t)) * r;
                    debugDisplay.DrawLine(prev, pt);
                    prev = pt;
                }
            };
            drawScreenCircle(radius);
            if (theme.m_centerGeometry == CenterHandleGeometry::BlenderDoubleWireCircle)
            {
                drawScreenCircle(theme.m_centerRadiusOuter * scale);
            }
            break;
        }
        }
    }

    void DrawTrackballViewRing(
        AzFramework::DebugDisplayRequests& debugDisplay,
        const AzFramework::CameraState& cameraState,
        const AZ::Vector3& worldPivot,
        const GizmoTheme& theme)
    {
        if (!theme.m_hasScreenRing)
        {
            return;
        }
        // Screen-aligned ring centred on the pivot, drawn once per frame. Use the same
        // screen-to-world multiplier the manipulator views use so it keeps a constant on-screen size.
        const float scale = AzToolsFramework::CalculateScreenToWorldMultiplier(worldPivot, cameraState);
        const AZ::Vector3 side = cameraState.m_side.GetNormalized();
        const AZ::Vector3 up = cameraState.m_up.GetNormalized();
        const int segments = AZStd::max(32, static_cast<int>(theme.m_ringSegments));
        const float twoPi = 6.2831853071795864769f;

        if (theme.m_screenRingBand)
        {
            // Unreal screen ring: a thick FILLED band (inner 1.964 / outer 2.0) in ScreenAxisColor,
            // opaque (UnrealWidgetRender.cpp DrawScreenSpaceRotationArc / arcball outer ring).
            const float rInner = theme.m_screenRingInner * scale;
            const float rOuter = theme.m_screenRingOuter * scale;
            debugDisplay.SetColor(theme.m_screenRingColor.GetAsVector4());
            AZ::Vector3 prevIn = worldPivot + side * rInner;
            AZ::Vector3 prevOut = worldPivot + side * rOuter;
            for (int i = 1; i <= segments; ++i)
            {
                const float t = (static_cast<float>(i) / static_cast<float>(segments)) * twoPi;
                const AZ::Vector3 dir = side * cosf(t) + up * sinf(t);
                const AZ::Vector3 curIn = worldPivot + dir * rInner;
                const AZ::Vector3 curOut = worldPivot + dir * rOuter;
                debugDisplay.DrawTri(prevIn, prevOut, curOut);
                debugDisplay.DrawTri(prevIn, curOut, curIn);
                prevIn = curIn;
                prevOut = curOut;
            }
            return;
        }

        // Blender view ring (MAN_AXIS_ROT_C): a thin screen-aligned circle, TH_GIZMO_VIEW_ALIGN
        // (white) at the base gizmo alpha 0.6, width 2.0.
        const float viewRadius = theme.m_viewRingRadius * scale;
        debugDisplay.SetColor(theme.m_screenRingColor.GetAsVector4());
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
