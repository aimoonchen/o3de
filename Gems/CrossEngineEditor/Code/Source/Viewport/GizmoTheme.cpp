/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/GizmoTheme.h>

namespace CrossEngineEditor
{
    GizmoTheme MakeBlenderGizmoTheme()
    {
        GizmoTheme theme;

        // Axis colors: Blender userdef_default_theme.c (TH_AXIS_X/Y/Z).
        theme.m_axisXColor = AZ::Color(1.0f, 0.200f, 0.322f, 1.0f); // #ff3352
        theme.m_axisYColor = AZ::Color(0.545f, 0.863f, 0.0f, 1.0f); // #8bdc00
        theme.m_axisZColor = AZ::Color(0.157f, 0.565f, 1.0f, 1.0f); // #2890ff
        theme.m_highlightColor = AZ::Color(1.0f, 1.0f, 1.0f, 1.0f); // gizmo_hi

        // gizmo_get_axis_color: normal alpha 0.6, highlighted alpha 1.0.
        theme.m_normalAlpha = 0.6f;
        theme.m_highlightAlpha = 1.0f;

        // Move arrow (arrow3d_gizmo.cc, ED_GIZMO_ARROW_STYLE_NORMAL):
        // line from 0 to length, cone len 0.25 / width 0.06, 8 segments.
        theme.m_arrowAxisLength = 1.0f;
        theme.m_arrowConeLength = 0.25f;
        theme.m_arrowConeRadius = 0.06f;
        theme.m_axisLineWidth = 2.0f; // GIZMO_AXIS_LINE_WIDTH.

        // Scale handle (ED_GIZMO_ARROW_STYLE_BOX): unit cube scaled by size 0.05 (half extent),
        // placed at arrow_length + size so the cube base touches the line end (gap 0).
        theme.m_scaleAxisLength = 1.0f;
        theme.m_scaleBoxGap = 0.0f;
        theme.m_scaleBoxHalfExtent = 0.05f;

        // Planar handles (arrow3d_gizmo.cc ED_GIZMO_ARROW_STYLE_PLANE): a filled diamond drawn at
        // the axis line end, offset MAN_AXIS_SCALE_PLANE_SCALE 0.7 along each axis; the diamond
        // spans +/-0.1 (verts {s,s},{-s,s},...). Inner fill alpha color[3]*0.5.
        theme.m_planeGeometry = PlaneHandleGeometry::BlenderDiamond;
        theme.m_planeSize = 0.1f;
        theme.m_planeOffset = 0.7f;
        theme.m_planeFillAlpha = 0.5f;

        // Rotate rings (dial3d): per-axis rings + view ring; rings never fade.
        theme.m_ringRadius = 1.0f;
        theme.m_ringLineWidth = 3.0f; // GIZMO_AXIS_LINE_WIDTH + 1.0.
        theme.m_viewRingLineWidth = 2.0f; // MAN_AXIS_ROT_C default branch = GIZMO_AXIS_LINE_WIDTH.
        theme.m_viewRingRadius = 1.2f; // transform_gizmo_3d.cc MAN_AXIS_ROT_C WM_gizmo_set_scale 1.2.
        theme.m_ringSegments = 48.0f; // DIAL_RESOLUTION (gizmo_library_intern.hh) full ring = 48.
        theme.m_rotateGeometry = RotationRingGeometry::BlenderHalfRingFade;

        theme.m_centerRadius = 0.2f; // MAN_AXIS_TRANS_C WM_gizmo_set_scale 0.2.
        return theme;
    }

