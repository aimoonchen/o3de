/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
#pragma once

namespace QtForPython
{
    // Qt6 upgrade: PySide is now provided by the pip-installed PySide6 wheel in the
    // O3DE Python venv (self-contained; bundles its own Qt6 + shiboken6). The old
    // Linux path manually dlopen'd hardcoded Qt5-era shared objects
    // (libpyside2.abi3.so.5.15 / libshiboken2.abi3.so.5.15 / libQt5Test.so.5),
    // which no longer exist and must not be preloaded. Reduced to a no-op shell to
    // match the Windows/Mac platforms.
    class InitializeEmbeddedPyside2
    {
    public:
        InitializeEmbeddedPyside2() = default;
        virtual ~InitializeEmbeddedPyside2() = default;
    };
} // namespace QtForPython
