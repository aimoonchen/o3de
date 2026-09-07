/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! MaterialSchema: hand-written rapidjson Load/Save for MaterialTypeDesc.
//!
//! Bypasses AZ::JsonSerialization which doesn't support AZStd::variant or
//! AZStd::optional by design. Handles type-tagged variant serialization,
//! optional null/omit, and string enum names explicitly.

#include <BackendAPI/IMaterialSource.h>

#include <AzCore/std/string/string.h>

namespace CrossEngineEditor
{
    class MaterialSchema
    {
    public:
        //! Load a MaterialTypeDesc from a JSON file. Returns true on success.
        [[nodiscard]] static bool LoadFromFile(const AZStd::string& filePath, MaterialTypeDesc& outSchema);

        //! Load a MaterialTypeDesc + values from a JSON file.
        //! Expects format: {"m_typeId":"...", "m_schema":{...}, "m_values":{...}}
        //! Falls back to defaults for any value not present in the file.
        [[nodiscard]] static bool LoadFromFile(
            const AZStd::string& filePath, MaterialTypeDesc& outSchema, MaterialPropertyValueMap& outValues);

        //! Load a MaterialTypeDesc from a JSON string. Returns true on success.
        [[nodiscard]] static bool LoadFromString(const AZStd::string& jsonString, MaterialTypeDesc& outSchema);

        //! Save a MaterialTypeDesc to a JSON file. Returns true on success.
        [[nodiscard]] static bool SaveToFile(const AZStd::string& filePath, const MaterialTypeDesc& schema);

        //! Save a MaterialTypeDesc + values to a JSON file.
        //! Format: {"m_typeId":"...", "m_schema":{...}, "m_values":{...}}
        [[nodiscard]] static bool SaveToFile(
            const AZStd::string& filePath, const MaterialTypeDesc& schema, const MaterialPropertyValueMap& values);
    };
} // namespace CrossEngineEditor
