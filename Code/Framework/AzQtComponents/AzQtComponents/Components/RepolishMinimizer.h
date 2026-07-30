/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <QStyle>

/**
 * RAII-style class that used to block unneeded polish requests when doing reparenting.
 * When QWidget::setParent() is called that triggers all children to be repolished, which is expensive
 * as all stylesheet rules have to be recalculated.
 *
 * The O3DE Qt5 fork exposed QStyle::enableMinimizePolishOptimizations() to skip that work.
 * Stock Qt6 has no such API, so on Qt6 this class is a no-op shell: correctness is unaffected,
 * only the reparenting polish optimization is no longer applied. Kept as a type so existing
 * call sites remain unchanged.
 */
namespace AzQtComponents
{
    class RepolishMinimizer
    {
    public:
        // User-provided (non-defaulted) special members so that stack instances are
        // treated as having side effects: avoids C4101 "unreferenced local variable"
        // at the many call sites that declare `RepolishMinimizer minimizer;` (with /WX).
        RepolishMinimizer() {}
        ~RepolishMinimizer() {}

    private:
        Q_DISABLE_COPY(RepolishMinimizer)
    };

} // namespace AzQtComponents
