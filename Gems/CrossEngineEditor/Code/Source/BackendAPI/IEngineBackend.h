/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Top-level engine backend service (plan §3.3).
//!
//! Exactly one backend is registered at startup via AZ::Interface<IEngineBackend>.
//! The editor shell only ever talks to this abstraction, never to a concrete engine.
//! Sub-contracts:
//!   - ISceneRenderer  (C5 render + C4-device overlay primitives)
//!   - IEntityMirror   (C6 object model)
//!   - IAssetSource    (C8 assets)
//! Camera (C1), viewport interaction (C2) and picking (C3) reuse existing O3DE buses.

#include <BackendAPI/BackendTypes.h>

#include <AzCore/RTTI/RTTI.h>

#include <expected>

namespace CrossEngineEditor
{
    class ISceneRenderer;
    class IEntityMirror;
    class IAssetSource;

    class IEngineBackend
    {
    public:
        AZ_RTTI(IEngineBackend, "{7B2F1C4A-9E6D-4C1B-9F3A-2D5E8A0C4B11}");
        virtual ~IEngineBackend() = default;

        //! Bring the engine runtime up for the given project. Returns an error on failure.
        [[nodiscard]] virtual std::expected<void, BackendError> Initialize(const BackendInitParams& params) = 0;

        //! Tear the engine runtime down.
        virtual void Shutdown() = 0;

        //! Advance the engine one editor frame (C7). Driven from the editor idle tick.
        virtual void Tick(float deltaSeconds) = 0;

        virtual ISceneRenderer& GetSceneRenderer() = 0;
        virtual IEntityMirror& GetEntityMirror() = 0;
        virtual IAssetSource& GetAssetSource() = 0;
    };
} // namespace CrossEngineEditor