    GizmoTheme MakeUnrealGizmoTheme()
    {
        GizmoTheme theme;

        // Axis colors: UnrealEd AxisDisplayInfo::GetAxisColor (linear, transcribed from source).
        theme.m_axisXColor = AZ::Color(0.594f, 0.0197f, 0.0f, 1.0f);
        theme.m_axisYColor = AZ::Color(0.1349f, 0.3959f, 0.0f, 1.0f);
        theme.m_axisZColor = AZ::Color(0.0251f, 0.207f, 0.85f, 1.0f);
        theme.m_highlightColor = AZ::Color(1.0f, 1.0f, 0.0f, 1.0f); // FWidget::CurrentColor = Yellow.

        // FWidget arcs use LargeOuterAlpha 0x7f for the resting handle; highlight is opaque.
        theme.m_normalAlpha = 0x7f / 255.0f;
        theme.m_highlightAlpha = 1.0f;

        // Move arrow (Render_Axis): cylinder over AXIS_LENGTH 35 with a cone head. The cone sits
        // ConeHeadOffset 12 past the axis end and is length 13 / base radius 13*tan(PI*5 deg)~=3.65
        // (DrawCone(FScaleMatrix(-13), angle=DegreesToRadians(PI*5))). Normalised by 35:
        // gap 12/35~=0.343, length 13/35~=0.371, radius 3.65/35~=0.104.
        theme.m_arrowAxisLength = 1.0f;
        theme.m_arrowConeGap = 0.343f;
        theme.m_arrowConeLength = 0.371f;
        theme.m_arrowConeRadius = 0.104f;
        theme.m_axisLineWidth = 3.0f;

        // Scale handle (Render_Cube head FVector(4.0) at RootPos AXIS_LENGTH+CubeHeadOffset 35+3):
        // normalised by 35 -> gap 3/35~=0.086, cube half-extent 4/35~=0.114.
        theme.m_scaleAxisLength = 1.0f;
        theme.m_scaleBoxGap = 0.086f;
        theme.m_scaleBoxHalfExtent = 0.114f;

        // Planar handles: UE DrawDualAxis draws an L-shaped corner bracket at CornerPos (7,7) with
        // arm length 12 and thickness 1.2 (over AXIS_LENGTH 35 -> offset ~0.2, arm ~0.34).
        theme.m_planeGeometry = PlaneHandleGeometry::UnrealCornerBracket;
        theme.m_planeSize = 0.34f;
        theme.m_planeOffset = 0.2f;
        theme.m_planeFillAlpha = 0.35f;

        // Rotate rings: Unreal draws each axis handle as a THICK FILLED annulus band, not a line.
        // Radii relative to AXIS_LENGTH 35: INNER_AXIS_CIRCLE_RADIUS 48 -> 48/35 ~= 1.371,
        // OUTER_AXIS_CIRCLE_RADIUS 56 -> 56/35 = 1.6 (UnrealWidgetRender.cpp DrawPartialRotationArc /
        // DrawThickArc). Idle draws a camera-facing quarter arc (PI/2) of that band.
        theme.m_rotateBand = true;
        theme.m_ringInnerRadius = 48.0f / 35.0f; // ~=1.371
        theme.m_ringOuterRadius = 56.0f / 35.0f; // =1.600
        theme.m_ringRadius = theme.m_ringOuterRadius; // border ring drawn at the outer edge.
        theme.m_ringLineWidth = 1.0f; // bright RingColor border line (SDPG_Foreground), thin.
        theme.m_viewRingLineWidth = 3.0f; // Unreal screen-rotate outline matches the ring width.
        theme.m_viewRingRadius = 1.16f; // OUTER/INNER ratio 56/48.
        theme.m_ringSegments = 96.0f; // AXIS_CIRCLE_SIDES 24 per PI/2 quadrant -> full ring 96.
        theme.m_rotateGeometry = RotationRingGeometry::UnrealCameraFacingQuarter;

        theme.m_centerRadius = 0.114f; // screen-space sphere radius 4 over AXIS_LENGTH 35.
        return theme;
    }

    GizmoTheme MakeGizmoTheme(GizmoStyle style)
    {
        switch (style)
        {
        case GizmoStyle::Unreal:
            return MakeUnrealGizmoTheme();
        case GizmoStyle::Blender:
        default:
            return MakeBlenderGizmoTheme();
        }
    }
} // namespace CrossEngineEditor
