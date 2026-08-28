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
//!
//! Godot 4.x (solid-filled handles look; reference recipe only - the active Blender theme is
//! unchanged. All values below are normalised to OUR unit system: ring radius 1.0 == Godot's
//! GIZMO_CIRCLE_SIZE 1.1, i.e. Godot values divided by 1.1):
//!   - size model: pixel-literal. gizmo_scale = manipulator_gizmo_size / px-per-world-unit at
//!     the gizmo depth (node_3d_editor_viewport.cpp:5018-5032); setting
//!     editors/3d/manipulator_gizmo_size default 80px, range 16-160 (editor_settings.cpp:1018).
//!   - constants (node_3d_editor_constants.h): GIZMO_CIRCLE_SIZE 1.1, GIZMO_RING_HALF_WIDTH 0.1
//!     (picking bound only - node_3d_editor_viewport.cpp:1581-1624, never rendered),
//!     GIZMO_ARROW_SIZE 0.35, GIZMO_ARROW_OFFSET = GIZMO_SCALE_OFFSET = 1.4,
//!     GIZMO_PLANE_SIZE 0.2, GIZMO_PLANE_DST 0.3.
//!   - mesh profiles (node_3d_editor_plugin.cpp): move arrow = 16-sided revolve, thin wire shaft
//!     (radius 0.01, ~1.6px) flaring to 0.065 at the cone base, cone tip at 1.75; scale handle =
//!     4-sided (square) revolve, wire 0.01 to 1.4, solid square block half-width 0.07 from 1.4 to
//!     1.554; rotation rings (all 4, incl. view rotation) = FLAT circle r=1.1, 128 segments, no
//!     camera clip - thickness is a shader normal-offset 0.02 (~3.2px) on the axis rings / 0.008
//!     (~1.3px, x1.14 in 4.7) on the view ring, so they read as thin lines, NOT filled bands;
//!     planes = FILLED square, near corner 0.3 per axis spanning to 0.5 per axis (full side 0.2).
//!
//!   To restyle the Blender theme with Godot's visual weight, set in MakeBlenderGizmoTheme():
//!     m_arrowAxisLength = 1.27; m_arrowConeLength = 0.32; m_arrowConeRadius = 0.059;
//!     m_coneSegments = 16;  (shaft stays a line: Godot's wire is only ~1.6px, vs our 2px line)
//!     m_scaleAxisLength = 1.27; m_scaleBoxCenter = 1.343; m_scaleBoxHalfExtent = 0.064;
//!     (Godot's scale block is 0.14 wide x 0.154 long - CEE draws a cube, close enough)
//!     m_ringSegments = 128; keep m_rotateBand = false (Godot rings are THIN lines ~3.2px, not
//!     filled bands - keep m_ringLineWidth 3.0, within a pixel);
//!     m_rotateGeometry = FullRing (new enum value: complete circle without the Blender half-ring
//!     clip, ~15 lines in GizmoViews.cpp);
//!     m_planeOffset = 0.386; m_planeSize = 0.129;
//!     (CEE offset = DIAGONAL distance to the near corner, size = across half-width; Godot's
//!      square has corners at 0.3/0.5 per axis -> diagonal 0.4243, across half 0.1414, /1.1.
//!      CEE draws a diamond: its 4 corners land on Godot's square corners, the edges differ.)
//!     View ring: m_viewRingRadius = 1.14; m_viewRingLineWidth = 1.5; keep m_screenRingBand =
//!     false (the band is Unreal's). (4.7+ scales the view ring 1.14x beyond the axis rings via
//!     view_plane_rotation_gizmo_scale, editor_settings.cpp:1021; on <= 4.6 use 1.0 / 1.3.)
//!
//!   Colors & highlight (Godot 4.x defaults; the geometry recipe above keeps Blender colours):
//!     m_axisXColor = (0.96,0.20,0.32); m_axisYColor = (0.53,0.84,0.01);
//!     m_axisZColor = (0.16,0.55,0.96)  (theme_modern.cpp:228-230);
//!     m_normalAlpha = 0.9; m_highlightAlpha = 1.0
//!     (every handle alpha = theme alpha x manipulator_gizmo_opacity, default 0.9 -
//!     editor_settings.cpp:1019 + plugin.cpp:1282; the hover albedo's alpha is a constant 1.0,
//!     plugin.cpp:1294);
//!     m_planeFillAlpha = 1.0  (Godot planes are filled at the full axis alpha x 0.9 opacity,
//!     not Blender's 0.5 relative fill; Godot planes also have NO outline - CEE's diamond
//!     outline is a Blenderism, skip it for exact parity);
//!     m_screenRingColor = (0.75,0.75,0.75,0.3)  (axis_view_plane_color (0.75,0.75,0.75,0.33)
//!     x 0.9 opacity, theme_modern.cpp:232; CEE's screen ring ignores m_normalAlpha, so the
//!     alpha is baked into the colour);
//!     hover tint = from_hsv(h, s x 0.25, 1, 1) per axis - keep hue, desaturate to 25%, full
//!     value (plugin.cpp:1294; formula = Color::set_hsv, core/math/color.cpp). With the axis
//!     colours above: X (1.00,0.80,0.83), Y (0.91,1.00,0.75), Z (0.79,0.89,1.00).
//!     CEE's Blender highlight (m_highlightKeepsAxisHue = true) raises alpha but never
//!     desaturates, and there is no per-axis hover-colour field - exact parity needs the pale
//!     tint computed per axis at highlight time; alpha 0.9 -> 1.0 alone is the close approx.
//!     Exact pixel parity (optional): ManipulatorViewBaseScale = 0.941 (Godot ring 1.1 x 80px =
//!     88px / our 93.5px per unit @1080p/60deg via CalculateScreenToWorldMultiplier =
//!     max(d, near)/10). Shrinks the Blender/Unreal themes too, but the screen ring + center
//!     handle draw with bare CalculateScreenToWorldMultiplier (GizmoViews.cpp:754/683) and do
//!     NOT follow BaseScale - either route them through ManipulatorViewScaleMultiplier or bump
//!     m_viewRingRadius to ~1.21 (1.14/0.941) if BaseScale is applied.

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
