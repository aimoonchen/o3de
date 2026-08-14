/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <AzQtComponents/Components/TitleBarOverdrawHandler_win.h>
#include <qnamespace.h>
#include <AzQtComponents/Components/TitleBarOverdrawScreenHandler_win.h>
#include <QDockWidget>

#include <QApplication>
#include <QOperatingSystemVersion>
#include <QScreen>
#include <QWindow>
#include <QWidget>
#include <QtGui/qpa/qplatformnativeinterface.h>

#include <VersionHelpers.h>
#include <shellapi.h>

namespace AzQtComponents
{

namespace
{

QMargins getMonitorAutohiddenTaskbarMargins(HMONITOR monitor)
{
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfo(monitor, &mi);
    auto hasTopmostAutohiddenTaskbar = [&mi](unsigned edge)
    {
        APPBARDATA data = { sizeof(APPBARDATA), NULL, 0, edge, mi.rcMonitor };
        HWND bar;

        if (IsWindows8OrGreater())
        {
            bar = reinterpret_cast<HWND>(SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data));
        }
        else
        {
            bar = reinterpret_cast<HWND>(SHAppBarMessage(ABM_GETAUTOHIDEBAR, &data));
        }

        return bar && (GetWindowLong(bar, GWL_EXSTYLE) & WS_EX_TOPMOST);
    };

    QMargins margins;
    if (hasTopmostAutohiddenTaskbar(ABE_LEFT))
    {
        margins.setLeft(1);
    }
    if (hasTopmostAutohiddenTaskbar(ABE_RIGHT))
    {
        margins.setRight(1);
    }
    if (hasTopmostAutohiddenTaskbar(ABE_TOP))
    {
        margins.setTop(1);
    }
    if (hasTopmostAutohiddenTaskbar(ABE_BOTTOM))
    {
        margins.setBottom(1);
    }
    return margins;
}

}

TitleBarOverdrawHandlerWindows::TitleBarOverdrawHandlerWindows(QApplication* application, QObject* parent)
    : TitleBarOverdrawHandler(parent)
{
    Q_UNUSED(application);
    Q_UNUSED(parent);
    m_user32Module = LoadLibraryA("user32.dll");
    if (m_user32Module)
    {
        // The below DPI related methods exist in the user32.dll only on Windows 10 version 1607 and beyond.  We attempt to locate these
        // methods at runtime through GetProcAddress so this module can still load correctly on older version of windows
        m_getDpiForSystemFn = (PFNGetDpiForSystem) GetProcAddress(m_user32Module, "GetDpiForSystem");
        m_getDpiForWindowFn = (PFNGetDpiForWindow) GetProcAddress(m_user32Module, "GetDpiForWindow");
        m_adjustWindowRectExForDpiFn = (PFNAdjustWindowRectExForDpi) GetProcAddress(m_user32Module, "AdjustWindowRectExForDpi");
    }
}

TitleBarOverdrawHandlerWindows::~TitleBarOverdrawHandlerWindows()
{
    if (m_user32Module)
    {
        FreeLibrary(m_user32Module);
    }
}

QMargins TitleBarOverdrawHandlerWindows::customTitlebarMargins(HMONITOR monitor, unsigned style, unsigned exStyle, bool maximized, int dpi)
{
    RECT rect = { 0, 0, 500, 500 };

    if (m_adjustWindowRectExForDpiFn)
    {
        m_adjustWindowRectExForDpiFn(&rect, style, FALSE, exStyle, dpi);
    }
    else
    {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }

    QMargins margins(0, rect.top, 0, 0);
    if (maximized)
    {
        margins.setTop(margins.top() - rect.left);
        if (monitor)
        {
            margins += getMonitorAutohiddenTaskbarMargins(monitor);
        }
    }

    return margins;
}

