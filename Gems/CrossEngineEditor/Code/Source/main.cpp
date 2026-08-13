/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include <Application/CrossEngineEditorApplication.h>

#include <AzCore/Debug/Trace.h>
#include <AzQtComponents/Application/AzQtApplication.h>

int main(int argc, char** argv)
{
    const AZ::Debug::Trace tracer;
    AzQtComponents::AzQtApplication::InitializeDpiScaling();

    CrossEngineEditor::CrossEngineEditorApplication app(&argc, &argv);
    app.Start({}, {});
    app.RunMainLoop();
    app.Stop();

    return 0;
}
