/*
 * Copyright (c) Contributors to the Open 3D Engine Project. For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzQtComponents/AzQtComponentsAPI.h>
#include <QApplication>

namespace AzQtComponents
{
    //! Base class for O3DE Tools Applications
    class AZ_QT_COMPONENTS_API AzQtApplication
        : public QApplication
    {
    public:
        AzQtApplication(int& argc, char** argv);

        //! Initializes Qt DPI scaling to handle displays with high display densities, such as Retina displays.
            //! Currently, this uses Qt's per-monitor DPI awareness (the Qt 6 default) together with a
            //! PassThrough scale-factor rounding policy, so 150% stays exactly 1.5x.
            //! \warning This must be called before this AzQtApplication instance is initialized.
        static void InitializeDpiScaling();
    };

} // namespace AzQtComponents



