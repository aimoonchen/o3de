/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Engine-agnostic viewport camera navigation (Plan §B2).
//!
//! Wraps AzFramework's CameraSystem and the standard set of camera inputs (rotate / pan /
//! translate / dolly / orbit) so the cross-engine viewport gets the same orbit/pan/dolly/fly
//! navigation semantics as the original O3DE editor, with zero Atom dependency. Qt input is
//! translated directly into AzFramework::InputState events; StepCamera is driven each frame
//! to produce the CameraState the viewport renders and picks with.

#include <AzFramework/Viewport/CameraInput.h>
#include <AzFramework/Viewport/CameraState.h>
#include <AzFramework/Viewport/ScreenGeometry.h>

#include <AzCore/Math/Aabb.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;

namespace CrossEngineEditor
{
    class EditorViewportCameraController
    {
    public:
        EditorViewportCameraController();

        //! Advance the camera by one frame and return the resulting camera state for the
        //! given viewport size. Should be called once per rendered frame.
        AzFramework::CameraState StepCamera(const AzFramework::ScreenSize& viewportSize, float deltaSeconds);

        //! Translate Qt input into camera navigation events. Return true if the camera system
        //! consumed the event (so the viewport can skip forwarding it to manipulators).
        bool HandleMousePress(const QMouseEvent& event, const AzFramework::ScreenSize& viewportSize);
        bool HandleMouseRelease(const QMouseEvent& event, const AzFramework::ScreenSize& viewportSize);
        //! \p pixelRatio converts the event's logical cursor position to the physical pixels the
        //! camera math runs in; without it the sensitivity would scale with the OS scale factor.
        //! Callers must pass it explicitly (no default): a forgotten argument would silently
        //! reintroduce scale-dependent navigation sensitivity.
        bool HandleMouseMove(
            const QMouseEvent& event, const AzFramework::ScreenSize& viewportSize, float pixelRatio);
        bool HandleWheel(const QWheelEvent& event, const AzFramework::ScreenSize& viewportSize);
        bool HandleKey(const QKeyEvent& event, bool pressed, const AzFramework::ScreenSize& viewportSize);

        //! Is the camera system currently consuming input (mid-navigation)?
        bool HandlingEvents() const { return m_cameraSystem.HandlingEvents(); }

        //! Frame the given world bounds: keep the current view direction, move the camera back
        //! just far enough that the bounds' bounding sphere fits the vertical FOV (with margin),
        //! and re-anchor the pivot at the bounds' center so later orbiting rotates around the
        //! framed area ("focus on selection", editor_polish.md P0-3). cameraState supplies the
        //! direction + fov to frame with (the viewport's current state).
        void FrameBounds(const AZ::Aabb& bounds, const AzFramework::CameraState& cameraState);

    private:
        bool DispatchDiscrete(
            const AzFramework::InputChannelId& channelId, bool pressed, const AzFramework::ScreenSize& viewportSize);

        AzFramework::CameraSystem m_cameraSystem;
        AzFramework::Camera m_camera;       //!< Current (smoothed) camera.
        AzFramework::Camera m_targetCamera; //!< Target the smoothed camera converges to.
        AzFramework::ModifierKeyStates m_modifierStates;

        // Camera behaviors kept alive for the lifetime of the controller.
        AZStd::shared_ptr<AzFramework::RotateCameraInput> m_rotateCamera;
        AZStd::shared_ptr<AzFramework::PanCameraInput> m_panCamera;
        AZStd::shared_ptr<AzFramework::TranslateCameraInput> m_translateCamera;
        AZStd::shared_ptr<AzFramework::LookScrollTranslationCameraInput> m_scrollDollyCamera;
        AZStd::shared_ptr<AzFramework::OrbitCameraInput> m_orbitCamera;
    };
} // namespace CrossEngineEditor
