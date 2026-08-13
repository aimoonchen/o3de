/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/EditorViewportCameraController.h>

#include <Profiling/CrossEngineProfiler.h>

#include <AzCore/std/smart_ptr/make_shared.h>

#include <AzFramework/Input/Devices/Keyboard/InputDeviceKeyboard.h>
#include <AzFramework/Input/Devices/Mouse/InputDeviceMouse.h>

AZ_PUSH_DISABLE_WARNING(4251 4800, "-Wunknown-warning-option")
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
AZ_POP_DISABLE_WARNING

namespace CrossEngineEditor
{
    namespace
    {
        //! Sensible, engine-neutral defaults matching the original editor's feel.
        constexpr float k_rotateSpeed = 0.005f;
        constexpr float k_panSpeed = 1.0f;
        constexpr float k_translateSpeed = 5.0f;
        constexpr float k_boostMultiplier = 3.0f;
        constexpr float k_scrollSpeed = 0.03f;

        AzFramework::ScreenPoint ScreenPointFromQt(const QPoint& point)
        {
            return AzFramework::ScreenPoint(point.x(), point.y());
        }

        //! Map a Qt key to the corresponding AzFramework keyboard input channel used by the
        //! translate camera behavior. Returns nullptr for keys the camera does not care about.
        const AzFramework::InputChannelId* TranslateKeyChannel(int qtKey)
        {
            switch (qtKey)
            {
            case Qt::Key_W:
                return &AzFramework::InputDeviceKeyboard::Key::AlphanumericW;
            case Qt::Key_S:
                return &AzFramework::InputDeviceKeyboard::Key::AlphanumericS;
            case Qt::Key_A:
                return &AzFramework::InputDeviceKeyboard::Key::AlphanumericA;
            case Qt::Key_D:
                return &AzFramework::InputDeviceKeyboard::Key::AlphanumericD;
            case Qt::Key_Q:
                return &AzFramework::InputDeviceKeyboard::Key::AlphanumericQ;
            case Qt::Key_E:
                return &AzFramework::InputDeviceKeyboard::Key::AlphanumericE;
            case Qt::Key_Shift:
                return &AzFramework::InputDeviceKeyboard::Key::ModifierShiftL;
            default:
                return nullptr;
            }
        }
    } // namespace

    EditorViewportCameraController::EditorViewportCameraController()
    {
        // Free-look on right mouse; pan on middle mouse; WASDQE translate; wheel dolly;
        // Alt turns the right-mouse look into an orbit around the pivot (original editor feel).
        AzFramework::TranslateCameraInputChannelIds translateIds;
        translateIds.m_forwardChannelId = AzFramework::InputDeviceKeyboard::Key::AlphanumericW;
        translateIds.m_backwardChannelId = AzFramework::InputDeviceKeyboard::Key::AlphanumericS;
        translateIds.m_leftChannelId = AzFramework::InputDeviceKeyboard::Key::AlphanumericA;
        translateIds.m_rightChannelId = AzFramework::InputDeviceKeyboard::Key::AlphanumericD;
        translateIds.m_downChannelId = AzFramework::InputDeviceKeyboard::Key::AlphanumericQ;
        translateIds.m_upChannelId = AzFramework::InputDeviceKeyboard::Key::AlphanumericE;
        translateIds.m_boostChannelId = AzFramework::InputDeviceKeyboard::Key::ModifierShiftL;

        m_rotateCamera = AZStd::make_shared<AzFramework::RotateCameraInput>(AzFramework::InputDeviceMouse::Button::Right);
        m_rotateCamera->m_rotateSpeedFn = [] { return k_rotateSpeed; };

        m_panCamera = AZStd::make_shared<AzFramework::PanCameraInput>(
            AzFramework::InputDeviceMouse::Button::Middle, AzFramework::LookPan, AzFramework::TranslatePivotLook);
        m_panCamera->m_panSpeedFn = [] { return k_panSpeed; };

        m_translateCamera = AZStd::make_shared<AzFramework::TranslateCameraInput>(
            translateIds, AzFramework::LookTranslation, AzFramework::TranslatePivotLook);
        m_translateCamera->m_translateSpeedFn = [] { return k_translateSpeed; };
        m_translateCamera->m_boostMultiplierFn = [] { return k_boostMultiplier; };

        m_scrollDollyCamera = AZStd::make_shared<AzFramework::LookScrollTranslationCameraInput>();
        m_scrollDollyCamera->m_scrollSpeedFn = [] { return k_scrollSpeed; };

        m_orbitCamera = AZStd::make_shared<AzFramework::OrbitCameraInput>(AzFramework::InputDeviceKeyboard::Key::ModifierAltL);
        {
            auto orbitRotate = AZStd::make_shared<AzFramework::RotateCameraInput>(AzFramework::InputDeviceMouse::Button::Left);
            orbitRotate->m_rotateSpeedFn = [] { return k_rotateSpeed; };
            auto orbitDolly = AZStd::make_shared<AzFramework::OrbitScrollDollyCameraInput>();
            auto orbitPan = AZStd::make_shared<AzFramework::PanCameraInput>(
                AzFramework::InputDeviceMouse::Button::Middle, AzFramework::LookPan, AzFramework::TranslateOffsetOrbit);
            orbitPan->m_panSpeedFn = [] { return k_panSpeed; };
            m_orbitCamera->m_orbitCameras.AddCamera(orbitRotate);
            m_orbitCamera->m_orbitCameras.AddCamera(orbitDolly);
            m_orbitCamera->m_orbitCameras.AddCamera(orbitPan);
        }

        m_cameraSystem.m_cameras.AddCamera(m_rotateCamera);
        m_cameraSystem.m_cameras.AddCamera(m_panCamera);
        m_cameraSystem.m_cameras.AddCamera(m_translateCamera);
        m_cameraSystem.m_cameras.AddCamera(m_scrollDollyCamera);
        m_cameraSystem.m_cameras.AddCamera(m_orbitCamera);

        // Default editor view: pulled back and looking towards the origin (Z-up, Y-forward).
        AzFramework::UpdateCameraFromTransform(
            m_targetCamera,
            AZ::Transform::CreateLookAt(
                AZ::Vector3(5.0f, -5.0f, 5.0f), AZ::Vector3::CreateZero(), AZ::Transform::Axis::YPositive));
        m_camera = m_targetCamera;
    }

