/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Self-drawn transform gizmo manager (plan: route B, §9). Owns the low-level O3DE
//! manipulators (Linear / Planar / Angular) but dresses them with our themed views
//! (ArrowGizmoView / BoxGizmoView / PlaneGizmoView / DialGizmoView) so the look is 100%
//! ours (Blender / Unreal) while picking / hover / drag stay in the reused framework.
//!
//! The manager listens for selection changes and rebuilds the manipulator set for the
//! active entity in the current mode (Move / Rotate / Scale). Drag callbacks write the new
//! transform back through TransformBus wrapped in a single undo batch, matching the engine's
//! own EditorTransformComponentSelection semantics.
//!
//! This replaces the engine-native transform gizmos: the CrossEngineViewportSelection handler
//! installs this instead of EditorTransformComponentSelection, so only our gizmos are drawn.

#if !defined(Q_MOC_RUN)
#include <Viewport/GizmoTheme.h>
#include <Viewport/GizmoViews.h>

#include <AzCore/Component/EntityId.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/EBus/EBus.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>

#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Entity/EditorEntityTransformBus.h>
#include <AzToolsFramework/Manipulators/ManipulatorBus.h>
#endif

namespace AzFramework
{
    struct CameraState;
    class DebugDisplayRequests;
} // namespace AzFramework

namespace AzToolsFramework
{
    class LinearManipulator;
    class PlanarManipulator;
    class AngularManipulator;
} // namespace AzToolsFramework

namespace CrossEngineEditor
{
    //! Which transform operation the gizmo currently drives.
    enum class GizmoMode
    {
        //! Pure selection: no transform handles are built, only the selection outline is shown.
        //! Clicking still selects in every mode (Unity/Godot semantics); Select mode's value is an
        //! unobstructed viewport with no gizmo to accidentally grab.
        Select,
        Move,
        Rotate,
        Scale,
        //! Blender-style combined gizmo: move + rotate + scale handles shown together (the default
        //! transform gizmo group). All three manipulator sets are built and drawn at once.
        Combined
    };

    //! Gizmo orientation space. World = axis-aligned handles (Unreal COORD_World / Blender Global);
    //! Local = handles aligned to the entity's own orientation (COORD_Local / Blender Local).
    enum class GizmoSpace
    {
        World,
        Local
    };

    //! Snap settings applied while dragging (Blender grid/angle snap, Unreal grid/rotation snap).
    //! Increments are in the BackendAPI convention (metres / degrees). Disabled by default so the
    //! feel matches a fresh editor; the toolbar toggles them.
    struct GizmoSnapSettings
    {
        bool m_gridSnapEnabled = false;
        float m_gridSize = 0.25f; //!< metres per grid step for translation.
        bool m_angleSnapEnabled = false;
        float m_angleStepDegrees = 15.0f; //!< degrees per rotation step (Blender default 5, UE 10; 15 is neutral).
        bool m_scaleSnapEnabled = false;
        float m_scaleStep = 0.1f;
    };

    //! Decoupled control surface so the main-window toolbar/menu can drive the gizmo without
    //! holding a pointer into the viewport selection handler (which the framework owns).
    class GizmoControlRequests : public AZ::EBusTraits
    {
    public:
        static const AZ::EBusHandlerPolicy HandlerPolicy = AZ::EBusHandlerPolicy::Single;
        static const AZ::EBusAddressPolicy AddressPolicy = AZ::EBusAddressPolicy::Single;

        virtual void SetGizmoMode(GizmoMode mode) = 0;
        virtual void SetGizmoStyle(GizmoStyle style) = 0;
        virtual void SetGizmoSpace(GizmoSpace space) = 0;
        //! Flip only the snap enabled flags, preserving the style-configured step increments
        //! (a fresh GizmoSnapSettings would reset them to neutral defaults).
        virtual void SetSnapEnabled(bool enabled) = 0;
    };
    using GizmoControlRequestBus = AZ::EBus<GizmoControlRequests>;

