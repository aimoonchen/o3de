/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/GizmoManager.h>
#include <Viewport/GizmoViews.h>
#include <Viewport/GenericDebugDisplay.h>

#include <AzCore/Component/ComponentBus.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/string/string.h>

#include <AzToolsFramework/Manipulators/AngularManipulator.h>
#include <AzToolsFramework/Manipulators/LinearManipulator.h>
#include <AzToolsFramework/Manipulators/ManipulatorView.h>
#include <AzToolsFramework/Manipulators/PlanarManipulator.h>
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>

#include <AzFramework/Viewport/CameraState.h>
#include <AzFramework/Viewport/ViewportScreen.h>
#include <AzFramework/Entity/EntityDebugDisplayBus.h>


namespace CrossEngineEditor
{
    using AzToolsFramework::AngularManipulator;
    using AzToolsFramework::LinearManipulator;
    using AzToolsFramework::ManipulatorView;
    using AzToolsFramework::ManipulatorViews;
    using AzToolsFramework::PlanarManipulator;
    using AzToolsFramework::ToolsApplicationRequestBus;
    using AzToolsFramework::ScopedUndoBatch;
    using AzToolsFramework::ToolsApplicationNotificationBus;

    namespace
    {
        //! Wrap a single view into the container the linear/planar manipulators expect.
        ManipulatorViews MakeViews(AZStd::shared_ptr<ManipulatorView> view)
        {
            ManipulatorViews views;
            views.emplace_back(AZStd::move(view));
            return views;
        }

        AZ::Vector3 GetEntityWorldTranslation(AZ::EntityId entityId)
        {
            AZ::Transform worldTm = AZ::Transform::CreateIdentity();
            AZ::TransformBus::EventResult(worldTm, entityId, &AZ::TransformBus::Events::GetWorldTM);
            return worldTm.GetTranslation();
        }

        //! Round a scalar to the nearest multiple of step (grid/angle/scale snap). step<=0 == off.
        //! Defined here (before the Build* drag callbacks) so those callbacks can see it.
        float SnapScalar(float value, float step)
        {
            if (step <= 0.0f)
            {
                return value;
            }
            return roundf(value / step) * step;
        }

        //! Snap each component of a vector to the grid (Blender/Unreal translation snap).
        AZ::Vector3 SnapVector(const AZ::Vector3& v, float step)
        {
            if (step <= 0.0f)
            {
                return v;
            }
            return AZ::Vector3(SnapScalar(v.GetX(), step), SnapScalar(v.GetY(), step), SnapScalar(v.GetZ(), step));
        }
    } // namespace

    GizmoManager::GizmoManager(AzToolsFramework::ManipulatorManagerId managerId, GizmoStyle style)
        : m_managerId(managerId)
        , m_style(style)
        , m_rotateDragState(AZStd::make_shared<GizmoDragState>())
    {
        ToolsApplicationNotificationBus::Handler::BusConnect();
        AzToolsFramework::EditorTransformChangeNotificationBus::Handler::BusConnect();
        GizmoControlRequestBus::Handler::BusConnect();

        // Build the gizmo for whatever is already selected when the handler is installed.
        AzToolsFramework::EntityIdList selected;
        ToolsApplicationRequestBus::BroadcastResult(selected, &ToolsApplicationRequestBus::Events::GetSelectedEntities);
        if (!selected.empty())
        {
            m_entityId = selected.back();
            RebuildManipulators();
        }
    }

    GizmoManager::~GizmoManager()
    {
        GizmoControlRequestBus::Handler::BusDisconnect();
        AzToolsFramework::EditorTransformChangeNotificationBus::Handler::BusDisconnect();
        ToolsApplicationNotificationBus::Handler::BusDisconnect();
        ClearManipulators();
    }

    void GizmoManager::SetMode(GizmoMode mode)
    {
        if (m_mode == mode)
        {
            return;
        }
        m_mode = mode;
        RebuildManipulators();
    }

    void GizmoManager::SetStyle(GizmoStyle style)
    {
        if (m_style == style)
        {
            return;
        }
        m_style = style;
        // Per-style default snap increments (only the increments, not the enabled flags): Blender
        // rotate 5 deg / grid follows the scene; Unreal rotate 10 deg / translate 10 cm.
        // Unreal's default rotation grid is CommonRotGridSizes[1] = 10 deg (BaseEditorPerProject
        // UserSettings.ini), NOT 22.5. 22.5 is only the LONG-TICK threshold and is applied
        // separately in the tick loop (fmod(deg,22.5)) - keeping the step at 10 restores the short
        // 0.25 ticks that a 22.5 step wrongly turned into dead code (N2).
        if (style == GizmoStyle::Unreal)
        {
            m_snap.m_angleStepDegrees = 10.0f;
            m_snap.m_gridSize = 0.1f;
        }
        else
        {
            m_snap.m_angleStepDegrees = 5.0f;
            m_snap.m_gridSize = 0.1f;
        }
        RebuildManipulators();
    }

    void GizmoManager::SetSpace(GizmoSpace space)
    {
        if (m_space == space)
        {
            return;
        }
        m_space = space;
        RebuildManipulators();
    }

    void GizmoManager::AfterEntitySelectionChanged(
        const AzToolsFramework::EntityIdList& /*newlySelectedEntities*/,
        const AzToolsFramework::EntityIdList& /*newlyDeselectedEntities*/)
    {
        // Single-entity gizmo: drive the most recently selected entity (multi-selection pivot
        // averaging is deferred).
        AzToolsFramework::EntityIdList selected;
        ToolsApplicationRequestBus::BroadcastResult(selected, &ToolsApplicationRequestBus::Events::GetSelectedEntities);

        const AZ::EntityId next = selected.empty() ? AZ::EntityId() : selected.back();
        if (next == m_entityId)
        {
            return;
        }
        m_entityId = next;
        RebuildManipulators();
    }

    void GizmoManager::OnEntityTransformChanged(const AzToolsFramework::EntityIdList& entityIds)
    {
        // While dragging, the drag callbacks own the space; only react to external changes
        // (Undo/Redo, Outliner drag, scripts) so the gizmo stays glued to the selected entity.
        if (m_undoBatch || !m_entityId.IsValid())
        {
            return;
        }
        for (const AZ::EntityId entityId : entityIds)
        {
            if (entityId == m_entityId)
            {
                RefreshSpace();
                break;
            }
        }
    }

