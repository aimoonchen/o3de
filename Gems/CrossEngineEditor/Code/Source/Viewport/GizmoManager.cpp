/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/GizmoManager.h>
#include <Viewport/GizmoViews.h>

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
        case GizmoMode::Move:
            BuildMove(theme);
            break;
        case GizmoMode::Scale:
            BuildScale(theme);
            break;
        case GizmoMode::Rotate:
            BuildRotate(theme);
            break;
        }

        RefreshSpace();
    }

    void GizmoManager::DrawOverlay(
        AzFramework::DebugDisplayRequests& debugDisplay, const AzFramework::CameraState& cameraState) const
    {
        if (!m_entityId.IsValid())
        {
            return;
        }
        const GizmoTheme theme = MakeGizmoTheme(m_style);

        // Rotate-mode trackball view ring (Blender only; Unreal's screen ring is opt-in) is drawn
        // once around the pivot, decoupled from the three per-axis dials.
        if (m_mode == GizmoMode::Rotate && theme.m_rotateGeometry == RotationRingGeometry::BlenderHalfRingFade)
        {
            const AZ::Vector3 worldPivot = EntityWorldTransformNoScale().GetTranslation();
            DrawTrackballViewRing(debugDisplay, cameraState, worldPivot, theme);
        }

        // Interaction-time aids (constraint line, ghost arc, snap ticks, numeric readout).
        if (m_dragKind != DragKind::None)
        {
            DrawDragFeedback(debugDisplay, cameraState, theme);
        }
    }

    void GizmoManager::BuildMove(const GizmoTheme& theme)
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
            manipulator->SetView(AZStd::make_unique<DialGizmoView>(theme, axis));

            manipulator->InstallLeftMouseDownCallback(
                [entityId, axis, this](const AngularManipulator::Action&)
                {
                    AZ::TransformBus::EventResult(
                        m_dragStartOrientation, entityId, &AZ::TransformBus::Events::GetLocalRotationQuaternion);
                    m_dragStartSpace = EntityWorldTransformNoScale();
                    m_dragKind = DragKind::Angle;
                    m_dragAxis = axis;
                    m_dragAngle = 0.0f;
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

        AZStd::string FormatValue(const char* prefix, float value)
        {
            char buffer[64];
            azsnprintf(buffer, sizeof(buffer), "%s%.3f", prefix, value);
            return AZStd::string(buffer);
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

            if (theme.m_rotateGeometry == RotationRingGeometry::BlenderHalfRingFade)
            {
                // Blender dial ghost: filled pie sector 0..delta in the ring colour at half alpha,
                // plus a helpline at the start angle and at the current angle.
                AZ::Color fill = axisColor;
                fill.SetA(0.5f);
                FillSector(debugDisplay, pivot, u, v, radius, 0.0f, m_dragAngle, fill);

                debugDisplay.SetColor(axisColor.GetAsVector4());
                debugDisplay.SetLineWidth(theme.m_ringLineWidth);
                debugDisplay.DrawLine(pivot, pivot + u * radius);
                const AZ::Vector3 cur = u * cosf(m_dragAngle) + v * sinf(m_dragAngle);
                debugDisplay.DrawLine(pivot, pivot + cur * radius);
            }
            else
            {
                // Unreal DrawRotationArc dragging branch (DrawPartialRotationArc): fill the annulus
                // BAND (inner..outer radius) 0..delta at LargeOuterAlpha, faded remainder at
                // SmallOuterAlpha, then start (Yellow highlight) / current markers.
                const float rInner = theme.m_ringInnerRadius * scale;
                const float rOuter = theme.m_ringOuterRadius * scale;
                AZ::Color filled = axisColor;
                filled.SetA(0x7f / 255.0f);
                FillBand(debugDisplay, pivot, u, v, rInner, rOuter, 0.0f, m_dragAngle, filled);
                AZ::Color remainder = axisColor;
                remainder.SetA(0x0f / 255.0f);
                const float rest = (m_dragAngle >= 0.0f ? AZ::Constants::TwoPi : -AZ::Constants::TwoPi) - m_dragAngle;
                FillBand(debugDisplay, pivot, u, v, rInner, rOuter, m_dragAngle, rest, remainder);

                debugDisplay.SetColor(theme.m_highlightColor.GetAsVector4());
                debugDisplay.SetLineWidth(theme.m_ringLineWidth);
                debugDisplay.DrawLine(pivot + u * rInner, pivot + u * rOuter);
                const AZ::Vector3 cur = u * cosf(m_dragAngle) + v * sinf(m_dragAngle);
                debugDisplay.DrawLine(pivot + cur * rInner, pivot + cur * rOuter);
            }

            // Numeric readout (both editors show degrees). Anchor exactly like Unreal
            // CacheRotationHUDText: the first of the four in-plane quadrant directions that is on
            // screen, at ROTATION_TEXT_RADIUS. Rendered as a fixed-size screen-space label by the
            // viewport (QPainter), so it never scales or is occluded.
            const AZ::Vector3 labelPos = RotationLabelAnchor(pivot, u, v, scale, cameraState);
            debugDisplay.SetColor(theme.m_highlightColor.GetAsVector4());
            debugDisplay.DrawTextLabel(labelPos, 1.2f, FormatAngle(AZ::RadToDeg(m_dragAngle)).c_str(), true, 0, 0);
        }
        else if (m_dragKind == DragKind::Axis || m_dragKind == DragKind::Plane)
        {
            const bool isScale = (m_mode == GizmoMode::Scale);

            // Blender draws a full-length axis constraint line through the pivot while translating /
            // scaling (transform_constraints.cc drawConstraint). Unreal shows no such line - it only
            // surfaces the numeric delta - so gate the line to the Blender style for 1:1 parity.
            if (m_style == GizmoStyle::Blender)
            {
                // Blender's constraint line colour is NOT the gizmo axis colour. drawLine() blends a
                // base grey 50/50 with TH_AXIS_x then shades by -10 per channel
                // (transform_constraints.cc make_axis_color -> get_color_blend_shade_3ubv, fac 0.5,
                // offset -10). Dragging a gizmo handle is a CON_AXIS constraint, so the base grey is
                // the DRAWLIGHT value (220,220,220) (transform_constraints.cc:748), and the axes are
                // drawn via the CON_AXISn branch (:836-844). Line width is U.pixelsize*2.
                auto blenderConstraintColor = [](const AZ::Color& axisColor)
                {
                    // DRAWLIGHT base grey (220,220,220) blended 50/50 with the axis colour, shade -10.
                    auto channel = [](float axisC)
                    {
                        const float k_base = 220.0f / 255.0f;
                        const float blended = floorf((0.5f * k_base + 0.5f * axisC) * 255.0f) - 10.0f;
                        return AZ::GetClamp(blended, 0.0f, 255.0f) / 255.0f;
                    };
                    return AZ::Color(
                        channel(axisColor.GetR()), channel(axisColor.GetG()), channel(axisColor.GetB()),
                        1.0f);
                };
                auto drawConstraintLine = [&](GizmoAxis a)
                {
                    const AZ::Vector3 dir = space.TransformVector(GizmoAxisToVector(a)).GetNormalized();
                    const float half = 1000.0f; // effectively the whole viewport (Blender uses clip_end).
                    const AZ::Color col = blenderConstraintColor(GizmoAxisColor(theme, a));
                    debugDisplay.SetColor(col.GetAsVector4());
                    debugDisplay.SetLineWidth(2.0f); // U.pixelsize * 2.0f.
                    debugDisplay.DrawLine(pivot - dir * half, pivot + dir * half);
                };
                drawConstraintLine(m_dragAxis);
                if (m_dragKind == DragKind::Plane)
                {
                    drawConstraintLine(m_dragAxis2);
                }
            }

            // Readout: distance moved, or the resulting scale factor, near the pivot.
            debugDisplay.SetColor(theme.m_highlightColor.GetAsVector4());
            const AZStd::string text = isScale
                ? FormatValue("S: ", m_dragScale)
                : FormatValue("D: ", m_dragOffset.GetLength());
            debugDisplay.DrawTextLabel(pivot + AZ::Vector3(0.0f, 0.0f, 0.25f * scale), 1.2f, text.c_str(), true, 0, 0);
        }

        debugDisplay.DepthTestOn();
    }
} // namespace CrossEngineEditor
