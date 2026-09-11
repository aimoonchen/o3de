/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Top-level engine backend service (Plan §B1).
//!
//! Exactly one backend is registered at startup via AZ::Interface<IEngineBackend>.
//! The editor shell only ever talks to this abstraction, never to a concrete engine.
//! Sub-contracts (Plan §A6 C4: engine-specific semantics forbidden; rendering/editing/asset faces):
//!   - ISceneRenderer  (rendering: surface lifecycle + overlay 3 primitives)
//!   - IEntityMirror   (editing: 19 methods = 15 pure-virtual + 4 sentinel defaults)
//!   - IAssetSource    (assets: 2 enumeration pure-virtual)
//! Camera / viewport interaction / picking reuse existing O3DE buses (framework-initiated, Plan §B5b).

#include <BackendAPI/BackendTypes.h>

#include <AzCore/RTTI/RTTI.h>

#include <expected>

namespace CrossEngineEditor
{
    class ISceneRenderer;
    class IEntityMirror;
    class IAssetSource;
    class IMaterialSource;

    class IEngineBackend
    {
    public:
        AZ_RTTI(IEngineBackend, "{7B2F1C4A-9E6D-4C1B-9F3A-2D5E8A0C4B11}");
        virtual ~IEngineBackend() = default;

        //! Bring the engine runtime up for the given project. Returns an error on failure.
        [[nodiscard]] virtual std::expected<void, BackendError> Initialize(const BackendInitParams& params) = 0;

        //! Tear the engine runtime down.
        virtual void Shutdown() = 0;

        //! Advance the engine one editor frame (Plan §B3 single-loop). Driven from the editor idle tick.
        virtual void Tick(float deltaSeconds) = 0;

        virtual ISceneRenderer& GetSceneRenderer() = 0;
        virtual IEntityMirror& GetEntityMirror() = 0;
        virtual IAssetSource& GetAssetSource() = 0;
        virtual IMaterialSource& GetMaterialSource() = 0;

        //! Engine-specific editor commands for the ActionManager (editor_polish.md P1-13 / M1).
        //! Default: none (a sentinel-style default keeps the pure-virtual surface unchanged -
        //! C4). The shell registers whatever a backend returns through a generic loop, so an
        //! engine adds its own menus/commands by implementing only this method.
        [[nodiscard]] virtual AZStd::vector<EngineActionPattern> GetActionRegistrationPatterns()
        {
            return {};
        }

        //! Engine-extension command escape hatch (E2 contract slimming, filament_migration.md
        //! §7.1). The operations that are not shared editing value live here instead of on the
        //! mirror's pure-virtual surface; a backend opts in by command name, everything else
        //! falls through to the "unsupported" default. Known commands and their arg layout:
        //!   "CreatePrefabFromNodes"  args = { entityId, ..., path }     payload unused
        //!   "AssignMaterial"         args = { entityId, assetPath, slotIndex }  payload unused
        //!   "AssignAnimation"        args = { entityId, assetPath }    payload unused
        //!   "SerializeNodes"         args = { entityId, ... }          payload = out bytes
        //!   "PasteNodes"             args = { parentId }               payload = in bytes
        //! (an invalid parent id is passed as the empty string = scene root). Returns false
        //! when the backend does not support the command or the command failed.
        virtual bool InvokeCustom(
            const AZStd::string& command, const AZStd::vector<AZStd::string>& args,
            AZStd::vector<AZ::u8>& payload)
        {
            (void)command;
            (void)args;
            (void)payload;
            return false;
        }
    };
} // namespace CrossEngineEditor