    //! Builds and drives the themed transform gizmos for the active selection.
    class GizmoManager
        : private AzToolsFramework::ToolsApplicationNotificationBus::Handler
        , private AzToolsFramework::EditorTransformChangeNotificationBus::Handler
        , private GizmoControlRequestBus::Handler
    {
    public:
        GizmoManager(AzToolsFramework::ManipulatorManagerId managerId, GizmoStyle style);
        ~GizmoManager();

        // GizmoControlRequestBus ...
        void SetGizmoMode(GizmoMode mode) override
        {
            SetMode(mode);
        }
        void SetGizmoStyle(GizmoStyle style) override
        {
            SetStyle(style);
        }
        void SetGizmoSpace(GizmoSpace space) override
        {
            SetSpace(space);
        }
        void SetSnapEnabled(bool enabled) override
        {
            // Keep the current step increments (set by SetStyle); only toggle the enables.
            m_snap.m_gridSnapEnabled = enabled;
            m_snap.m_angleSnapEnabled = enabled;
            m_snap.m_scaleSnapEnabled = enabled;
        }

        //! Switch transform mode (Move / Rotate / Scale) and rebuild the gizmo.
        void SetMode(GizmoMode mode);
        GizmoMode GetMode() const
        {
            return m_mode;
        }

        //! Switch the visual theme (Blender / Unreal) and rebuild the gizmo.
        void SetStyle(GizmoStyle style);
        GizmoStyle GetStyle() const
        {
            return m_style;
        }

        //! Switch the gizmo orientation space (World / Local) and rebuild the gizmo.
        void SetSpace(GizmoSpace space);
        GizmoSpace GetSpace() const
        {
            return m_space;
        }

        //! Draw gizmo-level overlays that are drawn once per frame rather than per manipulator
        //! (currently the rotate-mode trackball view ring). Called from the viewport selection
        //! handler's DisplayViewportSelection.
        void DrawOverlay(
            AzFramework::DebugDisplayRequests& debugDisplay, const AzFramework::CameraState& cameraState) const;

    private:
        // ToolsApplicationNotificationBus ...
        void AfterEntitySelectionChanged(
            const AzToolsFramework::EntityIdList& newlySelectedEntities,
            const AzToolsFramework::EntityIdList& newlyDeselectedEntities) override;

        // EditorTransformChangeNotificationBus ...
        //! Keep the gizmo glued to the entity when its transform changes without a reselection
        //! (Undo/Redo, Outliner drag, scripts). Skipped while dragging (drag refreshes itself).
        void OnEntityTransformChanged(const AzToolsFramework::EntityIdList& entityIds) override;

        //! Tear down the current manipulators and, if an entity is selected, build the set
        //! for the active mode positioned at the entity's world transform.
        void RebuildManipulators();
        void ClearManipulators();

        //! Refresh the manipulator space to the entity's current world transform (called after
        //! a drag writes it back so the gizmo follows the entity).
        void RefreshSpace();

        //! During a linear/planar drag, slide the WHOLE handle set (all axis arrows + planes) to
        //! the dragged handle's local position so the gizmo visually follows the entity as one unit
        //! (mirrors EditorTransformComponentSelection driving a single shared manipulator group).
        void FollowDragLocalPosition(const AZ::Vector3& localPosition);

        //! During an angular drag in Local space, re-orient the WHOLE dial set to the entity's new
        //! orientation so the rings track the object (World space keeps axis-aligned rings, matching
        //! Unreal COORD_World / Blender Global where the dials do not spin).
        void FollowDragOrientation();

        AZ::Transform EntityWorldTransformNoScale() const;

        void BuildMove(const GizmoTheme& theme);
        void BuildScale(const GizmoTheme& theme);
        void BuildRotate(const GizmoTheme& theme);
        //! Combined move+rotate+scale gizmo (Blender default), radially offset to avoid overlap.
        void BuildCombined(const GizmoTheme& theme);
        //! Axis-only handle builders (no planar handles) - shared by the single-mode Build* and the
        //! combined gizmo, which omits the plane handles to keep the crowded layout readable.
        void BuildMoveAxes(const GizmoTheme& theme);
        void BuildScaleAxes(const GizmoTheme& theme);
        //! Planar (two-axis) handle builders - the plane half of BuildMove / BuildScale.
        void BuildMovePlanes(const GizmoTheme& theme);
        void BuildScalePlanes(const GizmoTheme& theme);

        //! Feedback drawn while a drag is active: the constraint/reference line, rotation ghost
        //! arc + helplines, snap ticks and the numeric readout. Mirrors Blender's drawConstraint /
        //! dial ghostarc and Unreal's DrawRotationArc + DrawHUD. Split by style in DrawOverlay.
        void DrawDragFeedback(
            AzFramework::DebugDisplayRequests& debugDisplay, const AzFramework::CameraState& cameraState,
            const GizmoTheme& theme) const;

        //! Draw Blender's CON_AXIS constraint reference line through the pivot along one axis
        //! (grey-blended axis colour, 2px). Shared by translate/scale and rotate drag feedback.
        void DrawBlenderConstraintLine(
            AzFramework::DebugDisplayRequests& debugDisplay, const GizmoTheme& theme, const AZ::Transform& space,
            GizmoAxis axis, const AZ::Vector3& pivot) const;

        //! State captured continuously during a drag so DrawOverlay can render the aids. m_dragKind
        //! is None when no drag is in flight.
        enum class DragKind
        {
            None,
            Axis,   //!< linear move/scale along m_dragAxis
            Plane,  //!< planar move on the m_dragAxis / m_dragAxis2 pair
            Angle   //!< angular rotate about m_dragAxis
        };

        AzToolsFramework::ManipulatorManagerId m_managerId;
        GizmoMode m_mode = GizmoMode::Select;
        GizmoStyle m_style = GizmoStyle::Blender;
        GizmoSpace m_space = GizmoSpace::World;
        GizmoSnapSettings m_snap;
        AZ::EntityId m_entityId;

        //! Shared with the dial views so a per-axis dial can suppress its idle arc while its axis is
        //! being rotated (the manager draws the full drag arc/ghost instead).
        AZStd::shared_ptr<GizmoDragState> m_rotateDragState;

        AZStd::vector<AZStd::shared_ptr<AzToolsFramework::LinearManipulator>> m_linearManipulators;
        AZStd::vector<AZStd::shared_ptr<AzToolsFramework::PlanarManipulator>> m_planarManipulators;
        AZStd::vector<AZStd::shared_ptr<AzToolsFramework::AngularManipulator>> m_angularManipulators;

        //! Transform state captured at the start of a drag so each move applies a delta from it.
        AZ::Vector3 m_dragStartTranslation = AZ::Vector3::CreateZero();
        AZ::Quaternion m_dragStartOrientation = AZ::Quaternion::CreateIdentity();
        float m_dragStartScale = 1.0f;
        //! Manipulator space captured at drag start; used unchanged for the whole drag so an
        //! entity with rotation does not accumulate drift as its transform is written back.
        AZ::Transform m_dragStartSpace = AZ::Transform::CreateIdentity();
        //! Undo batch open for the duration of a single drag.
        AZStd::unique_ptr<AzToolsFramework::ScopedUndoBatch> m_undoBatch;

        // --- Live drag-feedback state (see DragKind) ---
        DragKind m_dragKind = DragKind::None;
        GizmoAxis m_dragAxis = GizmoAxis::X;
        GizmoAxis m_dragAxis2 = GizmoAxis::Y;
        AZ::Vector3 m_dragOffset = AZ::Vector3::CreateZero(); //!< world translation offset (move).
        float m_dragAngle = 0.0f; //!< signed rotation delta in radians (rotate).
        float m_dragScale = 1.0f; //!< resulting uniform scale (scale).
        //! For a DragKind::Axis drag in the combined gizmo, whether the grabbed handle is a scale
        //! bar (true) or a move arrow (false). Selects the modal stem length in the feedback (N3).
        bool m_dragAxisIsScale = false;
        //! World-space hit point at rotation grab. Blender anchors the ghost sector and the start
        //! helpline to the grab angle (dial3d_gizmo.cc angle_ofs), not to the ring's 0 degrees (N1).
        AZ::Vector3 m_dragStartHitWorld = AZ::Vector3::CreateZero();
    };
} // namespace CrossEngineEditor
