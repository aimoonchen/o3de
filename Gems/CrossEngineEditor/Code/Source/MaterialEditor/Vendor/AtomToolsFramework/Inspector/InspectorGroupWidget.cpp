/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

// Vendor copy of AtomToolsFramework InspectorGroupWidget for CrossEngineEditor.
// Original: Gems/Atom/Tools/AtomToolsFramework/Code/Source/Inspector/InspectorGroupWidget.cpp
// Upstream commit: 38261d0800

#include <MaterialEditor/Vendor/AtomToolsFramework/Inspector/InspectorGroupWidget.h>

namespace AtomToolsFramework
{
    InspectorGroupWidget::InspectorGroupWidget(QWidget* parent)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }

    void InspectorGroupWidget::Refresh()
    {
    }

    void InspectorGroupWidget::Rebuild()
    {
    }
} // namespace AtomToolsFramework

#include <MaterialEditor/Vendor/AtomToolsFramework/Inspector/moc_InspectorGroupWidget.cpp>
