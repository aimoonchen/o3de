/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

// Vendor: AtomToolsFramework Inspector header
#include "MaterialEditor/Vendor/AtomToolsFramework/Inspector/InspectorGroupHeaderWidget.h"

#include <AzCore/std/functional.h>
#include <AzCore/std/string/string.h>

#include <QRect>

namespace CrossEngineEditor
{
    // Group header with optional checkbox for PROPERTY_HINT_GROUP_ENABLE toggle.
    // When m_toggleProperty is non-empty, a checkbox is drawn left of the
    // expand/collapse icon. Clicking the checkbox toggles the associated bool
    // property without expanding/collapsing the group.
    class CeeInspectorGroupHeaderWidget final : public AtomToolsFramework::InspectorGroupHeaderWidget
    {
        Q_OBJECT
    public:
        explicit CeeInspectorGroupHeaderWidget(
            const AZStd::string& togglePropertyId,
            AZStd::function<void(bool)> onToggle,
            bool initialState = false,
            QWidget* parent = nullptr);

        void SetToggleState(bool state);

        [[nodiscard]] bool GetToggleState() const
        {
            return m_toggleState;
        }

    protected:
        void paintEvent(QPaintEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;

    private:
        AZStd::string m_togglePropertyId;
        AZStd::function<void(bool)> m_onToggle;
        bool m_toggleState = false;
        QRect m_checkboxRect; // hit-test area
    };
} // namespace CrossEngineEditor
