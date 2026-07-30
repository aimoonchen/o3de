/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
#pragma once

#include <AzCore/Math/Color.h>
#include <QMetaType>

// Qt6: Q_DECLARE_METATYPE(AZ::Color) removed during the Qt6 upgrade. AZ::Color is
// NOT a Qt built-in type; it was disabled because its QDataStream operators are no
// longer auto-registered the Qt5 way. Any QVariant/streaming use of AZ::Color must
// be revalidated (see Qt6 upgrade risk R7) before re-enabling this declaration.
// Q_DECLARE_METATYPE(AZ::Color)