void TitleBarOverdrawHandlerWindows::applyOverdrawMargins(QWindow* window)
{
    if (auto platformWindow = window->handle())
    {
        auto hWnd = (HWND)window->winId();
        WINDOWPLACEMENT placement;
        placement.length = sizeof(WINDOWPLACEMENT);
        const bool maximized = GetWindowPlacement(hWnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED;
        applyOverdrawMargins(platformWindow, hWnd, maximized);
    }
    else
    {
        // We should not create a real window (HWND) yet, so get margins using presumed style
        const static unsigned style = WS_OVERLAPPEDWINDOW & ~WS_OVERLAPPED;
        const static unsigned exStyle = 0;
        // Use the real system DPI for the creation-context margins: a Per-Monitor-V2 window is
        // created at its birth monitor's DPI and the baked margins are never recomputed in Qt6
        // (the runtime re-applies below are a no-op), so baking in 96-DPI margins leaves part of
        // the native title bar uncovered on scaled displays. GetDpiForSystem() equals the birth
        // monitor's DPI for windows born on the primary screen; a window born on a secondary
        // monitor with a different DPI is a known uncovered edge (Qt rescales customMargins only
        // on a subsequent WM_DPICHANGED).
        const UINT dpi = m_getDpiForSystemFn ? m_getDpiForSystemFn() : 96;
        const auto margins = customTitlebarMargins(nullptr, style, exStyle, false, static_cast<int>(dpi));
        // ... and apply them to the creation context for the future window
        window->setProperty("_q_windowsCustomMargins", QVariant::fromValue(margins));
    }
}

void TitleBarOverdrawHandlerWindows::polish(QWidget* widget)
{
    if (strcmp(widget->metaObject()->className(), "QDockWidgetGroupWindow") == 0)
    {
        addTitleBarOverdrawWidget(widget);
    }
}

void TitleBarOverdrawHandlerWindows::addTitleBarOverdrawWidget(QWidget* widget)
{
    if(QOperatingSystemVersion::current() < QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10) ||
        m_overdrawWidgets.contains(widget))
    {
        return;
    }

    m_overdrawWidgets.append(widget);
    connect(widget, &QWidget::destroyed, this, [widget, this] {
        m_overdrawWidgets.removeOne(widget);
    });

    if (auto handle = widget->windowHandle())
    {
        applyOverdrawMargins(handle);
        // Use TitleBarOverdrawScreenHandler for Qt 5.12.4.1-az and upwards, it uses Qt signals instead
        // of native events and prevents window resize loops on multiscreen setups with different dpis.
        m_screenHandlers.push_back(new TitleBarOverdrawScreenHandler(handle, this));
    }
    // We might not have a window handle yet if handling a StyledDockWidget,
    // make sure to apply margins once the widget becomes toplevel
    else if (auto dockWidget = qobject_cast<QDockWidget*>(widget))
    {
        m_screenHandlers.push_back(new TitleBarOverdrawScreenHandler(dockWidget, this));
    }
}

QPlatformWindow* TitleBarOverdrawHandlerWindows::overdrawWindow(const QVector<QWidget*>& overdrawWidgets, HWND hWnd)
{
    for (auto widget : overdrawWidgets)
    {
        auto handle = widget->windowHandle();
        if (handle && widget->internalWinId() == (WId)hWnd)
        {
            return handle->handle();
        }
    }

    return nullptr;
}

void TitleBarOverdrawHandlerWindows::applyOverdrawMargins(QPlatformWindow* window, HWND hWnd, bool maximized)
{
    if (auto pni = QGuiApplication::platformNativeInterface())
    {
        const auto style = GetWindowLongPtr(hWnd, GWL_STYLE);
        if (!(style & WS_CHILD))
        {
            const auto exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
            const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONULL);

            UINT dpi = 0;
            if (m_getDpiForWindowFn)
            {
                dpi = m_getDpiForWindowFn(hWnd);
            }

            const auto margins = customTitlebarMargins(monitor, static_cast<int>(style), static_cast<int>(exStyle), maximized, dpi);
            RECT rect;
            GetWindowRect(hWnd, &rect);
            // Qt 6 has no runtime "WindowsCustomMargins" window property: the QPA base
            // implementation of setWindowProperty is empty and the windows plugin never
            // overrides it, so this re-apply chain (screenChanged / Show / WindowStateChange)
            // is inert. The only effective path is the creation-context
            // "_q_windowsCustomMargins" property; Qt itself rescales custom margins on
            // WM_DPICHANGED, which keeps cross-monitor overdraw correct.
            pni->setWindowProperty(window, QStringLiteral("WindowsCustomMargins"), QVariant::fromValue(margins));
            const auto width = rect.right - rect.left;
            const auto height = rect.bottom - rect.top;
            SetWindowPos(hWnd, 0, rect.left, rect.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

} // namespace AzQtComponents