    AzFramework::CameraState EditorViewportCameraController::StepCamera(
        const AzFramework::ScreenSize& viewportSize, float deltaSeconds)
    {
        m_targetCamera = m_cameraSystem.StepCamera(m_targetCamera, deltaSeconds);
        m_camera = m_targetCamera;

        AzFramework::CameraState cameraState = AzFramework::CreateDefaultCamera(m_camera.Transform(), viewportSize);
        return cameraState;
    }

    bool EditorViewportCameraController::DispatchDiscrete(
        const AzFramework::InputChannelId& channelId, bool pressed, const AzFramework::ScreenSize& /*viewportSize*/)
    {
        const AzFramework::DiscreteInputEvent discreteEvent{
            channelId, pressed ? AzFramework::InputChannel::State::Began : AzFramework::InputChannel::State::Ended
        };
        return m_cameraSystem.HandleEvents(AzFramework::InputState{ discreteEvent, m_modifierStates });
    }

    bool EditorViewportCameraController::HandleMousePress(
        const QMouseEvent& event, const AzFramework::ScreenSize& viewportSize)
    {
        const AzFramework::InputChannelId* channel = nullptr;
        switch (event.button())
        {
        case Qt::LeftButton:
            channel = &AzFramework::InputDeviceMouse::Button::Left;
            break;
        case Qt::RightButton:
            channel = &AzFramework::InputDeviceMouse::Button::Right;
            break;
        case Qt::MiddleButton:
            channel = &AzFramework::InputDeviceMouse::Button::Middle;
            break;
        default:
            return false;
        }
        return DispatchDiscrete(*channel, true, viewportSize);
    }

    bool EditorViewportCameraController::HandleMouseRelease(
        const QMouseEvent& event, const AzFramework::ScreenSize& viewportSize)
    {
        const AzFramework::InputChannelId* channel = nullptr;
        switch (event.button())
        {
        case Qt::LeftButton:
            channel = &AzFramework::InputDeviceMouse::Button::Left;
            break;
        case Qt::RightButton:
            channel = &AzFramework::InputDeviceMouse::Button::Right;
            break;
        case Qt::MiddleButton:
            channel = &AzFramework::InputDeviceMouse::Button::Middle;
            break;
        default:
            return false;
        }
        return DispatchDiscrete(*channel, false, viewportSize);
    }

    bool EditorViewportCameraController::HandleMouseMove(
        const QMouseEvent& event, const AzFramework::ScreenSize& /*viewportSize*/)
    {
        CEE_PROFILE_FUNCTION();
        const AzFramework::CursorEvent cursorEvent{ ScreenPointFromQt(event.pos()) };
        return m_cameraSystem.HandleEvents(AzFramework::InputState{ cursorEvent, m_modifierStates });
    }

    bool EditorViewportCameraController::HandleWheel(
        const QWheelEvent& event, const AzFramework::ScreenSize& /*viewportSize*/)
    {
        const AzFramework::ScrollEvent scrollEvent{ aznumeric_cast<float>(event.angleDelta().y()) };
        return m_cameraSystem.HandleEvents(AzFramework::InputState{ scrollEvent, m_modifierStates });
    }

    bool EditorViewportCameraController::HandleKey(
        const QKeyEvent& event, bool pressed, const AzFramework::ScreenSize& viewportSize)
    {
        // Track the Alt modifier so the orbit camera can activate off it.
        if (event.key() == Qt::Key_Alt)
        {
            m_modifierStates.SetActive(AzFramework::ModifierKeyMask::AltL, pressed);
            return DispatchDiscrete(AzFramework::InputDeviceKeyboard::Key::ModifierAltL, pressed, viewportSize);
        }

        const AzFramework::InputChannelId* channel = TranslateKeyChannel(event.key());
        if (channel == nullptr)
        {
            return false;
        }
        return DispatchDiscrete(*channel, pressed, viewportSize);
    }
} // namespace CrossEngineEditor
