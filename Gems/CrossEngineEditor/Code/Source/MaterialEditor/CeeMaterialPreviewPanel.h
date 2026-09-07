/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Material preview panel - simple QImage display widget.
//!
//! Holds a QImage backed by a byte buffer. The throttle gate
//! calls SetImageData() when the backend returns Updated from
//! AcquirePreviewImage.

#include <AzCore/base.h>
#include <AzCore/std/containers/vector.h>

#include <QWidget>

class QImage;

namespace CrossEngineEditor
{
    class CeeMaterialPreviewPanel final : public QWidget
    {
        Q_OBJECT
    public:
        explicit CeeMaterialPreviewPanel(QWidget* parent = nullptr);
        ~CeeMaterialPreviewPanel() override = default;

        //! Update the displayed image. pixels must be RGBA8, tightly packed (w*h*4).
        void SetImageData(uint32_t w, uint32_t h, const AZStd::vector<AZ::u8>& pixels);

        //! True when the panel has been modified and needs a refresh from the backend.
        [[nodiscard]] bool IsDirty() const { return m_dirty; }
        void MarkDirty() { m_dirty = true; }
        void ClearDirty() { m_dirty = false; }

        //! Last time we acquired preview data (for throttling).
        [[nodiscard]] AZ::s64 LastAcquireTime() const { return m_lastAcquireTime; }
        void SetLastAcquireTime(AZ::s64 t) { m_lastAcquireTime = t; }

    protected:
        void paintEvent(QPaintEvent* event) override;

    private:
        AZStd::vector<AZ::u8> m_pixels;
        QImage m_image;           //!< QImage backed by m_pixels data (implicit sharing)
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_dirty = true;
        AZ::s64 m_lastAcquireTime = 0;
    };
} // namespace CrossEngineEditor