    AZ::Transform GizmoManager::EntityWorldTransformNoScale() const
    {
        AZ::Transform worldTm = AZ::Transform::CreateIdentity();
        if (m_entityId.IsValid())
        {
            AZ::TransformBus::EventResult(worldTm, m_entityId, &AZ::TransformBus::Events::GetWorldTM);
        }
        // Manipulator space must not contain scale (see LinearManipulator::MakeShared note).
        worldTm.ExtractUniformScale();
        // World space: axis-aligned handles at the entity's location (Unreal COORD_World / Blender
        // Global). Local space: keep the entity's orientation so handles align to it. The drag maths
        // already works in whatever space the manipulator is given (m_dragStartSpace), so switching
        // here is sufficient - no per-callback changes needed.
        if (m_space == GizmoSpace::World)
        {
            return AZ::Transform::CreateTranslation(worldTm.GetTranslation());
        }
        return worldTm;
    }

    void GizmoManager::RefreshSpace()
    {
        const AZ::Transform space = EntityWorldTransformNoScale();
        for (auto& manipulator : m_linearManipulators)
        {
            manipulator->SetSpace(space);
            manipulator->SetLocalPosition(AZ::Vector3::CreateZero());
            manipulator->SetBoundsDirty();
        }
        for (auto& manipulator : m_planarManipulators)
        {
            manipulator->SetSpace(space);
            manipulator->SetLocalPosition(AZ::Vector3::CreateZero());
            manipulator->SetBoundsDirty();
        }
        for (auto& manipulator : m_angularManipulators)
        {
            manipulator->SetSpace(space);
            manipulator->SetBoundsDirty();
        }
    }

    void GizmoManager::FollowDragLocalPosition(const AZ::Vector3& localPosition)
    {
        // Slide every linear + planar handle to the same local position inside the fixed drag-start
        // space. The space itself stays anchored at drag start so LocalPositionOffset() remains
        // drift-free, while all handles (not just the grabbed one) track the moving entity as a set.
        for (auto& manipulator : m_linearManipulators)
        {
            manipulator->SetLocalPosition(localPosition);
            manipulator->SetBoundsDirty();
        }
        for (auto& manipulator : m_planarManipulators)
        {
            manipulator->SetLocalPosition(localPosition);
            manipulator->SetBoundsDirty();
        }
    }

    void GizmoManager::FollowDragOrientation()
    {
        // Local space: spin the whole dial set with the entity so the rings stay glued to it. World
        // space: leave the rings axis-aligned (Unreal COORD_World / Blender Global do not spin).
        if (m_space != GizmoSpace::Local)
        {
            return;
        }
        const AZ::Transform space = EntityWorldTransformNoScale();
        for (auto& manipulator : m_angularManipulators)
        {
            manipulator->SetSpace(space);
            manipulator->SetBoundsDirty();
        }
    }

    void GizmoManager::ClearManipulators()
    {
        for (auto& manipulator : m_linearManipulators)
        {
            manipulator->Unregister();
        }
        for (auto& manipulator : m_planarManipulators)
        {
            manipulator->Unregister();
        }
        for (auto& manipulator : m_angularManipulators)
        {
            manipulator->Unregister();
        }
        m_linearManipulators.clear();
        m_planarManipulators.clear();
        m_angularManipulators.clear();
    }

    void GizmoManager::RebuildManipulators()
    {
        ClearManipulators();

        if (!m_entityId.IsValid())
        {
            return;
        }

        const GizmoTheme theme = MakeGizmoTheme(m_style);

        switch (m_mode)
        {
        case GizmoMode::Select:
            // Pure selection: no manipulators (ClearManipulators above already left the set empty).
            // The selection outline is drawn by CrossEngineViewportSelection regardless of mode.
            return;
        case GizmoMode::Move:
            BuildMove(theme);
            break;
        case GizmoMode::Scale:
            BuildScale(theme);
            break;
        case GizmoMode::Rotate:
            BuildRotate(theme);
            break;
        case GizmoMode::Combined:
            BuildCombined(theme);
            break;
        }

        RefreshSpace();
    }

    void GizmoManager::BuildCombined(const GizmoTheme& theme)
    {
        // Blender's default transform gizmo shows move + rotate + scale together, radially separated
        // so the handle sets do not overlap (gizmo_line_range, transform_gizmo_3d.cc:1173-1203) and,
        // crucially, the move arrow draws ONLY its head - no stem - when rotate/scale are also shown
        // (draw_options=0, transform_gizmo_3d.cc:1539-1543). The pick bounds still span the whole
        // axis (see ArrowGizmoView/BoxGizmoView), so every handle stays easy to grab.
        // Absolute line ranges with all three visible:
        //   translate arrow: start = (1.0 - 0.125) + 0.215 = 1.09, end = 1.0 + 0.215 + 0.2 = 1.415
        //                    (head only, sitting OUTSIDE the rings).
        //   scale bar:       start = 0.2 (unchanged), end = 1.0 - 0.225 = 0.775  -> box just inside.
        //   rotate rings:    radius 1.0 (unchanged).
        GizmoTheme moveTheme = theme;
        moveTheme.m_axisLineStart = 1.09f;    // head base position (stem hidden).
        moveTheme.m_arrowAxisLength = 1.415f; // arrow head base / tip.
        moveTheme.m_drawArrowStem = false;    // Blender hides the stem in the combined gizmo.

        GizmoTheme scaleTheme = theme;
        scaleTheme.m_scaleLineStart = 0.2f;   // Blender keeps the scale bar start at 0.2 when combined.
        scaleTheme.m_scaleAxisLength = 0.775f; // end = 1.0 - 0.225.
        scaleTheme.m_scaleBoxCenter = 0.825f;  // box centre = end + half-extent (0.775 + 0.05).

        // Plane handles are omitted in the combined gizmo (Blender's combined layout is already dense;
        // its plane handles read poorly stacked with the rings) - BuildMoveAxes/BuildScaleAxes only.
        BuildRotate(theme);
        BuildMoveAxes(moveTheme);
        BuildScaleAxes(scaleTheme);
    }

