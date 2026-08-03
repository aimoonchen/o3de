/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Gizmo visual themes (plan: route B, fully self-drawn transform gizmos).
//!
//! The editor draws its own transform gizmos (move / rotate / scale) so the look can be
//! switched between engine conventions while the picking / transform / undo plumbing keeps
//! reusing O3DE's low-level manipulators. Each theme is a set of geometry constants +
//! colors; the manipulator views (ArrowGizmoView / BoxGizmoView / ...) read the active
//! theme when tessellating.
//!
//! Every value is transcribed verbatim from the respective editor's own source (not guessed):
//!   Blender (5.x):
//!     - axis colors: userdef_default_theme.c (TH_AXIS_X/Y/Z)
//!     - handle geometry: editors/transform/transform_gizmo_3d.cc +
//!       editors/gizmo_library/gizmo_types/arrow3d_gizmo.cc / dial3d_gizmo.cc
//!     - normal alpha 0.6 / highlight alpha 1.0, hue kept (gizmo_get_axis_color copies col->col_hi)
//!   Unreal (legacy FWidget):
//!     - AXIS_LENGTH 35, INNER/OUTER circle 48/56, TEXT 75, sides 24, arrow segs 16
//!       (UnrealWidget.h), colours AxisDisplayInfo.cpp, geometry UnrealWidgetRender.cpp.
//!
//! Lengths are in the manipulator's local, screen-fixed space (1.0 == the base handle length,
//! i.e. Unreal's AXIS_LENGTH 35 or Blender's arrow length 1.0), matching how both O3DE
//! manipulators and the source editors author their geometry.

#include <AzCore/Math/Color.h>
#include <AzCore/std/string/string.h>

namespace CrossEngineEditor
{
    //! Which editor's gizmo look to emulate.
    enum class GizmoStyle
    {
        Blender,
        Unreal
    };

    //! Per-axis rotation ring geometry differs between the two editors:
    //!   - Blender dials draw a single-colour thin ring hard-clipped by a camera-facing clip plane,
    //!     so the half of the ring behind the pivot is not drawn (dial3d_gizmo.cc DIAL_CLIP).
    //!   - Unreal draws only the camera-facing quarter (PI/2) of a THICK FILLED annulus band when
    //!     idle (UnrealWidgetRender.cpp DrawRotationArc idle branch -> DrawThickArc).
    enum class RotationRingGeometry
    {
        BlenderHalfRingFade,
        UnrealCameraFacingQuarter
    };

    //! Planar (two-axis) handle shape differs between the two editors:
    //!   - Blender draws a filled diamond whose near corner sits at the axis line end offset 0.7
    //!     along each axis (arrow3d_gizmo.cc ED_GIZMO_ARROW_STYLE_PLANE).
    //!   - Unreal draws an L-shaped corner bracket of two solid square-section arms near the pivot
    //!     (UnrealWidgetRender.cpp DrawDualAxis / DrawCornerHelper).
    enum class PlaneHandleGeometry
    {
        BlenderDiamond,
        UnrealCornerBracket
    };

    //! Central handle geometry (uniform move / scale / free rotate):
    //!   - Blender move: wireframe screen-aligned circle r=0.2 (MAN_AXIS_TRANS_C).
    //!   - Blender scale: two concentric wireframe circles r=0.2 and r=1.2 (MAN_AXIS_SCALE_C annulus).
    //!   - Unreal move: opaque white solid sphere r=0.114 (Render_Axis center).
    //!   - Unreal scale: opaque white solid cube half-extent 0.114 (Render_Cube center).
    enum class CenterHandleGeometry
    {
        None,
        BlenderWireCircle,
        BlenderDoubleWireCircle,
        UnrealSolidSphere,
        UnrealSolidCube
    };

    struct GizmoTheme
    {
        // --- Axis colors ---
        AZ::Color m_axisXColor;
        AZ::Color m_axisYColor;
        AZ::Color m_axisZColor;
        //! Mouse-over / active highlight color. Blender per-axis handles do NOT use this (they keep
        //! their hue, only alpha rises to 1.0); it is used by Blender's white view ring/center and
        //! by every Unreal handle (which turns Yellow on hover/drag).
        AZ::Color m_highlightColor;
        //! When true (Blender), a hovered per-axis handle keeps its own axis colour and only raises
        //! alpha to m_highlightAlpha; when false (Unreal) it switches RGB to m_highlightColor.
        bool m_highlightKeepsAxisHue = false;
        //! Alpha for a resting handle and for a highlighted handle.
        float m_normalAlpha = 1.0f;
        float m_highlightAlpha = 1.0f;

        //! View-angle fade (Blender gizmo_get_axis_color alpha_fac): line/scale axis handles fade out
        //! as their axis points toward the camera, over the idot window [min,max]; plane handles use
        //! a wider, flipped window. idot = 1 - |dot(viewDir, axis)|. Off (Unreal) keeps full alpha.
        bool m_axisViewFade = false;
        float m_axisFadeMin = 0.02f;
        float m_axisFadeMax = 0.1f;
        float m_planeFadeMin = 0.175f;
        float m_planeFadeMax = 0.25f;

