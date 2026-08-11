/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Thin GDExtension C-API facade (final plan §3.2/§6.3): a single choke point that turns the
//! GDExtensionInterfaceGetProcAddress handed to us by libgodot into the small set of function
//! pointers the Godot backend needs, plus a handful of ergonomic helpers.
//!
//! Why this exists (KISS): libgodot returns an opaque GodotInstance object and the engine's
//! scene tree / nodes / properties are reachable ONLY through the GDExtension C interface. We
//! deliberately do NOT link Godot's internal C++ classes (SceneTree/Node/Camera3D headers) at
//! build time - that would drag Godot's whole include island in and fight the runtime-load
//! architecture. Instead every engine operation is expressed as "call a named method on an
//! Object / read-write a named property", which the GDExtension interface supports directly and
//! WITHOUT method-hash fragility (we go through Variant::call, not classdb_get_method_bind).
//!
//! All Godot value marshalling is confined to this header so the rest of the backend speaks in
//! plain C++/AZ types.

#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Color.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/containers/vector.h>

#if defined(_MSC_VER)
#    pragma warning(push, 0)
#endif
#if defined(CEE_GODOT_GDEXTENSION_HEADER)
#    include CEE_GODOT_GDEXTENSION_HEADER
#else
#    include "core/extension/gdextension_interface.gen.h"
#endif
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

#include <cstring>

namespace CrossEngineEditor
{
    //! Godot uses opaque byte buffers for Variant/StringName. The GDExtension ABI guarantees a
    //! Variant is at most GDExtensionVariantSize bytes; StringName is a single pointer. We store
    //! them in fixed local buffers so callers never touch Godot's real class layout.
    //! (Variant max size across Godot 4.x builds is well under 40 bytes; 40 is a safe upper bound.)
    //!
    //! Ownership (GDExtension ABI): a Variant holding a ref-counted payload (String / Array /
    //! Dictionary / Transform3D / Object(RefCounted) / ...) owns an engine-side heap reference that
    //! MUST be released with variant_destroy, or the engine leaks. Inline types (bool/int/float/
    //! Vector3/Color) hold nothing, and variant_destroy is a safe no-op on them, so we simply call
    //! it on every constructed Variant. GodotVariant is therefore move-only RAII: every factory in
    //! GodotApi stamps m_destroy; the destructor releases; copies are forbidden (a byte-copy would
    //! alias one payload into two buffers and double-free).
    struct GodotVariant
    {
        alignas(8) uint8_t m_bytes[40] = {};
        GDExtensionInterfaceVariantDestroy m_destroy = nullptr; //!< set by GodotApi factories.

        GodotVariant() = default;
        ~GodotVariant() { Release(); }

        GodotVariant(const GodotVariant&) = delete;
        GodotVariant& operator=(const GodotVariant&) = delete;

        GodotVariant(GodotVariant&& other) noexcept { MoveFrom(other); }
        GodotVariant& operator=(GodotVariant&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                MoveFrom(other);
            }
            return *this;
        }

        void* Ptr() { return m_bytes; }
        const void* Ptr() const { return m_bytes; }