    void GizmoManager::DrawOverlay(
        AzFramework::DebugDisplayRequests& debugDisplay, const AzFramework::CameraState& cameraState) const
    {
        if (!m_entityId.IsValid())
        {
            return;
        }
        const GizmoTheme theme = MakeGizmoTheme(m_style);

        // Rotate-mode screen-aligned ring (Blender thin white view ring / Unreal thick screen band)
        // is drawn once around the pivot, decoupled from the three per-axis dials. Also shown in the
        // combined gizmo, which includes the rotate rings.
        if ((m_mode == GizmoMode::Rotate || m_mode == GizmoMode::Combined) && theme.m_hasScreenRing)
        {
            const AZ::Vector3 worldPivot = EntityWorldTransformNoScale().GetTranslation();
            DrawTrackballViewRing(debugDisplay, cameraState, worldPivot, theme);
        }

        // Move/Scale center handle (Blender wire circle(s) / Unreal solid sphere/cube). Drawn as an
        // overlay so it reads exactly like both editors; the uniform-drag interaction is provided by
        // the axis handles and (for scale) the existing uniform-scale maths. Skipped in Combined mode
        // (crowded layout) - matching Blender, whose combined gizmo has no centre dot.
        if ((m_mode == GizmoMode::Move || m_mode == GizmoMode::Scale) &&
            theme.m_centerGeometry != CenterHandleGeometry::None)
        {
            const AZ::Vector3 worldPivot = EntityWorldTransformNoScale().GetTranslation();
            DrawCenterHandle(debugDisplay, cameraState, worldPivot, theme);
        }

        // Interaction-time aids (constraint line, ghost arc, snap ticks, numeric readout).
        if (m_dragKind != DragKind::None)
        {
            DrawDragFeedback(debugDisplay, cameraState, theme);
        }
    }

    void GizmoManager::BuildMove(const GizmoTheme& theme)
    {
        BuildMoveAxes(theme);
        BuildMovePlanes(theme);
    }

    void GizmoManager::BuildMoveAxes(const GizmoTheme& theme)
    {
        const AZ::Transform space = EntityWorldTransformNoScale();
        const AZ::EntityId entityId = m_entityId;

        // Translate the entity by the manipulator-space offset, following the same maths as the
        // engine's EditorTransformComponentSelection. The manipulator space is captured once at
        // drag start (m_dragStartSpace) and reused for the whole drag; refreshing it mid-drag on a
        // rotated entity would re-project the offset each frame and accumulate drift.
        auto applyTranslation = [entityId, this](const AZ::Vector3& localOffsetIn)
        {
            // Grid snap: quantise the offset in manipulator-local space (each handle axis snaps to
            // the grid step), mirroring Blender/Unreal translation snapping.
            const AZ::Vector3 localOffset =
                m_snap.m_gridSnapEnabled ? SnapVector(localOffsetIn, m_snap.m_gridSize) : localOffsetIn;
            const AZ::Vector3 worldOffset = m_dragStartSpace.TransformVector(localOffset);
            AZ::TransformBus::Event(
                entityId, &AZ::TransformBus::Events::SetWorldTranslation, m_dragStartTranslation + worldOffset);
            return worldOffset;
        };

        constexpr GizmoAxis axes[] = { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z };
        for (const GizmoAxis axis : axes)
        {
            auto manipulator = LinearManipulator::MakeShared(space);
            manipulator->AddEntityComponentIdPair(AZ::EntityComponentIdPair(entityId, AZ::InvalidComponentId));
            manipulator->SetAxis(GizmoAxisToVector(axis));
            manipulator->SetViews(MakeViews(AZStd::make_shared<ArrowGizmoView>(theme, axis)));

            manipulator->InstallLeftMouseDownCallback(
                [entityId, axis, this](const LinearManipulator::Action&)
                {
                    m_dragStartTranslation = GetEntityWorldTranslation(entityId);
                    m_dragStartSpace = EntityWorldTransformNoScale();
                    m_dragKind = DragKind::Axis;
                    m_dragAxis = axis;
                    m_dragAxisIsScale = false;
                    m_dragOffset = AZ::Vector3::CreateZero();
                    m_undoBatch = AZStd::make_unique<ScopedUndoBatch>("Move Entity");
                });
            manipulator->InstallMouseMoveCallback(
                [applyTranslation, this](const LinearManipulator::Action& action)
                {
                    const AZ::Vector3 localOffset = action.LocalPositionOffset();
                    m_dragOffset = applyTranslation(localOffset);
                    // Follow the cursor by moving the WHOLE handle set inside its fixed space. The
                    // space stays anchored at drag start so LocalPositionOffset() remains drift-free
                    // (no jitter), while every arrow/plane tracks the entity as one unit.
                    FollowDragLocalPosition(action.LocalPosition());
                });
            manipulator->InstallLeftMouseUpCallback(
                [entityId, this](const LinearManipulator::Action&)
                {
                    ScopedUndoBatch::MarkEntityDirty(entityId);
                    m_undoBatch.reset();
                    m_dragKind = DragKind::None;
                    RefreshSpace();
                });

            manipulator->Register(m_managerId);
            m_linearManipulators.emplace_back(AZStd::move(manipulator));
        }
    }

    void GizmoManager::BuildMovePlanes(const GizmoTheme& theme)
    {
        const AZ::Transform space = EntityWorldTransformNoScale();
        const AZ::EntityId entityId = m_entityId;

        auto applyTranslation = [entityId, this](const AZ::Vector3& localOffsetIn)
        {
            const AZ::Vector3 localOffset =
                m_snap.m_gridSnapEnabled ? SnapVector(localOffsetIn, m_snap.m_gridSize) : localOffsetIn;
            const AZ::Vector3 worldOffset = m_dragStartSpace.TransformVector(localOffset);
            AZ::TransformBus::Event(
                entityId, &AZ::TransformBus::Events::SetWorldTranslation, m_dragStartTranslation + worldOffset);
            return worldOffset;
        };

        // Planar handles on each pair of axes (XY / YZ / ZX).
        const AZStd::pair<GizmoAxis, GizmoAxis> planes[] = {
            { GizmoAxis::X, GizmoAxis::Y }, { GizmoAxis::Y, GizmoAxis::Z }, { GizmoAxis::Z, GizmoAxis::X }
        };
        for (const auto& [a1, a2] : planes)
        {
            auto manipulator = PlanarManipulator::MakeShared(space);
            manipulator->AddEntityComponentIdPair(AZ::EntityComponentIdPair(entityId, AZ::InvalidComponentId));
            manipulator->SetAxes(GizmoAxisToVector(a1), GizmoAxisToVector(a2));
            manipulator->SetViews(MakeViews(AZStd::make_shared<PlaneGizmoView>(theme, a1, a2)));

            manipulator->InstallLeftMouseDownCallback(
                [entityId, a1, a2, this](const PlanarManipulator::Action&)
                {
                    m_dragStartTranslation = GetEntityWorldTranslation(entityId);
                    m_dragStartSpace = EntityWorldTransformNoScale();
                    m_dragKind = DragKind::Plane;
                    m_dragAxis = a1;
                    m_dragAxis2 = a2;
                    m_dragOffset = AZ::Vector3::CreateZero();
                    m_undoBatch = AZStd::make_unique<ScopedUndoBatch>("Move Entity");
                });
            manipulator->InstallMouseMoveCallback(
                [applyTranslation, this](const PlanarManipulator::Action& action)
                {
                    const AZ::Vector3 localOffset = action.LocalPositionOffset();
                    m_dragOffset = applyTranslation(localOffset);
                    FollowDragLocalPosition(action.LocalPosition());
                });
            manipulator->InstallLeftMouseUpCallback(
                [entityId, this](const PlanarManipulator::Action&)
                {
                    ScopedUndoBatch::MarkEntityDirty(entityId);
                    m_undoBatch.reset();
                    m_dragKind = DragKind::None;
                    RefreshSpace();
                });

            manipulator->Register(m_managerId);
            m_planarManipulators.emplace_back(AZStd::move(manipulator));
        }
    }

