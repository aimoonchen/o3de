/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Application/CrossEngineEditorApplication.h>

#include <AzCore/Debug/Trace.h>
#include <AzQtComponents/Application/AzQtApplication.h>

#include <cstdio>

namespace
{
    //! Minimal always-on boot trace to a fixed file next to the exe, so a headless double-click
    //! that exits early still leaves evidence of how far startup got.
    void BootLog(const char* stage)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, "cee_boot.log", "a") == 0 && f != nullptr)
        {
            fprintf(f, "%s\n", stage);
            fclose(f);
        }
    }
} // namespace

int main(int argc, char** argv)
{
    BootLog("main:enter");
    const AZ::Debug::Trace tracer;
    AzQtComponents::AzQtApplication::InitializeDpiScaling();

    CrossEngineEditor::CrossEngineEditorApplication app(&argc, &argv);
    BootLog("app:constructed");
    app.Start({}, {});
    BootLog("app:started");
    app.RunMainLoop();
    BootLog("app:mainloop-returned");
    app.Stop();
    BootLog("app:stopped");

    return 0;
}
