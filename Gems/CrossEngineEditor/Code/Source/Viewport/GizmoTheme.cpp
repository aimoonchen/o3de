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
        theme.m_highlightColor = AZ::Color(1.0f, 1.0f, 1.0f, 1.0f); // white (view ring / center only)

        // gizmo_get_axis_color: per-axis handles keep their hue; only alpha rises to 1.0 on hover
        // (copy_v4_v4(col_hi, col); col_hi[3] = alpha_hi). White is reserved for the view ring/center.
        theme.m_highlightKeepsAxisHue = true;
        theme.m_normalAlpha = 0.6f;
        theme.m_highlightAlpha = 1.0f;

        // View-angle fade (gizmo_get_axis_color alpha_fac): axis handles fade toward the camera over
        // idot [0.02,0.1]; plane handles use the flipped [0.175,0.25] window (g_tw_axis_range).
        theme.m_axisViewFade = true;
        theme.m_axisFadeMin = 0.02f;
        theme.m_axisFadeMax = 0.1f;
        theme.m_planeFadeMin = 0.175f;
        theme.m_planeFadeMax = 0.25f;

        // Move arrow (arrow3d_gizmo.cc, ED_GIZMO_ARROW_STYLE_NORMAL): Blender draws the shaft from
        // 0.2 to 1.0 with a centre wire circle filling the gap. This editor drops the centre circle
        // (user preference), so the shaft starts at 0.0 instead of 0.2 to make the three axes meet at
        // the pivot. Cone len 0.25 / radius 0.06, cone_segments 8.
        theme.m_axisLineStart = 0.0f;
        theme.m_arrowAxisLength = 1.0f;
        theme.m_arrowConeGap = 0.0f;
        theme.m_arrowConeLength = 0.25f;
        theme.m_arrowConeRadius = 0.06f;
        theme.m_coneSegments = 8;
        theme.m_axisLineWidth = 2.0f; // GIZMO_AXIS_LINE_WIDTH.
        theme.m_axisCylinderRadius = 0.0f; // draw as a line (not a solid cylinder).

        // Scale handle (ED_GIZMO_ARROW_STYLE_BOX): Blender draws the bar 0.2..1.0 with a centre
        // circle; matching the move arrow, this editor starts the bar at 0.0 so the three scale bars
        // meet at the pivot. Cube half-extent 0.05, centre at line end + size (~1.05).
        theme.m_scaleLineStart = 0.0f;
        theme.m_scaleAxisLength = 1.0f;
        theme.m_scaleBoxCenter = 1.05f;
        theme.m_scaleBoxHalfExtent = 0.05f;

        // Planar handle (arrow3d_gizmo.cc ED_GIZMO_ARROW_STYLE_PLANE): a filled diamond whose verts
        // are {0,0,0},{s,0,s},{0,0,2s},{-s,0,s} (s=0.1) translated to the line end offset
        // MAN_AXIS_SCALE_PLANE_SCALE 0.7 along each in-plane axis. Outline width 1.0
        // (transform_gizmo_3d.cc plane branch), inner fill alpha color[3]*0.5.
        theme.m_planeGeometry = PlaneHandleGeometry::BlenderDiamond;
        theme.m_planeSize = 0.1f;
        theme.m_planeOffset = 0.7f;
        theme.m_planeFillAlpha = 0.5f;
        theme.m_planeLineWidth = 1.0f;

        // Rotate rings (dial3d): per-axis thin ring, camera-facing clip; view ring; rings never fade.
        theme.m_ringRadius = 1.0f;
        theme.m_rotateBand = false;
        theme.m_ringLineWidth = 3.0f; // GIZMO_AXIS_LINE_WIDTH + 1.0 (per-axis ROT rings).
        theme.m_ringSegments = 48; // DIAL_RESOLUTION.
        theme.m_rotateGeometry = RotationRingGeometry::BlenderHalfRingFade;
        theme.m_innerDiscAlpha = 0.0f; // no inner disc.

        // Screen-aligned white view ring (MAN_AXIS_ROT_C): thin line, r=1.2, white, alpha 0.6,
        // width 2.0 (transform_gizmo_3d.cc default branch = GIZMO_AXIS_LINE_WIDTH).
        theme.m_hasScreenRing = true;
        theme.m_screenRingBand = false;
        theme.m_viewRingLineWidth = 2.0f;
        theme.m_viewRingRadius = 1.2f;
        theme.m_screenRingColor = AZ::Color(1.0f, 1.0f, 1.0f, 0.6f);

        // Center handle: disabled for this editor - the three axis handles meet at the pivot with no
        // extra ring (user preference; Blender's MAN_AXIS_TRANS_C wire circle is intentionally
        // dropped). Rotate still uses the screen-aligned view ring below.
        theme.m_centerGeometry = CenterHandleGeometry::None;
        theme.m_centerRadius = 0.2f;
        theme.m_centerRadiusOuter = 1.2f;
        theme.m_centerColor = AZ::Color(1.0f, 1.0f, 1.0f, 0.6f);

        // Rotate drag feedback: ghost arc grey {0.8,0.8,0.8,0.2}; helplines width 1.0 (start) / 3.0
        // (current) (dial3d_gizmo.cc).
        theme.m_ghostArcColor = AZ::Color(0.8f, 0.8f, 0.8f, 0.2f);
        theme.m_helplineStartWidth = 1.0f;
        theme.m_helplineCurrentWidth = 3.0f;
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

        // FWidget axis handles (arrow / box / corner / center) are OPAQUE; the 0x7f/0x0f alpha is
        // used only by the rotation arcs. Hover swaps RGB to yellow (not a hue-preserving alpha bump).
        theme.m_highlightKeepsAxisHue = false;
        theme.m_normalAlpha = 1.0f;
        theme.m_highlightAlpha = 1.0f;

        // Move arrow (Render_Axis, AxisLengthOffset 0): cylinder over AXIS_LENGTH 35 -> 0..1.0,
        // radius 1.2/35 ~= 0.0343, 16 sides. Cone: RootPos 47 -> tip 47/35 = 1.343; FScaleMatrix(-13)
        // makes the cone 13 tall so the base is at 34 -> 0.986; base radius 13*sin(15.708deg) ~= 3.52
        // -> 0.1006; 32 sides.
        theme.m_axisLineStart = 0.0f;
        theme.m_arrowAxisLength = 0.986f; // shaft cylinder ends where the cone base begins.
        theme.m_arrowConeGap = 0.0f;
        theme.m_arrowConeLength = 1.343f - 0.986f; // ~=0.357 (cone base 0.986 -> tip 1.343).
        theme.m_arrowConeRadius = 0.1006f;
        theme.m_coneSegments = 32;
        theme.m_axisLineWidth = 1.0f; // shaft drawn as a solid cylinder, not a wide line.
        theme.m_axisCylinderRadius = 1.2f / 35.0f; // ~=0.0343.
        theme.m_axisCylinderSegments = 16;

        // Scale handle (Render_Axis bCubeHead, AxisLengthOffset AXIS_LENGTH_SCALE_OFFSET 5):
        // AxisLength = 35 - 2*5 = 25, cylinder offset 5..30 -> 0.143..0.857; cube RootPos
        // 25+3+5 = 33 -> 0.943, half-extent 4/35 = 0.114.
        theme.m_scaleLineStart = 5.0f / 35.0f; // 0.143.
        theme.m_scaleAxisLength = 30.0f / 35.0f; // 0.857.
        theme.m_scaleBoxCenter = 33.0f / 35.0f; // 0.943.
        theme.m_scaleBoxHalfExtent = 4.0f / 35.0f; // 0.114.

        // Planar handle: UE DrawDualAxis draws two solid square-section arms (DrawCornerHelper),
        // CornerPos (7,0,7), AxisSize (12,1.2,12), CornerLength 1.2; each arm spans axis 1..13 ->
        // 0.029..0.371, section 1.2/35 ~= 0.034, meeting at (13,13)/35 ~= (0.371,0.371).
        theme.m_planeGeometry = PlaneHandleGeometry::UnrealCornerBracket;
        theme.m_planeSize = 12.0f / 35.0f; // arm length 0.343.
        theme.m_planeOffset = 13.0f / 35.0f; // closed corner at 0.371.
        theme.m_planeCornerThickness = 1.2f / 35.0f; // 0.034.

        // Rotate rings: THICK FILLED annulus band, INNER 48/35 ~= 1.371 / OUTER 56/35 = 1.6, idle
        // draws the camera-facing quarter (PI/2). AXIS_CIRCLE_SIDES 24 per quadrant -> full ring 96.
        theme.m_ringRadius = 56.0f / 35.0f; // border ring at the outer edge.
        theme.m_rotateBand = true;
        theme.m_ringInnerRadius = 48.0f / 35.0f; // 1.371.
        theme.m_ringOuterRadius = 56.0f / 35.0f; // 1.600.
        theme.m_ringLineWidth = 1.0f; // bright RingColor border (SDPG_Foreground), thin.
        theme.m_ringSegments = 96;
        theme.m_rotateGeometry = RotationRingGeometry::UnrealCameraFacingQuarter;
        // Idle inner pie disc 0..1.371 at axis alpha 0x0f, hover 0x3f (UnrealWidgetRender.cpp).
        theme.m_innerDiscAlpha = 0x0f / 255.0f;
        theme.m_innerDiscHoverAlpha = 0x3f / 255.0f;

        // Screen-aligned outer ring: a thick FILLED band, inner 55*1.25/35 = 1.964, outer
        // 56*1.25/35 = 2.0, ScreenAxisColor (0.76,0.72,0.14) opaque, yellow on hover/drag.
        theme.m_hasScreenRing = true;
        theme.m_screenRingBand = true;
        theme.m_viewRingLineWidth = 1.0f;
        theme.m_screenRingInner = 55.0f * 1.25f / 35.0f; // 1.964.
        theme.m_screenRingOuter = 56.0f * 1.25f / 35.0f; // 2.000.
        theme.m_screenRingColor = AZ::Color(0.76f, 0.72f, 0.14f, 1.0f);

        // Center handle: move = opaque white solid sphere r=4/35 = 0.114; scale = opaque white solid
        // cube half-extent 0.114. The rotate build clears this (arcball is opt-in).
        theme.m_centerGeometry = CenterHandleGeometry::UnrealSolidSphere;
        theme.m_centerRadius = 4.0f / 35.0f; // 0.114.
        theme.m_centerColor = AZ::Color(1.0f, 1.0f, 1.0f, 1.0f);

        // Rotate drag feedback: markers/arcs are yellow; handled in the view/manager per-style.
        // (m_ghostArcColor is a Blender-only field - UE drag feedback uses m_highlightColor - so it
        // is left at its struct default here.)
        theme.m_helplineStartWidth = 1.0f;
        theme.m_helplineCurrentWidth = 1.0f;
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
