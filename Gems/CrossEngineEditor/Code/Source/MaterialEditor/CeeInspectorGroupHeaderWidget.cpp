/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <MaterialEditor/CeeInspectorGroupHeaderWidget.h>

#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace CrossEngineEditor
{
    CeeInspectorGroupHeaderWidget::CeeInspectorGroupHeaderWidget(
        const AZStd::string& togglePropertyId,
        AZStd::function<void(bool)> onToggle,
        bool initialState,
        QWidget* parent)
        : AtomToolsFramework::InspectorGroupHeaderWidget(parent)
        , m_togglePropertyId(togglePropertyId)
        , m_onToggle(AZStd::move(onToggle))
        , m_toggleState(initialState)
    {
        // Widen left margin to make room for the checkbox.
        // The base class draws the expand icon at x=5; we need space for a 16x16 checkbox
        // plus a gap before the existing icon. Total left offset: 5 (checkbox) + 4 (gap) = 9
        // extra pixels, pushed into contentsMargins in paintEvent.
    }

    void CeeInspectorGroupHeaderWidget::SetToggleState(bool state)
    {
        if (m_toggleState != state)
        {
            m_toggleState = state;
            update();
        }
    }

    void CeeInspectorGroupHeaderWidget::paintEvent(QPaintEvent* event)
    {
        // Call base class first to draw expand/collapse icon and text.
        AtomToolsFramework::InspectorGroupHeaderWidget::paintEvent(event);

        if (m_togglePropertyId.empty())
        {
            return;
        }

        QPainter painter(this);
        QStyle* style = QApplication::style();

        // Draw checkbox at left edge, vertically centered and offset to avoid
        // overlapping the base expand/collapse icon.
        const int checkboxSize = 16;
        const int leftMargin = 20;
        const int topMargin = (rect().height() - checkboxSize) / 2;
        m_checkboxRect = QRect(leftMargin, topMargin, checkboxSize, checkboxSize);

        QStyleOptionViewItem option;
        option.rect = m_checkboxRect;
        option.state = QStyle::State_Enabled;
        if (m_toggleState)
        {
            option.state |= QStyle::State_On;
        }
        else
        {
            option.state |= QStyle::State_Off;
        }

        style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &option, &painter, this);
    }

    void CeeInspectorGroupHeaderWidget::mousePressEvent(QMouseEvent* event)
    {
        if (!m_togglePropertyId.empty() && m_checkboxRect.contains(event->pos()))
        {
            m_toggleState = !m_toggleState;
            if (m_onToggle)
            {
                m_onToggle(m_toggleState);
            }
            update();
            return; // Consume the event; do not toggle expand/collapse.
        }

        // Click outside checkbox: normal expand/collapse behavior.
        AtomToolsFramework::InspectorGroupHeaderWidget::mousePressEvent(event);
    }
} // namespace CrossEngineEditor

#include <MaterialEditor/moc_CeeInspectorGroupHeaderWidget.cpp>
