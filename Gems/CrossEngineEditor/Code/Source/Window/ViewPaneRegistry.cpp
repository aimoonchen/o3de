/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Window/ViewPaneRegistry.h>

namespace CrossEngineEditor
{
    void ViewPaneRegistry::RegisterPane(ViewPaneEntry entry)
    {
        // Stable names are the whole point of the registry (ADS restoreState + menu ids key off
        // them), so a duplicate registration is a programming error worth failing loudly.
        if (const ViewPaneEntry* existing = Find(entry.m_name); existing != nullptr)
        {
            AZ_Assert(false, "ViewPane '%s' is already registered.", entry.m_name.toUtf8().constData());
            return;
        }
        m_entries.push_back(AZStd::move(entry));
    }

    const ViewPaneEntry* ViewPaneRegistry::Find(const QString& name) const
    {
        for (const ViewPaneEntry& entry : m_entries)
        {
            if (entry.m_name == name)
            {
                return &entry;
            }
        }
        return nullptr;
    }
} // namespace CrossEngineEditor
