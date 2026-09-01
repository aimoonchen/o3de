/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Cross-engine editor backend contract - shared value types.
//!
//! These are plain data types exchanged across the editor <-> engine boundary.
//! They intentionally depend only on AzCore math/containers so that a backend
//! implementation never needs to pull in editor internals.

#include <AzCore/Math/Color.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/functional.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/containers/vector.h>

#include <cstdint>

namespace CrossEngineEditor
{
    //! A single vertex submitted to the backend immediate-mode debug renderer.
    //! GenericDebugDisplay tessellates every DebugDisplayRequests draw call down
    //! to batches of these (see Plan §A3 "63 -> 3" decomposition).
    struct DebugVertex
    {
        AZ::Vector3 m_position;
        AZ::Color m_color;
    };

    //! One engine-specific editor command, described as data (editor_polish.md P1-13 / M1).
    //! Backends return a static table of these from IEngineBackend::
    //! GetActionRegistrationPatterns; a GENERIC loop in the shell registers them into the
    //! ActionManager, so wiring a new engine's own commands never touches shell code. Ids use
    //! cee.action.<engine>.* (D2b). An empty menu identifier means "command palette only".
    //! POD + AZStd::function only - no Qt types (C4 data-plane rule).
    struct EngineActionPattern
    {
        AZStd::string m_id;
        AZStd::string m_name;
        AZStd::string m_description;
        AZStd::string m_hotKey;        //!< Empty = no hotkey.
        AZStd::string m_menuIdentifier; //!< Empty = command palette only.
        int m_sortKey = 0;
        AZStd::function<void()> m_handler;
    };

    //! Parameters passed to IEngineBackend::Initialize.
    struct BackendInitParams
    {
        AZStd::string m_projectPath;   //!< Root path of the engine project to edit.
        AZStd::string m_scenePath;     //!< Optional engine scene to open on start (resource-relative).
        bool m_headless = false;       //!< True for automated tests (no window surface).
    };

    //! Identifies why a backend operation failed. Returned inside std::expected.
    enum class BackendError : uint8_t
    {
        NotInitialized,
        InvalidProject,
        RenderDeviceUnavailable,
        Unsupported,
        Internal,
    };
} // namespace CrossEngineEditor
