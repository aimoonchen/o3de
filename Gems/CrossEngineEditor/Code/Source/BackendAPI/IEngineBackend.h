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
//! Sub-contracts (Plan §A6 C4: 契约禁引擎专属语义; 渲染面/编辑面/资产面 三子契约):
//!   - ISceneRenderer  (渲染面: 表面生命周期 + overlay 3 原语)
//!   - IEntityMirror   (编辑面: 19 方法 = 15 纯虚 + 4 哨兵默认)
//!   - IAssetSource    (资产面: 2 枚举纯虚)
//! Camera / viewport interaction / picking reuse existing O3DE buses (框架发起, Plan §B5b).

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

        //! Advance the engine one editor frame (Plan §B3 单循环). Driven from the editor idle tick.
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
    };
} // namespace CrossEngineEditor
