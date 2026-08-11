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
#include <AzCore/std/string/string.h>

#include <cstdint>

namespace CrossEngineEditor
{
    //! A single vertex submitted to the backend immediate-mode debug renderer.
    //! GenericDebugDisplay tessellates every DebugDisplayRequests draw call down
    //! to batches of these (see plan §3.1 "70 -> 3" decomposition).
    struct DebugVertex
    {
        AZ::Vector3 m_position;
        AZ::Color m_color;
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
