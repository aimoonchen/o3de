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
//! reusing O3DE's low-level manipulators. Each theme is a small set of geometry constants +
//! colors; the manipulator views (ArrowGizmoView / BoxGizmoView / ...) read the active
//! theme when tessellating.
//!
//! The Blender values are transcribed verbatim from Blender's own source (not guessed):
//!   - axis colors: userdef_default_theme.c (TH_AXIS_X/Y/Z)
//!   - handle geometry: editors/transform/transform_gizmo_3d.cc +
//!     editors/gizmo_library/gizmo_types/arrow3d_gizmo.cc / dial3d_gizmo.cc
//!   - normal alpha 0.6 / highlight alpha 1.0, rings never fade (gizmo_get_axis_color)

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

    //! All tunables needed to draw the three transform gizmos for one style. Values are in
    //! the manipulator's local, screen-fixed space (1.0 == the base handle length), matching
    //! how both O3DE manipulators and Blender's gizmos author their geometry.
    //! Rotation ring geometry differs between the two editors, not just its colour:
    //!   - Blender dials draw a single-colour ring hard-clipped by a camera-facing clip plane, so
    //!     the half of the ring behind the pivot is not drawn (dial3d_gizmo.cc DIAL_CLIP).
    //!   - Unreal draws only the quarter-ring (PI/2 arc) in the camera-facing quadrant when idle
    //!     (UnrealWidgetRender.cpp FWidget::DrawRotationArc, non-dragging branch).
    enum class RotationRingGeometry
    {
        BlenderHalfRingFade,
        UnrealCameraFacingQuarter
    };

    //! Planar (two-axis) handle shape differs between the two editors:
    //!   - Blender draws a filled diamond centred on the axis line end, offset ~0.7 along the
    //!     axis, tinted by one of its axes at half alpha (arrow3d_gizmo.cc ED_GIZMO_ARROW_STYLE_PLANE).
    //!   - Unreal draws an L-shaped corner bracket (two axis-coloured arms) near the pivot
    //!     (UnrealWidgetRender.cpp DrawDualAxis / DrawCornerHelper).
    enum class PlaneHandleGeometry
    {
        BlenderDiamond,
        UnrealCornerBracket
    };

    struct GizmoTheme
    {
        // --- Axis colors ---
        AZ::Color m_axisXColor;
        AZ::Color m_axisYColor;
        AZ::Color m_axisZColor;
        //! Mouse-over / active highlight color.
        AZ::Color m_highlightColor;
        //! Alpha for a resting handle and for a highlighted handle.
        float m_normalAlpha = 1.0f;
        float m_highlightAlpha = 1.0f;

        // --- Move: axis arrow (line + cone head) ---
        float m_arrowAxisLength = 1.0f; //!< Length of the axis line (handle base length).
        float m_arrowConeGap = 0.0f; //!< Gap between the line end and the cone base.
        float m_arrowConeLength = 0.25f; //!< Cone (arrow head) length past the line end.
        float m_arrowConeRadius = 0.06f; //!< Cone base radius.
        float m_axisLineWidth = 2.0f; //!< Screen-space line width for axis lines.

        // --- Scale: axis handle (line + box tip) ---
        float m_scaleAxisLength = 1.0f;
        float m_scaleBoxGap = 0.0f; //!< Gap between the line end and the cube base.
        float m_scaleBoxHalfExtent = 0.05f; //!< Half-size of the cube tip.

        // --- Planar handle (move/scale corner squares) ---
        float m_planeSize = 0.2f; //!< Edge / arm length of the planar handle.
        float m_planeOffset = 0.4f; //!< Distance of the handle from the pivot along each axis.
        float m_planeFillAlpha = 0.5f; //!< Inner fill alpha relative to the axis color.
        PlaneHandleGeometry m_planeGeometry = PlaneHandleGeometry::BlenderDiamond;

        // --- Rotate: dial rings ---
        float m_ringRadius = 1.0f; //!< Radius of the per-axis rotation rings.
        //! Unreal draws each rotation handle as a THICK FILLED annulus band between an inner and
        //! outer radius (UnrealWidgetRender.cpp DrawThickArc: INNER_AXIS_CIRCLE_RADIUS 48 /
        //! OUTER_AXIS_CIRCLE_RADIUS 56 over AXIS_LENGTH 35). When m_rotateBand is true the dial view
        //! fills the band and draws a bright border ring; when false it is a single thin line ring
        //! (Blender). Radii are relative to the handle-base length like every other constant here.
        bool m_rotateBand = false;
        float m_ringInnerRadius = 1.37f; //!< 48/35, band inner edge (Unreal).
        float m_ringOuterRadius = 1.6f; //!< 56/35, band outer edge (Unreal).
        float m_ringLineWidth = 3.0f; //!< Per-axis rings (Blender ROT_X/Y/Z = GIZMO_AXIS_LINE_WIDTH+1 = 3).
        //! Screen-aligned view ring line width. Blender MAN_AXIS_ROT_C falls through to the default
        //! branch = GIZMO_AXIS_LINE_WIDTH = 2.0 (transform_gizmo_3d.cc:1516), NOT the +1.0 used by the
        //! per-axis rings. Keep this separate so the view ring is not 50% too thick.
        float m_viewRingLineWidth = 2.0f;
        float m_viewRingRadius = 1.15f; //!< Outer view-aligned ring (trackball outline).
        float m_ringSegments = 64.0f; //!< Tessellation of a full ring.
        RotationRingGeometry m_rotateGeometry = RotationRingGeometry::BlenderHalfRingFade;

        // --- Center handle (uniform move/scale, screen-space circle) ---
        float m_centerRadius = 0.1f;
    };

    //! Blender 5.x gizmo theme (values sourced from Blender's own code, see file header).
    GizmoTheme MakeBlenderGizmoTheme();

    //! Unreal-style gizmo theme (brighter, opaque, thicker handles; arrowheads + boxes).
    GizmoTheme MakeUnrealGizmoTheme();

    //! Build a theme for the requested style.
    GizmoTheme MakeGizmoTheme(GizmoStyle style);
} // namespace CrossEngineEditor
