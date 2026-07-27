/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Viewport/EditorGrid.h>

#include <AzFramework/Entity/EntityDebugDisplayBus.h>
#include <AzFramework/Viewport/CameraState.h>

#include <AzCore/Math/Vector3.h>

#include <cmath>

namespace CrossEngineEditor
{
    void EditorGrid::Draw(
        AzFramework::DebugDisplayRequests& debugDisplay, const AzFramework::CameraState& cameraState) const
    {
        // Centre the grid under the camera's horizontal position (projected onto the z=0
        // ground plane) and snap that centre to the cell size. Snapping to whole cells is
        // what makes the grid appear static as the camera slides - the lines always sit on
        // the same world-space multiples of the cell size.
        const int centreCellX = static_cast<int>(floorf(cameraState.m_position.GetX() / m_settings.m_cellSize));
        const int centreCellY = static_cast<int>(floorf(cameraState.m_position.GetY() / m_settings.m_cellSize));

        const float extent = m_settings.m_halfLineCount * m_settings.m_cellSize;

        for (int i = -m_settings.m_halfLineCount; i <= m_settings.m_halfLineCount; ++i)
        {
            // Absolute world cell index for this line, so major/axis classification is stable
            // in world space regardless of where the camera is.
            const int cellX = centreCellX + i;
            const int cellY = centreCellY + i;
            const float worldX = cellX * m_settings.m_cellSize;
            const float worldY = cellY * m_settings.m_cellSize;
            const float centreWorldX = centreCellX * m_settings.m_cellSize;
            const float centreWorldY = centreCellY * m_settings.m_cellSize;

            // Line parallel to X (constant Y = worldY). Highlight the world X axis (Y == 0).
            {
                const AZ::Color color =
                    (cellY == 0) ? m_settings.m_axisXColor
                                 : ((cellY % m_settings.m_majorLineEvery) == 0 ? m_settings.m_majorColor : m_settings.m_minorColor);
                debugDisplay.SetColor(color);
                debugDisplay.DrawLine(
                    AZ::Vector3(centreWorldX - extent, worldY, 0.0f), AZ::Vector3(centreWorldX + extent, worldY, 0.0f));
            }

            // Line parallel to Y (constant X = worldX). Highlight the world Y axis (X == 0).
            {
                const AZ::Color color =
                    (cellX == 0) ? m_settings.m_axisYColor
                                 : ((cellX % m_settings.m_majorLineEvery) == 0 ? m_settings.m_majorColor : m_settings.m_minorColor);
                debugDisplay.SetColor(color);
                debugDisplay.DrawLine(
                    AZ::Vector3(worldX, centreWorldY - extent, 0.0f), AZ::Vector3(worldX, centreWorldY + extent, 0.0f));
            }
        }
    }
} // namespace CrossEngineEditor