    void GizmoManager::BuildScale(const GizmoTheme& theme)
    {
        BuildScaleAxes(theme);
        BuildScalePlanes(theme);
    }

    void GizmoManager::BuildScaleAxes(const GizmoTheme& theme)
    {
        const AZ::Transform space = EntityWorldTransformNoScale();
        const AZ::EntityId entityId = m_entityId;

        constexpr GizmoAxis axes[] = { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z };
        for (const GizmoAxis axis : axes)
        {
            auto manipulator = LinearManipulator::MakeShared(space);
            manipulator->AddEntityComponentIdPair(AZ::EntityComponentIdPair(entityId, AZ::InvalidComponentId));
            manipulator->SetAxis(GizmoAxisToVector(axis));
            manipulator->SetViews(MakeViews(AZStd::make_shared<BoxGizmoView>(theme, axis)));

            manipulator->InstallLeftMouseDownCallback(
                [entityId, axis, this](const LinearManipulator::Action&)
                {
                    AZ::TransformBus::EventResult(m_dragStartScale, entityId, &AZ::TransformBus::Events::GetLocalUniformScale);
                    m_dragStartSpace = EntityWorldTransformNoScale();
                    m_dragKind = DragKind::Axis;
                    m_dragAxis = axis;
                    m_dragAxisIsScale = true;
                    m_dragScale = m_dragStartScale;
                    m_undoBatch = AZStd::make_unique<ScopedUndoBatch>("Scale Entity");
                });
            manipulator->InstallMouseMoveCallback(
                [entityId, this](const LinearManipulator::Action& action)
                {
                    // Uniform scale driven by the summed axis offset (screen-consistent feel).
                    const float delta = action.LocalPositionOffset().Dot(action.m_fixed.m_axis);
                    float newScale = AZStd::max(0.01f, m_dragStartScale + delta);
                    if (m_snap.m_scaleSnapEnabled)
                    {
                        newScale = AZStd::max(0.01f, SnapScalar(newScale, m_snap.m_scaleStep));
                    }
                    m_dragScale = newScale;
                    AZ::TransformBus::Event(entityId, &AZ::TransformBus::Events::SetLocalUniformScale, newScale);
                });
            manipulator->InstallLeftMouseUpCallback(
                [entityId, this](const LinearManipulator::Action&)
                {
                    ScopedUndoBatch::MarkEntityDirty(entityId);
                    m_undoBatch.reset();
                    m_dragKind = DragKind::None;
                    RefreshSpace();
                });

            manipulator->Register(m_managerId);
            m_linearManipulators.emplace_back(AZStd::move(manipulator));
        }
    }

    void GizmoManager::BuildScalePlanes(const GizmoTheme& theme)
    {
        const AZ::Transform space = EntityWorldTransformNoScale();
        const AZ::EntityId entityId = m_entityId;

        // Scale plane handles (Blender: same diamond as move; Unreal: L-shaped 2-segment line).
        // Dragging a scale plane applies uniform scale from the summed in-plane offset.
        const AZStd::pair<GizmoAxis, GizmoAxis> planes[] = {
            { GizmoAxis::X, GizmoAxis::Y }, { GizmoAxis::Y, GizmoAxis::Z }, { GizmoAxis::Z, GizmoAxis::X }
        };
        for (const auto& [a1, a2] : planes)
        {
            auto manipulator = PlanarManipulator::MakeShared(space);
            manipulator->AddEntityComponentIdPair(AZ::EntityComponentIdPair(entityId, AZ::InvalidComponentId));
            manipulator->SetAxes(GizmoAxisToVector(a1), GizmoAxisToVector(a2));
            manipulator->SetViews(MakeViews(AZStd::make_shared<PlaneGizmoView>(theme, a1, a2, /*scaleMode=*/true)));

            manipulator->InstallLeftMouseDownCallback(
                [entityId, a1, a2, this](const PlanarManipulator::Action&)
                {
                    AZ::TransformBus::EventResult(m_dragStartScale, entityId, &AZ::TransformBus::Events::GetLocalUniformScale);
                    m_dragStartSpace = EntityWorldTransformNoScale();
                    m_dragKind = DragKind::Plane;
                    m_dragAxis = a1;
                    m_dragAxis2 = a2;
                    m_dragScale = m_dragStartScale;
                    m_undoBatch = AZStd::make_unique<ScopedUndoBatch>("Scale Entity");
                });
            manipulator->InstallMouseMoveCallback(
                [entityId, this](const PlanarManipulator::Action& action)
                {
                    const float delta = action.LocalPositionOffset().GetX() + action.LocalPositionOffset().GetY() +
                        action.LocalPositionOffset().GetZ();
                    float newScale = AZStd::max(0.01f, m_dragStartScale + delta);
                    if (m_snap.m_scaleSnapEnabled)
                    {
                        newScale = AZStd::max(0.01f, SnapScalar(newScale, m_snap.m_scaleStep));
                    }
                    m_dragScale = newScale;
                    AZ::TransformBus::Event(entityId, &AZ::TransformBus::Events::SetLocalUniformScale, newScale);
                });
            manipulator->InstallLeftMouseUpCallback(
                [entityId, this](const PlanarManipulator::Action&)
                {
                    ScopedUndoBatch::MarkEntityDirty(entityId);
                    m_undoBatch.reset();
                    m_dragKind = DragKind::None;
                    RefreshSpace();
                });

            manipulator->Register(m_managerId);
            m_planarManipulators.emplace_back(AZStd::move(manipulator));
        }
    }

