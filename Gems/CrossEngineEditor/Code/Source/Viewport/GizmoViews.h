/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Self-drawn transform gizmo views (plan: route B). Each view subclasses O3DE's
//! ManipulatorView so the low-level manipulators keep handling picking / hover / drag /
//! undo, while the geometry is tessellated here to match the active GizmoTheme (Blender or
//! Unreal). Every view registers a picking bound so the reused manipulators can hit-test.
//!
//! Views are screen-size fixed: geometry is authored in the manipulator's local space where
//! the base handle length is 1.0, then scaled by the camera-distance multiplier - same
//! convention Blender and O3DE use for constant on-screen size.

#include <Viewport/GizmoTheme.h>

#include <AzToolsFramework/Manipulators/ManipulatorView.h>

namespace CrossEngineEditor
{
    //! Which axis a directional handle acts on (drives which theme color is used).
    enum class GizmoAxis
    {
        X,
        Y,
        Z
    };

    //! Unit vector for a gizmo axis. Single source of truth shared by views and the manager
    //! (defined in GizmoViews.cpp) so the mapping is never duplicated.
    AZ::Vector3 GizmoAxisToVector(GizmoAxis axis);

    //! Resting themed colour for an axis (X/Y/Z), used by both the views and the manager's
    //! drag-feedback overlay so the constraint line / ghost arc match the handle colour.
    AZ::Color GizmoAxisColor(const GizmoTheme& theme, GizmoAxis axis);

    //! Shared base: resolves the themed color for an axis + hover state.
    class ThemedGizmoView : public AzToolsFramework::ManipulatorView
    {
    public:
        ThemedGizmoView(const GizmoTheme& theme, GizmoAxis axis)
            : ManipulatorView(/*screenSizeFixed=*/true)
            , m_theme(theme)
            , m_axis(axis)
        {
        }

    protected:
        AZ::Color ResolvedColor(bool mouseOver) const;
        AZ::Vector3 AxisVector() const;

        GizmoTheme m_theme;
        GizmoAxis m_axis;
    };

    //! Move handle: a themed axis line with a cone (arrow) head. Mirrors Blender's
    //! ED_GIZMO_ARROW_STYLE_NORMAL (line 0..length, cone length/radius from theme).
    class ArrowGizmoView final : public ThemedGizmoView
    {
    public:
        AZ_CLASS_ALLOCATOR(ArrowGizmoView, AZ::SystemAllocator)
        using ThemedGizmoView::ThemedGizmoView;

        void Draw(
            AzToolsFramework::ManipulatorManagerId managerId,
            const AzToolsFramework::ManipulatorManagerState& managerState,
            AzToolsFramework::ManipulatorId manipulatorId,
            const AzToolsFramework::ManipulatorState& manipulatorState,
            AzFramework::DebugDisplayRequests& debugDisplay,
            const AzFramework::CameraState& cameraState,
            const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction) override;
    };

    //! Scale handle: a themed axis line with a solid cube tip. Mirrors Blender's
    //! ED_GIZMO_ARROW_STYLE_BOX.
    class BoxGizmoView final : public ThemedGizmoView
    {
    public:
        AZ_CLASS_ALLOCATOR(BoxGizmoView, AZ::SystemAllocator)
        using ThemedGizmoView::ThemedGizmoView;

        void Draw(
            AzToolsFramework::ManipulatorManagerId managerId,
            const AzToolsFramework::ManipulatorManagerState& managerState,
            AzToolsFramework::ManipulatorId manipulatorId,
            const AzToolsFramework::ManipulatorState& manipulatorState,
            AzFramework::DebugDisplayRequests& debugDisplay,
            const AzFramework::CameraState& cameraState,
            const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction) override;
    };

    //! Planar handle: a small quad on the plane spanned by two axes (move/scale corners).
    class PlaneGizmoView final : public AzToolsFramework::ManipulatorView
    {
    public:
        AZ_CLASS_ALLOCATOR(PlaneGizmoView, AZ::SystemAllocator)
        PlaneGizmoView(const GizmoTheme& theme, GizmoAxis axis1, GizmoAxis axis2);

        void Draw(
            AzToolsFramework::ManipulatorManagerId managerId,
            const AzToolsFramework::ManipulatorManagerState& managerState,
            AzToolsFramework::ManipulatorId manipulatorId,
            const AzToolsFramework::ManipulatorState& manipulatorState,
            AzFramework::DebugDisplayRequests& debugDisplay,
            const AzFramework::CameraState& cameraState,
            const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction) override;

    private:
        GizmoTheme m_theme;
        GizmoAxis m_axis1;
        GizmoAxis m_axis2;
    };

    //! Rotate handle: a themed ring around one axis (Blender dial). Blender draws a single-colour
    //! ring hard-clipped by a camera-facing clip plane (the back half is not drawn); Unreal draws
    //! only the camera-facing quarter arc when idle. The shared screen-aligned trackball outline
    //! (Blender's view ring) is drawn once for the whole gizmo by DrawTrackballViewRing.
    class DialGizmoView final : public ThemedGizmoView
    {
    public:
        AZ_CLASS_ALLOCATOR(DialGizmoView, AZ::SystemAllocator)
        using ThemedGizmoView::ThemedGizmoView;

        void Draw(
            AzToolsFramework::ManipulatorManagerId managerId,
            const AzToolsFramework::ManipulatorManagerState& managerState,
            AzToolsFramework::ManipulatorId manipulatorId,
            const AzToolsFramework::ManipulatorState& manipulatorState,
            AzFramework::DebugDisplayRequests& debugDisplay,
            const AzFramework::CameraState& cameraState,
            const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction) override;
    };

    //! Draw the screen-aligned trackball outline (Blender's view ring) once, centred on the
    //! rotate gizmo pivot. Kept out of DialGizmoView so it is drawn exactly once per frame
    //! rather than being coupled to (and overdrawn by) the three per-axis rings.
    void DrawTrackballViewRing(
        AzFramework::DebugDisplayRequests& debugDisplay,
        const AzFramework::CameraState& cameraState,
        const AZ::Vector3& worldPivot,
        const GizmoTheme& theme);
} // namespace CrossEngineEditor