    private:
        void Release()
        {
            if (m_destroy)
            {
                m_destroy(m_bytes);
                m_destroy = nullptr;
            }
        }
        void MoveFrom(GodotVariant& other)
        {
            std::memcpy(m_bytes, other.m_bytes, sizeof(m_bytes));
            m_destroy = other.m_destroy;
            other.m_destroy = nullptr;            // moved-from: no longer owns the payload.
            std::memset(other.m_bytes, 0, sizeof(other.m_bytes));
        }
    };

    //! Resolved GDExtension entry points + helpers. One instance, owned by the backend, shared by
    //! every sub-service. Valid only after Bind() succeeds (returns false otherwise).
    class GodotApi
    {
    public:
        //! Resolve every function pointer we need from libgodot's get_proc_address. Returns false
        //! if the interface is missing a required entry (then the backend degrades gracefully).
        bool Bind(GDExtensionInterfaceGetProcAddress getProc)
        {
            if (!getProc)
            {
                return false;
            }
            m_getProc = getProc;

            m_stringNameNew = reinterpret_cast<GDExtensionInterfaceStringNameNewWithUtf8Chars>(
                getProc("string_name_new_with_utf8_chars"));
            m_stringNew = reinterpret_cast<GDExtensionInterfaceStringNewWithUtf8Chars>(
                getProc("string_new_with_utf8_chars"));
            m_stringToUtf8 = reinterpret_cast<GDExtensionInterfaceStringToUtf8Chars>(
                getProc("string_to_utf8_chars"));
            m_variantNewNil = reinterpret_cast<GDExtensionInterfaceVariantNewNil>(
                getProc("variant_new_nil"));
            m_variantDestroy = reinterpret_cast<GDExtensionInterfaceVariantDestroy>(
                getProc("variant_destroy"));
            m_variantGetType = reinterpret_cast<GDExtensionInterfaceVariantGetType>(
                getProc("variant_get_type"));
            m_variantCall = reinterpret_cast<GDExtensionInterfaceVariantCall>(
                getProc("variant_call"));
            m_variantGetNamed = reinterpret_cast<GDExtensionInterfaceVariantGetNamed>(
                getProc("variant_get_named"));
            m_variantSetNamed = reinterpret_cast<GDExtensionInterfaceVariantSetNamed>(
                getProc("variant_set_named"));
            m_variantGetIndexed = reinterpret_cast<GDExtensionInterfaceVariantGetIndexed>(
                getProc("variant_get_indexed"));
            m_variantGetKeyed = reinterpret_cast<GDExtensionInterfaceVariantGetKeyed>(
                getProc("variant_get_keyed"));
            m_fromType = reinterpret_cast<GDExtensionInterfaceGetVariantFromTypeConstructor>(
                getProc("get_variant_from_type_constructor"));
            m_toType = reinterpret_cast<GDExtensionInterfaceGetVariantToTypeConstructor>(
                getProc("get_variant_to_type_constructor"));
            m_getPtrDestructor = reinterpret_cast<GDExtensionInterfaceVariantGetPtrDestructor>(
                getProc("variant_get_ptr_destructor"));
            m_globalGetSingleton = reinterpret_cast<GDExtensionInterfaceGlobalGetSingleton>(
                getProc("global_get_singleton"));
            m_constructObject = reinterpret_cast<GDExtensionInterfaceClassdbConstructObject2>(
                getProc("classdb_construct_object2"));
            if (!m_constructObject)
            {
                // 4.7 renamed to construct_object3; fall back for either ABI.
                m_constructObject = reinterpret_cast<GDExtensionInterfaceClassdbConstructObject2>(
                    getProc("classdb_construct_object3"));
            }
            m_objectGetInstanceId = reinterpret_cast<GDExtensionInterfaceObjectGetInstanceId>(
                getProc("object_get_instance_id"));
            m_objectFromId = reinterpret_cast<GDExtensionInterfaceObjectGetInstanceFromId>(
                getProc("object_get_instance_from_id"));

            m_valid = m_stringNameNew && m_stringNew && m_stringToUtf8 && m_variantNewNil &&
                m_variantDestroy && m_variantGetType && m_variantCall && m_variantGetNamed &&
                m_variantSetNamed && m_fromType && m_toType && m_globalGetSingleton &&
                m_objectGetInstanceId && m_objectFromId;
            return m_valid;
        }

        bool IsValid() const { return m_valid; }

        // ---- StringName / String -------------------------------------------------------------

        //! Build a StringName (single-pointer opaque) from utf8. Caller keeps it on the stack.
        void MakeStringName(const char* utf8, void* outStringName) const
        {
            m_stringNameNew(outStringName, utf8);
        }

        //! Read a Godot String Variant into an AZStd::string.
        AZStd::string VariantStringToAz(const GodotVariant& v) const
        {
            // Convert Variant(STRING) -> String buffer, then String -> utf8.
            uint8_t stringBuf[16] = {}; // Godot String is a single CowData pointer; 16 is safe.
            auto toStr = m_toType(GDEXTENSION_VARIANT_TYPE_STRING);
            if (!toStr)
            {
                return {};
            }
            toStr(stringBuf, const_cast<void*>(v.Ptr()));
            const GDExtensionInt len = m_stringToUtf8(stringBuf, nullptr, 0);
            AZStd::string out;
            if (len > 0)
            {
                out.resize(static_cast<size_t>(len));
                m_stringToUtf8(stringBuf, out.data(), len);
            }
            DestroyBuiltin(GDEXTENSION_VARIANT_TYPE_STRING, stringBuf); // release the CowData ref.
            return out;
        }

        // ---- Variant construction from primitives --------------------------------------------

        GodotVariant MakeNil() const
        {
            GodotVariant v;
            m_variantNewNil(v.Ptr());
            v.m_destroy = m_variantDestroy; // take ownership so the payload is released on scope exit.
            return v;
        }

        GodotVariant MakeBool(bool b) const { return FromPrimitive(GDEXTENSION_VARIANT_TYPE_BOOL, &b); }
        GodotVariant MakeInt(int64_t i) const { return FromPrimitive(GDEXTENSION_VARIANT_TYPE_INT, &i); }
        GodotVariant MakeFloat(double d) const { return FromPrimitive(GDEXTENSION_VARIANT_TYPE_FLOAT, &d); }

        GodotVariant MakeString(const char* utf8) const
        {
            uint8_t stringBuf[16] = {};
            m_stringNew(stringBuf, utf8);
            GodotVariant v = FromPrimitive(GDEXTENSION_VARIANT_TYPE_STRING, stringBuf);
            DestroyBuiltin(GDEXTENSION_VARIANT_TYPE_STRING, stringBuf); // Variant copied it; drop ours.
            return v;
        }

        //! Godot Vector3 is 3 floats; Color is 4 floats (r,g,b,a) - matching the ABI layout.
        GodotVariant MakeVector3(const AZ::Vector3& v) const
        {
            float raw[3] = { v.GetX(), v.GetY(), v.GetZ() };
            return FromPrimitive(GDEXTENSION_VARIANT_TYPE_VECTOR3, raw);
        }
        GodotVariant MakeColor(const AZ::Color& c) const
        {
            float raw[4] = { c.GetR(), c.GetG(), c.GetB(), c.GetA() };
            return FromPrimitive(GDEXTENSION_VARIANT_TYPE_COLOR, raw);
        }

        //! Wrap an Object pointer in a Variant(OBJECT) so we can call methods on it.
        GodotVariant MakeObject(GDExtensionObjectPtr obj) const
        {
            return FromPrimitive(GDEXTENSION_VARIANT_TYPE_OBJECT, &obj);
        }

        //! Godot Transform3D ABI is 12 floats: Basis rows (3x3, row-major) then origin (x,y,z).
        //! raw12 must point to 12 floats laid out [b00..b02, b10..b12, b20..b22, ox,oy,oz].
        GodotVariant MakeTransform3D(const float raw12[12]) const
        {
            return FromPrimitive(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, raw12);
        }
        void AsTransform3D(const GodotVariant& v, float outRaw12[12]) const
        {
            ToPrimitive(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, v, outRaw12);
        }

        // ---- Variant readback to primitives --------------------------------------------------

        GDExtensionVariantType TypeOf(const GodotVariant& v) const { return m_variantGetType(v.Ptr()); }

        bool AsBool(const GodotVariant& v) const { bool b = false; ToPrimitive(GDEXTENSION_VARIANT_TYPE_BOOL, v, &b); return b; }
        int64_t AsInt(const GodotVariant& v) const { int64_t i = 0; ToPrimitive(GDEXTENSION_VARIANT_TYPE_INT, v, &i); return i; }
        double AsFloat(const GodotVariant& v) const { double d = 0; ToPrimitive(GDEXTENSION_VARIANT_TYPE_FLOAT, v, &d); return d; }
        AZ::Vector3 AsVector3(const GodotVariant& v) const
        {
            float raw[3] = {};
            ToPrimitive(GDEXTENSION_VARIANT_TYPE_VECTOR3, v, raw);
            return AZ::Vector3(raw[0], raw[1], raw[2]);
        }
        AZ::Color AsColor(const GodotVariant& v) const
        {
            float raw[4] = {};
            ToPrimitive(GDEXTENSION_VARIANT_TYPE_COLOR, v, raw);
            return AZ::Color(raw[0], raw[1], raw[2], raw[3]);
        }
        //! Extract the underlying Object pointer from a Variant(OBJECT), or null.
        GDExtensionObjectPtr AsObject(const GodotVariant& v) const
        {
            if (TypeOf(v) != GDEXTENSION_VARIANT_TYPE_OBJECT)
            {
                return nullptr;
            }
            GDExtensionObjectPtr obj = nullptr;
            ToPrimitive(GDEXTENSION_VARIANT_TYPE_OBJECT, v, &obj);
            return obj;
        }

        // ---- Object operations ---------------------------------------------------------------

        //! Godot's global singleton by name (e.g. "SceneTree" is NOT a singleton; use it via the
        //! MainLoop - see backend). Kept for engine singletons we do resolve by name.
        GDExtensionObjectPtr GetSingleton(const char* name) const
        {
            uint8_t sn[sizeof(void*)] = {};
            MakeStringName(name, sn);
            GDExtensionObjectPtr singleton = m_globalGetSingleton(sn);
            DestroyBuiltin(GDEXTENSION_VARIANT_TYPE_STRING_NAME, sn);
            return singleton;
        }

        //! Stable per-scene id of an object (stored as EngineNodeComponent's node handle).
        uint64_t InstanceId(GDExtensionObjectPtr obj) const
        {
            return obj ? static_cast<uint64_t>(m_objectGetInstanceId(obj)) : 0;
        }
        GDExtensionObjectPtr ObjectFromId(uint64_t id) const
        {
            return m_objectFromId(static_cast<GDObjectInstanceID>(id));
        }

        //! Instantiate a registered engine class by name (ClassDB). Returns null on failure.
        GDExtensionObjectPtr ConstructObject(const char* className) const
        {
            if (!m_constructObject)
            {
                return nullptr;
            }
            uint8_t sn[sizeof(void*)] = {};
            MakeStringName(className, sn);
            GDExtensionObjectPtr obj = m_constructObject(sn);
            DestroyBuiltin(GDEXTENSION_VARIANT_TYPE_STRING_NAME, sn);
            return obj;
        }

        //! Call a named method on an object with N Variant args. Returns the result Variant.
        GodotVariant Call(GDExtensionObjectPtr obj, const char* method,
            const GodotVariant* args = nullptr, int argc = 0) const
        {
            GodotVariant self = MakeObject(obj);
            uint8_t sn[sizeof(void*)] = {};
            MakeStringName(method, sn);

            AZStd::vector<const void*> argPtrs;
            argPtrs.reserve(argc);
            for (int i = 0; i < argc; ++i)
            {
                argPtrs.push_back(args[i].Ptr());
            }

            GodotVariant ret = MakeNil();
            GDExtensionCallError err{};
            m_variantCall(self.Ptr(), sn,
                reinterpret_cast<const GDExtensionConstVariantPtr*>(argPtrs.data()),
                static_cast<GDExtensionInt>(argc), ret.Ptr(), &err);
            DestroyBuiltin(GDEXTENSION_VARIANT_TYPE_STRING_NAME, sn);
            return ret;
        }

        //! Read a named property off an object (Object.get(name) via Variant get_named).
        GodotVariant GetProperty(GDExtensionObjectPtr obj, const char* name) const
        {
            GodotVariant self = MakeObject(obj);
            uint8_t sn[sizeof(void*)] = {};
            MakeStringName(name, sn);
            GodotVariant ret = MakeNil();
            GDExtensionBool valid = false;
            m_variantGetNamed(self.Ptr(), sn, ret.Ptr(), &valid);
            DestroyBuiltin(GDEXTENSION_VARIANT_TYPE_STRING_NAME, sn);
            return ret;
        }

        //! Write a named property on an object (Object.set(name, value) via Variant set_named).
        void SetProperty(GDExtensionObjectPtr obj, const char* name, const GodotVariant& value) const
        {
            GodotVariant self = MakeObject(obj);
            uint8_t sn[sizeof(void*)] = {};
            MakeStringName(name, sn);
            GDExtensionBool valid = false;
            m_variantSetNamed(self.Ptr(), sn, value.Ptr(), &valid);
            DestroyBuiltin(GDEXTENSION_VARIANT_TYPE_STRING_NAME, sn);
        }

        // ---- Builtin Variant containers (Array / Dictionary) ---------------------------------
        // get_property_list() returns a TypedArray<Dictionary> (a Variant of type ARRAY whose
        // elements are Variant(DICTIONARY)). We read it WITHOUT constructing Godot's Array/
        // Dictionary C++ types: element access goes through variant_get_indexed (Array by index)
        // and variant_get_keyed (Dictionary by key Variant). Both are plain GDExtension calls,
        // so property-list marshalling stays inside this facade (KISS, plan §6.3).

        //! Number of elements in an Array Variant (calls its builtin "size" method).
        int64_t ArraySize(const GodotVariant& arr) const
        {
            GodotVariant ret = CallVariant(arr, "size");
            return AsInt(ret);
        }

        //! Element i of an Array Variant via variant_get_indexed. Returns Nil on failure.
        GodotVariant ArrayGet(const GodotVariant& arr, int64_t index) const
        {
            GodotVariant ret = MakeNil();
            if (m_variantGetIndexed)
            {
                GDExtensionBool valid = false;
                GDExtensionBool oob = false;
                m_variantGetIndexed(arr.Ptr(), static_cast<GDExtensionInt>(index), ret.Ptr(), &valid, &oob);
            }
            return ret;
        }

        //! Value for a string key of a Dictionary Variant via variant_get_keyed.
        GodotVariant DictGet(const GodotVariant& dict, const char* key) const
        {
            GodotVariant ret = MakeNil();
            if (m_variantGetKeyed)
            {
                GodotVariant keyV = MakeString(key);
                GDExtensionBool valid = false;
                m_variantGetKeyed(dict.Ptr(), keyV.Ptr(), ret.Ptr(), &valid);
            }
            return ret;
        }

        //! Call a builtin method on ANY Variant (Array/Dictionary/etc.), not just Objects.
        //! Unlike Call(), the self here is a Variant value, not an Object pointer.
        GodotVariant CallVariant(const GodotVariant& self, const char* method,
            const GodotVariant* args = nullptr, int argc = 0) const
        {
            uint8_t sn[sizeof(void*)] = {};
            MakeStringName(method, sn);

            AZStd::vector<const void*> argPtrs;
            argPtrs.reserve(argc);
            for (int i = 0; i < argc; ++i)
            {
                argPtrs.push_back(args[i].Ptr());
            }

            GodotVariant ret = MakeNil();
            GDExtensionCallError err{};
            m_variantCall(const_cast<void*>(self.Ptr()), sn,
                reinterpret_cast<const GDExtensionConstVariantPtr*>(argPtrs.data()),
                static_cast<GDExtensionInt>(argc), ret.Ptr(), &err);
            DestroyBuiltin(GDEXTENSION_VARIANT_TYPE_STRING_NAME, sn);
            return ret;
        }

    private:
        GodotVariant FromPrimitive(GDExtensionVariantType type, const void* raw) const
        {
            GodotVariant v = MakeNil();
            if (auto ctor = m_fromType(type))
            {
                ctor(v.Ptr(), const_cast<void*>(raw));
            }
            return v;
        }
        void ToPrimitive(GDExtensionVariantType type, const GodotVariant& v, void* outRaw) const
        {
            if (auto conv = m_toType(type))
            {
                conv(outRaw, const_cast<void*>(v.Ptr()));
            }
        }

        //! Release a raw (non-Variant) builtin value buffer, e.g. a String produced by
        //! to_type(STRING) / string_new_with_utf8_chars. String owns a CowData heap pointer, so it
        //! must be destructed or it leaks. No-op if the destructor interface is unavailable.
        void DestroyBuiltin(GDExtensionVariantType type, void* raw) const
        {
            if (m_getPtrDestructor)
            {
                if (auto dtor = m_getPtrDestructor(type))
                {
                    dtor(raw);
                }
            }
        }

        GDExtensionInterfaceGetProcAddress m_getProc = nullptr;
        GDExtensionInterfaceStringNameNewWithUtf8Chars m_stringNameNew = nullptr;
        GDExtensionInterfaceStringNewWithUtf8Chars m_stringNew = nullptr;
        GDExtensionInterfaceStringToUtf8Chars m_stringToUtf8 = nullptr;
        GDExtensionInterfaceVariantNewNil m_variantNewNil = nullptr;
        GDExtensionInterfaceVariantDestroy m_variantDestroy = nullptr;
        GDExtensionInterfaceVariantGetType m_variantGetType = nullptr;
        GDExtensionInterfaceVariantCall m_variantCall = nullptr;
        GDExtensionInterfaceVariantGetNamed m_variantGetNamed = nullptr;
        GDExtensionInterfaceVariantSetNamed m_variantSetNamed = nullptr;
        GDExtensionInterfaceVariantGetIndexed m_variantGetIndexed = nullptr;
        GDExtensionInterfaceVariantGetKeyed m_variantGetKeyed = nullptr;
        GDExtensionInterfaceGetVariantFromTypeConstructor m_fromType = nullptr;
        GDExtensionInterfaceGetVariantToTypeConstructor m_toType = nullptr;
        GDExtensionInterfaceVariantGetPtrDestructor m_getPtrDestructor = nullptr;
        GDExtensionInterfaceGlobalGetSingleton m_globalGetSingleton = nullptr;
        GDExtensionInterfaceClassdbConstructObject2 m_constructObject = nullptr;
        GDExtensionInterfaceObjectGetInstanceId m_objectGetInstanceId = nullptr;
        GDExtensionInterfaceObjectGetInstanceFromId m_objectFromId = nullptr;
        bool m_valid = false;
    };
} // namespace CrossEngineEditor