    void GizmoManager::BuildRotate(const GizmoTheme& theme)
    {
        const AZ::Transform space = EntityWorldTransformNoScale();
        const AZ::EntityId entityId = m_entityId;

        constexpr GizmoAxis axes[] = { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z };
        for (const GizmoAxis axis : axes)
        {
            auto manipulator = AngularManipulator::MakeShared(space);
            manipulator->AddEntityComponentIdPair(AZ::EntityComponentIdPair(entityId, AZ::InvalidComponentId));
            manipulator->SetAxis(GizmoAxisToVector(axis));
            manipulator->SetView(AZStd::make_unique<DialGizmoView>(theme, axis, m_rotateDragState));

            manipulator->InstallLeftMouseDownCallback(
                [entityId, axis, this](const AngularManipulator::Action& action)
                {
                    AZ::TransformBus::EventResult(
                        m_dragStartOrientation, entityId, &AZ::TransformBus::Events::GetLocalRotationQuaternion);
                    m_dragStartSpace = EntityWorldTransformNoScale();
                    m_dragKind = DragKind::Angle;
                    m_dragAxis = axis;
                    m_dragAngle = 0.0f;
                    // Grab point in world space; the draw code projects it onto the ring plane to
                    // recover the grab angle theta0 that the ghost sector/helpline anchor to (N1).
                    m_dragStartHitWorld = action.m_start.m_worldHitPosition;
                    m_rotateDragState->m_rotating = true;
                    m_rotateDragState->m_axis = axis;
                    m_undoBatch = AZStd::make_unique<ScopedUndoBatch>("Rotate Entity");
                });
            manipulator->InstallMouseMoveCallback(
                [entityId, axis, this](const AngularManipulator::Action& action)
                {
                    const AZ::Quaternion rawDelta = action.m_current.m_delta;
                    // Signed rotation about the gizmo axis: the delta quaternion is about this axis,
                    // so its half-angle times the axis-alignment sign recovers the value to show.
                    const AZ::Vector3 axisVec = GizmoAxisToVector(axis);
                    const float sign = rawDelta.GetImaginary().Dot(axisVec) < 0.0f ? -1.0f : 1.0f;
                    float angle = sign * 2.0f * acosf(AZ::GetClamp(rawDelta.GetW(), -1.0f, 1.0f));

                    // Angle snap (Blender Ctrl increment / Unreal rotation grid): quantise the angle
                    // to the step and rebuild the delta about the gizmo axis from the snapped value.
                    AZ::Quaternion delta = rawDelta;
                    if (m_snap.m_angleSnapEnabled)
                    {
                        angle = AZ::DegToRad(SnapScalar(AZ::RadToDeg(angle), m_snap.m_angleStepDegrees));
                        delta = AZ::Quaternion::CreateFromAxisAngle(axisVec, angle);
                    }
                    m_dragAngle = angle;

                    // Unreal clamps the accumulated rotation to +/-360 deg (UnrealWidget.cpp); Blender
                    // supports multi-turn (ghost alpha increments). Clamp only for the Unreal style.
                    if (m_style == GizmoStyle::Unreal)
                    {
                        m_dragAngle = AZ::GetClamp(m_dragAngle, -AZ::Constants::TwoPi, AZ::Constants::TwoPi);
                    }

                    const AZ::Quaternion newRotation = (m_dragStartOrientation * delta).GetNormalized();
                    AZ::TransformBus::Event(entityId, &AZ::TransformBus::Events::SetLocalRotationQuaternion, newRotation);

                    // Keep the dial set glued to the entity while rotating in Local space (World
                    // space keeps axis-aligned rings, matching Unreal COORD_World / Blender Global).
                    FollowDragOrientation();
                });
            manipulator->InstallLeftMouseUpCallback(
                [entityId, this](const AngularManipulator::Action&)
                {
                    ScopedUndoBatch::MarkEntityDirty(entityId);
                    m_undoBatch.reset();
                    m_dragKind = DragKind::None;
                    m_rotateDragState->m_rotating = false;
                    RefreshSpace();
                });

            manipulator->Register(m_managerId);
            m_angularManipulators.emplace_back(AZStd::move(manipulator));
        }
    }

    namespace
    {
        //! Basis (u, v) spanning the plane perpendicular to a unit axis.
        void PlaneBasis(const AZ::Vector3& axis, AZ::Vector3& u, AZ::Vector3& v)
        {
            const AZ::Vector3 seed = (fabsf(axis.GetX()) < 0.9f) ? AZ::Vector3::CreateAxisX() : AZ::Vector3::CreateAxisY();
            u = axis.Cross(seed).GetNormalized();
            v = axis.Cross(u).GetNormalized();
        }

        //! Fill a pie sector (pivot + arc) as a triangle fan in the plane spanned by (u, v).
        void FillSector(
            AzFramework::DebugDisplayRequests& debugDisplay, const AZ::Vector3& pivot, const AZ::Vector3& u,
            const AZ::Vector3& v, float radius, float startAngle, float sweep, const AZ::Color& color)
        {
            constexpr int k_maxSteps = 64;
            const int steps = AZ::GetClamp(static_cast<int>(ceilf(fabsf(sweep) / (AZ::Constants::TwoPi / k_maxSteps)) + 0.5f), 1, k_maxSteps);
            const float step = sweep / static_cast<float>(steps);
            debugDisplay.SetColor(color.GetAsVector4());
            AZ::Vector3 prev = pivot + (u * cosf(startAngle) + v * sinf(startAngle)) * radius;
            for (int i = 1; i <= steps; ++i)
            {
                const float a = startAngle + step * static_cast<float>(i);
                const AZ::Vector3 cur = pivot + (u * cosf(a) + v * sinf(a)) * radius;
                debugDisplay.DrawTri(pivot, prev, cur);
                prev = cur;
            }
        }

        //! Fill an annulus band sector (Unreal DrawThickArc): a quad strip between rInner and rOuter
        //! spanning [startAngle, startAngle+sweep] in the plane spanned by (u, v).
        void FillBand(
            AzFramework::DebugDisplayRequests& debugDisplay, const AZ::Vector3& pivot, const AZ::Vector3& u,
            const AZ::Vector3& v, float rInner, float rOuter, float startAngle, float sweep, const AZ::Color& color)
        {
            constexpr int k_maxSteps = 96;
            const int steps = AZ::GetClamp(
                static_cast<int>(ceilf(fabsf(sweep) / (AZ::Constants::TwoPi / k_maxSteps)) + 0.5f), 1, k_maxSteps);
            const float step = sweep / static_cast<float>(steps);
            debugDisplay.SetColor(color.GetAsVector4());
            AZ::Vector3 prevIn = pivot + (u * cosf(startAngle) + v * sinf(startAngle)) * rInner;
            AZ::Vector3 prevOut = pivot + (u * cosf(startAngle) + v * sinf(startAngle)) * rOuter;
            for (int i = 1; i <= steps; ++i)
            {
                const float a = startAngle + step * static_cast<float>(i);
                const AZ::Vector3 dir = u * cosf(a) + v * sinf(a);
                const AZ::Vector3 curIn = pivot + dir * rInner;
                const AZ::Vector3 curOut = pivot + dir * rOuter;
                debugDisplay.DrawTri(prevIn, prevOut, curOut);
                debugDisplay.DrawTri(prevIn, curOut, curIn);
                prevIn = curIn;
                prevOut = curOut;
            }
        }