        // --- Axis shaft (move arrow / scale bar) ---
        //! Line/cylinder start offset from the pivot along the axis (Blender 0.2, Unreal 0.0).
        float m_axisLineStart = 0.0f;
        float m_arrowAxisLength = 1.0f; //!< End of the axis shaft (handle base length).
        float m_arrowConeGap = 0.0f; //!< Gap between the shaft end and the cone base.
        float m_arrowConeLength = 0.25f; //!< Cone (arrow head) length past the shaft end.
        float m_arrowConeRadius = 0.06f; //!< Cone base radius.
        int m_coneSegments = 8; //!< Cone tessellation (Blender 8, Unreal 32).
        float m_axisLineWidth = 2.0f; //!< Screen-space line width for axis lines (Blender thin-line style).
        //! Whether to draw the move-arrow stem (shaft) at all. Blender hides it (draw_options=0) in
        //! the combined gizmo so only the arrow head shows (transform_gizmo_3d.cc:1539-1543); the
        //! picking bound still spans the whole axis so the handle stays easy to grab.
        bool m_drawArrowStem = true;
        //! Unreal draws the shaft as a 3D solid cylinder instead of a line; 0 == draw as a line
        //! (Blender). Unreal radius 1.2/35 ~= 0.0343, 16 sides.
        float m_axisCylinderRadius = 0.0f;
        int m_axisCylinderSegments = 16;

        // --- Scale: axis handle (line + box tip) ---
        float m_scaleLineStart = 0.0f; //!< Blender 0.2, Unreal 0.143.
        float m_scaleAxisLength = 1.0f; //!< Blender 1.0, Unreal 0.857 (cube sits past it).
        float m_scaleBoxCenter = 1.05f; //!< Cube centre along the axis (Blender 1.05, Unreal 0.943).
        float m_scaleBoxHalfExtent = 0.05f; //!< Half-size of the cube tip (Blender 0.05, Unreal 0.114).

        // --- Planar handle (move/scale corner handles) ---
        float m_planeSize = 0.1f; //!< Blender diamond half-size / Unreal arm length.
        float m_planeOffset = 0.7f; //!< Distance of the handle from the pivot along each axis.
        float m_planeFillAlpha = 0.5f; //!< Inner fill alpha relative to the axis color (Blender).
        float m_planeLineWidth = 1.0f; //!< Blender plane outline width (transform_gizmo_3d.cc plane branch = 1.0).
        float m_planeCornerThickness = 0.034f; //!< Unreal solid arm square-section thickness (1.2/35).
        PlaneHandleGeometry m_planeGeometry = PlaneHandleGeometry::BlenderDiamond;

        // --- Rotate: per-axis dial ---
        float m_ringRadius = 1.0f; //!< Blender thin ring radius.
        bool m_rotateBand = false; //!< Unreal: fill a thick annulus band instead of a thin line ring.
        float m_ringInnerRadius = 1.37f; //!< 48/35, band inner edge (Unreal).
        float m_ringOuterRadius = 1.6f; //!< 56/35, band outer edge (Unreal).
        float m_ringLineWidth = 3.0f; //!< Blender ROT_X/Y/Z = GIZMO_AXIS_LINE_WIDTH+1 = 3 / Unreal border 1.
        int m_ringSegments = 48; //!< Blender DIAL_RESOLUTION 48 / Unreal 96 (24 per quadrant).
        RotationRingGeometry m_rotateGeometry = RotationRingGeometry::BlenderHalfRingFade;
        //! Unreal idle inner pie disc alpha (0x0f) and its hover alpha (0x3f); 0 disables (Blender).
        float m_innerDiscAlpha = 0.0f;
        float m_innerDiscHoverAlpha = 0.0f;

        // --- Rotate: screen-aligned outer ring ---
        //! Blender view ring: a thin white screen-aligned circle (MAN_AXIS_ROT_C, r=1.2, white,
        //! alpha 0.6, width 2). Unreal screen ring: a thick FILLED band (inner 1.964 / outer 2.0,
        //! ScreenAxisColor) that turns yellow on hover/drag. m_screenRingBand selects which.
        bool m_hasScreenRing = false;
        bool m_screenRingBand = false; //!< Unreal filled band vs Blender thin line.
        float m_viewRingLineWidth = 2.0f; //!< Blender thin ring width.
        float m_viewRingRadius = 1.2f; //!< Blender thin ring radius.
        float m_screenRingInner = 1.964f; //!< Unreal band inner (55*1.25/35).
        float m_screenRingOuter = 2.0f; //!< Unreal band outer (56*1.25/35).
        AZ::Color m_screenRingColor = AZ::Color(1.0f, 1.0f, 1.0f, 0.6f); //!< Blender white / Unreal ScreenAxisColor.

        // --- Center handle (uniform move/scale / free rotate) ---
        CenterHandleGeometry m_centerGeometry = CenterHandleGeometry::None;
        float m_centerRadius = 0.2f; //!< Primary center handle radius.
        float m_centerRadiusOuter = 1.2f; //!< Blender scale annulus outer circle radius.
        AZ::Color m_centerColor = AZ::Color(1.0f, 1.0f, 1.0f, 0.6f);

        // --- Rotate drag feedback ---
        //! Blender ghost arc fill colour {0.8,0.8,0.8,0.2} (dial3d_gizmo.cc), NOT the axis colour.
        AZ::Color m_ghostArcColor = AZ::Color(0.8f, 0.8f, 0.8f, 0.2f);
        float m_helplineStartWidth = 1.0f; //!< Blender start helpline width 1.0.
        float m_helplineCurrentWidth = 3.0f; //!< Blender current helpline width 3.0.
    };

    //! Blender 5.x gizmo theme (values sourced from Blender's own code, see file header).
    GizmoTheme MakeBlenderGizmoTheme();

    //! Unreal-style gizmo theme (legacy FWidget: opaque handles, solid cylinders/spheres, thick
    //! filled rotation bands + yellow highlight).
    GizmoTheme MakeUnrealGizmoTheme();

    //! Build a theme for the requested style.
    GizmoTheme MakeGizmoTheme(GizmoStyle style);
} // namespace CrossEngineEditor
