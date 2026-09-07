/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <MaterialEditor/CeeMaterialPreviewPanel.h>

#include <QImage>
#include <QPainter>

namespace CrossEngineEditor
{
    CeeMaterialPreviewPanel::CeeMaterialPreviewPanel(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumSize(128, 128);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void CeeMaterialPreviewPanel::SetImageData(uint32_t w, uint32_t h, const AZStd::vector<AZ::u8>& pixels)
    {
        if (w == 0 || h == 0 || pixels.size() < w * h * 4)
        {
            return;
        }

        m_width = w;
        m_height = h;
        m_pixels = pixels;

        // QImage wraps the pixel buffer without deep copy (implicit sharing).
        m_image = QImage(m_pixels.data(), m_width, m_height, m_width * 4, QImage::Format_RGBA8888);
        m_dirty = false;
        update();
    }

    void CeeMaterialPreviewPanel::paintEvent([[maybe_unused]] QPaintEvent* event)
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);

        if (!m_image.isNull() && m_width > 0 && m_height > 0)
        {
            // Draw scaled to fill widget, centered, with aspect ratio preserved.
            const QRect source(0, 0, static_cast<int>(m_width), static_cast<int>(m_height));
            const QSize scaled = source.size().scaled(rect().size(), Qt::KeepAspectRatio);
            const QRect target(
                (rect().width() - scaled.width()) / 2,
                (rect().height() - scaled.height()) / 2,
                scaled.width(), scaled.height());
            painter.drawImage(target, m_image, source);
        }
        else
        {
            painter.setPen(Qt::gray);
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No Preview"));
        }
    }
} // namespace CrossEngineEditor