        //! Rotation start/stop marker: an isosceles triangle at the arc angle, apex at the outer
        //! radius (Unreal DrawStartStopMarker). Hollow (3 edge lines) for the start marker, filled
        //! for the current marker. Height = 0.8 * band width, half-width = height * tan30.
        void DrawRotationMarker(
            AzFramework::DebugDisplayRequests& debugDisplay, const AZ::Vector3& pivot, const AZ::Vector3& u,
            const AZ::Vector3& v, float angle, float rInner, float rOuter, const AZ::Color& color, bool filled)
        {
            const AZ::Vector3 dir = u * cosf(angle) + v * sinf(angle);
            const AZ::Vector3 tangent = -u * sinf(angle) + v * cosf(angle);
            const float bandWidth = rOuter - rInner;
            const float height = 0.8f * bandWidth;
            const float halfWidth = height * 0.57735026f; // tan(30 deg).
            const AZ::Vector3 apex = pivot + dir * rOuter;
            const AZ::Vector3 baseCenter = pivot + dir * (rOuter - height);
            const AZ::Vector3 c0 = baseCenter + tangent * halfWidth;
            const AZ::Vector3 c1 = baseCenter - tangent * halfWidth;
            if (filled)
            {
                AZ::Color f = color;
                f.SetA(0x7f / 255.0f);
                debugDisplay.SetColor(f.GetAsVector4());
                debugDisplay.DrawTri(apex, c0, c1);
            }
            else
            {
                AZ::Color e = color;
                e.SetA(1.0f);
                debugDisplay.SetColor(e.GetAsVector4());
                debugDisplay.SetLineWidth(1.0f);
                debugDisplay.DrawLine(apex, c0);
                debugDisplay.DrawLine(c0, c1);
                debugDisplay.DrawLine(c1, apex);
            }
        }

        //! Angle readout format. Unreal CacheRotationHUDText uses "%3.2f" (2 decimals); match it.
        AZStd::string FormatAngle(float degrees)
        {
            char buffer[64];
            azsnprintf(buffer, sizeof(buffer), "%3.2f", degrees);
            return AZStd::string(buffer);
        }

        //! True when a world point projects inside the viewport rectangle and is in front of the
        //! camera. Mirrors Unreal's CacheRotationHUDText visibility gate (WorldToScreen + IsWithin
        //! the view rect) so we can pick a label anchor that is actually on screen.
        bool WorldPointOnScreen(const AZ::Vector3& worldPos, const AzFramework::CameraState& cameraState)
        {
            if ((worldPos - cameraState.m_position).Dot(cameraState.m_forward) <= 0.0f)
            {
                return false;
            }
            const AzFramework::ScreenPoint sp = AzFramework::WorldToScreen(worldPos, cameraState);
            const float w = static_cast<float>(cameraState.m_viewportSize.m_width);
            const float h = static_cast<float>(cameraState.m_viewportSize.m_height);
            return sp.m_x >= 0.0f && sp.m_x <= w && sp.m_y >= 0.0f && sp.m_y <= h;
        }

        //! Choose the rotation label anchor exactly like Unreal's CacheRotationHUDText: try the four
        //! in-plane directions {u, v, -u, -v} at TextDistance and keep the first that is on screen.
        //! ROTATION_TEXT_RADIUS 75 over AXIS_LENGTH 35 -> ~2.14 in our handle-relative units.
        AZ::Vector3 RotationLabelAnchor(
            const AZ::Vector3& pivot, const AZ::Vector3& u, const AZ::Vector3& v, float scale,
            const AzFramework::CameraState& cameraState)
        {
            const float textDistance = (75.0f / 35.0f) * scale;
            const AZ::Vector3 dirs[4] = { u, v, -u, -v };
            for (const AZ::Vector3& dir : dirs)
            {
                const AZ::Vector3 candidate = pivot + dir * textDistance;
                if (WorldPointOnScreen(candidate, cameraState))
                {
                    return candidate;
                }
            }
            return pivot + u * textDistance; // fallback: none on screen (unlikely), use axis 0.
        }
    } // namespace

