/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Editor world grid overlay (plan §5.3).
//!
//! A grid is an editor overlay - not a manipulator and not part of any engine scene - so
//! it is owned by the shell and drawn every frame straight into a DebugDisplayRequests
//! target. The look (line spacing, axis highlight, fade) follows the modern Blender/O3DE
//! convention: minor lines, brighter major lines every N cells, and coloured world axes.

#include <AzCore/Math/Color.h>

namespace AzFramework
{
    class DebugDisplayRequests;
    struct CameraState;
} // namespace AzFramework

namespace CrossEngineEditor
{
    class EditorGrid
    {
    public:
        //! Emit the grid geometry for this frame into the given debug display, sized/faded
        //! around the camera so it always appears as an infinite ground plane.
        void Draw(AzFramework::DebugDisplayRequests& debugDisplay, const AzFramework::CameraState& cameraState) const;

        struct Settings
        {
            float m_cellSize = 1.0f;         //!< World units between minor grid lines.
            int m_majorLineEvery = 10;       //!< Every Nth line is drawn brighter.
            int m_halfLineCount = 50;        //!< Grid extends this many cells each side of centre.
            AZ::Color m_minorColor = AZ::Color(0.30f, 0.30f, 0.30f, 1.0f);
            AZ::Color m_majorColor = AZ::Color(0.42f, 0.42f, 0.42f, 1.0f);
            AZ::Color m_axisXColor = AZ::Color(0.79f, 0.25f, 0.30f, 1.0f);
            AZ::Color m_axisYColor = AZ::Color(0.42f, 0.60f, 0.20f, 1.0f);
        };

        Settings& GetSettings() { return m_settings; }

    private:
        Settings m_settings;
    };
} // namespace CrossEngineEditor