    void GizmoManager::DrawDragFeedback(
        AzFramework::DebugDisplayRequests& debugDisplay, const AzFramework::CameraState& cameraState,
        const GizmoTheme& theme) const
    {
        const AZ::Transform space = m_dragStartSpace;
        const AZ::Vector3 pivot = space.GetTranslation();
        const float scale = AzToolsFramework::CalculateScreenToWorldMultiplier(pivot, cameraState);

        // Draw all aids in front of the scene so they read like both editors' overlays.
        debugDisplay.DepthTestOff();

        if (m_dragKind == DragKind::Angle)
        {
            const AZ::Vector3 axis = space.TransformVector(GizmoAxisToVector(m_dragAxis)).GetNormalized();
            AZ::Vector3 u, v;
            PlaneBasis(axis, u, v);
            const float radius = theme.m_ringRadius * scale;
            const AZ::Color axisColor = GizmoAxisColor(theme, m_dragAxis);

            // Blender area-header status text: "Rotation: X.XXdeg" (transform_mode_rotate.cc).
            if (m_style == GizmoStyle::Blender)
            {
                char header[96];
                azsnprintf(header, sizeof(header), "Rotation: %.2f deg", static_cast<double>(AZ::RadToDeg(m_dragAngle)));
                static_cast<GenericDebugDisplay&>(debugDisplay).SetHeaderText(header);
            }

            if (theme.m_rotateGeometry == RotationRingGeometry::BlenderHalfRingFade)
            {
                // Blender anchors the ghost arc/helplines to the GRAB angle theta0, not to the ring
                // 0deg (dial3d_gizmo.cc dial_ghostarc_get_angles: angle_ofs = projected mouse-down
                // direction). Recover theta0 by projecting the grab hit onto the (u,v) ring plane.
                float theta0 = 0.0f;
                {
                    AZ::Vector3 rel = m_dragStartHitWorld - pivot;
                    rel -= axis * rel.Dot(axis); // project onto ring plane.
                    if (rel.GetLengthSq() > 1e-8f)
                    {
                        theta0 = atan2f(rel.Dot(v), rel.Dot(u));
                    }
                }

                // Blender dial ghost: filled pie sector [theta0, theta0+delta] in a GREY
                // {0.8,0.8,0.8,0.2} - NOT the axis colour (dial3d_gizmo.cc).
                FillSector(debugDisplay, pivot, u, v, radius, theta0, m_dragAngle, theme.m_ghostArcColor);

                // Modal drag disables the clip plane, so the dragged dial is drawn as a FULL ring
                // (dial3d_gizmo.cc use_clip_plane = !is_modal). The idle dial for this axis is
                // suppressed (GizmoDragState) so this replaces it cleanly.
                debugDisplay.SetColor(axisColor.GetAsVector4());
                debugDisplay.SetLineWidth(theme.m_ringLineWidth);
                {
                    const int segs = AZStd::max(48, theme.m_ringSegments);
                    AZ::Vector3 prevRing = pivot + u * radius;
                    for (int i = 1; i <= segs; ++i)
                    {
                        const float t = (static_cast<float>(i) / static_cast<float>(segs)) * AZ::Constants::TwoPi;
                        const AZ::Vector3 pt = pivot + (u * cosf(t) + v * sinf(t)) * radius;
                        debugDisplay.DrawLine(prevRing, pt);
                        prevRing = pt;
                    }
                }

                // Helplines: 1px at the grab angle theta0, 3px at the current angle theta0+delta
                // (dial_ghostarc_draw_with_helplines).
                debugDisplay.SetColor(axisColor.GetAsVector4());
                const AZ::Vector3 startDir = u * cosf(theta0) + v * sinf(theta0);
                debugDisplay.SetLineWidth(theme.m_helplineStartWidth);
                debugDisplay.DrawLine(pivot, pivot + startDir * radius);
                debugDisplay.SetLineWidth(theme.m_helplineCurrentWidth);
                const AZ::Vector3 cur = u * cosf(theta0 + m_dragAngle) + v * sinf(theta0 + m_dragAngle);
                debugDisplay.DrawLine(pivot, pivot + cur * radius);

                // Blender also draws the CON_AXIS constraint line through the pivot while rotating
                // (transform_constraints.cc drawConstraint fires for TFM_ROTATION too).
                DrawBlenderConstraintLine(debugDisplay, theme, space, m_dragAxis, pivot);

                // Snap ticks (dial_ghostarc_draw_incremental_angle): a ring of white 1px ticks at
                // radius DIAL_WIDTH*1.1 .. *1.21, count = round(2pi/increment), only while snapping.
                if (m_snap.m_angleSnapEnabled && m_snap.m_angleStepDegrees > 0.0f)
                {
                    const float inc = AZ::DegToRad(m_snap.m_angleStepDegrees);
                    const int total = AZStd::max(1, static_cast<int>(roundf(AZ::Constants::TwoPi / inc)));
                    const float rIn = radius * 1.1f;
                    const float rOut = radius * 1.21f;
                    debugDisplay.SetColor(AZ::Color(1.0f, 1.0f, 1.0f, 1.0f).GetAsVector4());
                    debugDisplay.SetLineWidth(1.0f);
                    for (int i = 0; i < total; ++i)
                    {
                        const float a = inc * static_cast<float>(i);
                        const AZ::Vector3 dir = u * cosf(a) + v * sinf(a);
                        debugDisplay.DrawLine(pivot + dir * rIn, pivot + dir * rOut);
                    }
                }
            }
            else
            {
                // Unreal DrawRotationArc dragging branch (DrawPartialRotationArc): the dragged axis
                // band turns YELLOW (RGB) at 0x7f, faded remainder at 0x0f, then triangle markers.
                const float rInner = theme.m_ringInnerRadius * scale;
                const float rOuter = theme.m_ringOuterRadius * scale;
                AZ::Color filled = theme.m_highlightColor; // yellow.
                filled.SetA(0x7f / 255.0f);
                FillBand(debugDisplay, pivot, u, v, rInner, rOuter, 0.0f, m_dragAngle, filled);
                AZ::Color remainder = theme.m_highlightColor;
                remainder.SetA(0x0f / 255.0f);
                const float rest = (m_dragAngle >= 0.0f ? AZ::Constants::TwoPi : -AZ::Constants::TwoPi) - m_dragAngle;
                FillBand(debugDisplay, pivot, u, v, rInner, rOuter, m_dragAngle, rest, remainder);

                // Start marker = hollow yellow triangle (A=0), current = filled yellow triangle
                // (A=0x7f); apex at the outer radius, height 0.8*band, half-width height*tan30
                // (UnrealWidgetRender.cpp DrawStartStopMarker).
                DrawRotationMarker(debugDisplay, pivot, u, v, 0.0f, rInner, rOuter, theme.m_highlightColor, false);
                DrawRotationMarker(debugDisplay, pivot, u, v, m_dragAngle, rInner, rOuter, theme.m_highlightColor, true);

                // Snap ticks (DrawSnapMarker): radial yellow lines from the outer radius inward by
                // band*PercentSize (0.75 for 22.5deg multiples, else 0.25), skipping 90deg multiples;
                // only while RotGridEnabled. Band = rOuter - rInner.
                if (m_snap.m_angleSnapEnabled && m_snap.m_angleStepDegrees > 0.0f)
                {
                    const float band = rOuter - rInner;
                    debugDisplay.SetColor(theme.m_highlightColor.GetAsVector4());
                    debugDisplay.SetLineWidth(1.0f);
                    for (float deg = 0.0f; deg < 360.0f; deg += m_snap.m_angleStepDegrees)
                    {
                        if (fmodf(deg, 90.0f) == 0.0f)
                        {
                            continue; // 90deg multiples use the wide axis markers instead.
                        }
                        const float pct = (fmodf(deg, 22.5f) == 0.0f) ? 0.75f : 0.25f;
                        const float a = AZ::DegToRad(deg);
                        const AZ::Vector3 dir = u * cosf(a) + v * sinf(a);
                        debugDisplay.DrawLine(pivot + dir * rOuter, pivot + dir * (rOuter - band * pct));
                    }
                    // 0/90/180/270 wide axis markers (PercentSize 0.25 from the outer radius).
                    // UE colours these with the DRAGGED-AXIS colour (RGB rotation of InColor), not
                    // the highlight yellow (UnrealWidgetRender.cpp DrawSnapMarker, N8).
                    debugDisplay.SetColor(axisColor.GetAsVector4());
                    for (int q = 0; q < 4; ++q)
                    {
                        const float a = AZ::DegToRad(90.0f * static_cast<float>(q));
                        const AZ::Vector3 dir = u * cosf(a) + v * sinf(a);
                        debugDisplay.DrawLine(pivot + dir * rOuter, pivot + dir * (rOuter - band * 0.25f));
                    }
                }
            }

            // Numeric readout. Unreal shows a white-on-black-block HUD only for rotation
            // (CacheRotationHUDText); anchor at the first of the four in-plane quadrant directions
            // that is on screen, at ROTATION_TEXT_RADIUS. Blender shows the angle in the area header
            // (handled by the Qt overlay), not floating in the viewport.
            if (m_style == GizmoStyle::Unreal)
            {
                const AZ::Vector3 labelPos = RotationLabelAnchor(pivot, u, v, scale, cameraState);
                debugDisplay.SetColor(AZ::Color(1.0f, 1.0f, 1.0f, 1.0f).GetAsVector4()); // white text.
                // Our editor only ever routes gizmo drawing through GenericDebugDisplay, so this is
                // the concrete type; request the Unreal black HUD block behind the next label.
                static_cast<GenericDebugDisplay&>(debugDisplay).SetNextLabelBackgroundBlock(true);
                debugDisplay.DrawTextLabel(labelPos, 1.2f, FormatAngle(AZ::RadToDeg(m_dragAngle)).c_str(), true, 0, 0);
            }
        }
        else if (m_dragKind == DragKind::Axis || m_dragKind == DragKind::Plane)
        {
            // Blender draws a full-length CON_AXIS constraint line through the pivot while
            // translating / scaling (transform_constraints.cc drawConstraint). Unreal shows NO line
            // and NO viewport HUD for translate/scale (Render_Translate has no dragging branch) - its
            // hover/drag difference is only the yellow tint - so all feedback here is Blender-only.
            if (m_style == GizmoStyle::Blender)
            {
                DrawBlenderConstraintLine(debugDisplay, theme, space, m_dragAxis, pivot);
                if (m_dragKind == DragKind::Plane)
                {
                    DrawBlenderConstraintLine(debugDisplay, theme, space, m_dragAxis2, pivot);
                }

                // Combined gizmo only: at idle the move/scale axis is stem-less (draw_options=0), but
                // Blender's modal draw switches the dragged axis to a FULL-LENGTH stem from the pivot
                // (gizmo_3d_setup_draw_modal: length=end, STEM|ORIGIN, transform_gizmo_3d.cc). Draw
                // that stem here so the dragged direction visibly grows out of the origin (N3).
                if (m_mode == GizmoMode::Combined && m_dragKind == DragKind::Axis)
                {
                    const AZ::Color axisCol = GizmoAxisColor(theme, m_dragAxis);
                    const AZ::Vector3 axisDir = space.TransformVector(GizmoAxisToVector(m_dragAxis)).GetNormalized();
                    // Move end = 1.415, scale end = 0.775 in handle-relative units (BuildCombined).
                    const float stemEnd = (m_dragAxisIsScale ? 0.775f : 1.415f) * scale;
                    debugDisplay.SetColor(axisCol.GetAsVector4());
                    debugDisplay.SetLineWidth(theme.m_axisLineWidth);
                    debugDisplay.DrawLine(pivot, pivot + axisDir * stemEnd);
                }

                // Modal drag draws a filled ORIGIN dot at the pivot in the axis colour
                // (ED_GIZMO_ARROW_DRAW_FLAG_ORIGIN, point size 10*pixelsize, 8-seg fill).
                const AZ::Color axisColor = GizmoAxisColor(theme, m_dragAxis);
                debugDisplay.SetColor(axisColor.GetAsVector4());
                AZ::Vector3 du, dv;
                PlaneBasis(cameraState.m_forward.GetNormalizedSafe(), du, dv);
                const float dotR = 0.08f * scale; // ~10px screen-constant.
                AZ::Vector3 prevDot = pivot + du * dotR;
                for (int i = 1; i <= 8; ++i)
                {
                    const float t = (static_cast<float>(i) / 8.0f) * AZ::Constants::TwoPi;
                    const AZ::Vector3 cur = pivot + (du * cosf(t) + dv * sinf(t)) * dotR;
                    debugDisplay.DrawTri(pivot, prevDot, cur);
                    prevDot = cur;
                }
                // Blender's numeric delta lives in the area header (Qt overlay), not the viewport.
                char header[96];
                if (m_mode == GizmoMode::Scale)
                {
                    azsnprintf(header, sizeof(header), "Scale: %.4f", static_cast<double>(m_dragScale));
                }
                else
                {
                    const AZ::Vector3 d = m_dragOffset;
                    azsnprintf(
                        header, sizeof(header), "D: %.4f (%.4f, %.4f, %.4f)", static_cast<double>(d.GetLength()),
                        static_cast<double>(d.GetX()), static_cast<double>(d.GetY()), static_cast<double>(d.GetZ()));
                }
                static_cast<GenericDebugDisplay&>(debugDisplay).SetHeaderText(header);
            }
        }

        debugDisplay.DepthTestOn();
    }

    void GizmoManager::DrawBlenderConstraintLine(
        AzFramework::DebugDisplayRequests& debugDisplay, const GizmoTheme& theme, const AZ::Transform& space,
        GizmoAxis axis, const AZ::Vector3& pivot) const
    {
        // Blender's constraint line colour is NOT the gizmo axis colour. drawLine() blends a base
        // grey 50/50 with TH_AXIS_x then shades by -10 per channel (transform_constraints.cc
        // make_axis_color -> get_color_blend_shade_3ubv, fac 0.5, offset -10). Dragging a gizmo
        // handle is a CON_AXIS constraint, so the base grey is DRAWLIGHT (220,220,220). Width 2px.
        auto channel = [](float axisC)
        {
            const float k_base = 220.0f / 255.0f;
            const float blended = floorf((0.5f * k_base + 0.5f * axisC) * 255.0f) - 10.0f;
            return AZ::GetClamp(blended, 0.0f, 255.0f) / 255.0f;
        };
        const AZ::Color axisColor = GizmoAxisColor(theme, axis);
        const AZ::Color col(channel(axisColor.GetR()), channel(axisColor.GetG()), channel(axisColor.GetB()), 1.0f);
        const AZ::Vector3 dir = space.TransformVector(GizmoAxisToVector(axis)).GetNormalized();
        const float half = 1000.0f; // effectively the whole viewport (Blender uses clip_end).
        debugDisplay.SetColor(col.GetAsVector4());
        debugDisplay.SetLineWidth(2.0f); // U.pixelsize * 2.0f.
        debugDisplay.DrawLine(pivot - dir * half, pivot + dir * half);
    }
} // namespace CrossEngineEditor
