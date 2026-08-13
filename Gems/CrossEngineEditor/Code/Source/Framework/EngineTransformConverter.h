/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Coordinate-system conversion between the O3DE editor space and a target engine
//! space (final plan section 5). One place, pure math, engine-agnostic.
//!
//! The BackendAPI boundary is ALWAYS O3DE convention: Z-up, right-handed, metres.
//! Each backend converts at its own edge using the functions below, so the shell and
//! the manipulators never see engine-specific axes.
//!
//! Basis mapping (plan section 5, verified against I:\rbfx / I:\godot checkouts):
//!
//!   O3DE (Z-up, RH, +Y forward)  ->  rbfx  (Y-up, LH, +Z forward):   pos [x, z,  y]
//!   O3DE (Z-up, RH, +Y forward)  ->  Godot (Y-up, RH, -Z forward):   pos [x, z, -y]
//!
//! Both are the "swap Y and Z" basis (a -90 deg rotation about X). rbfx additionally
//! flips handedness (RH->LH); Godot stays RH. A handedness flip negates the rotation
//! angle, i.e. negates the quaternion's imaginary (axis) part after the same axis remap.

#include <AzCore/Math/Aabb.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/Math/Matrix3x3.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>

namespace CrossEngineEditor
{
    //! Target engine coordinate conventions handled by the converter.
    enum class EngineSpace
    {
        Rbfx,  //!< Y-up, left-handed, +Z forward.
        Godot, //!< Y-up, right-handed, -Z forward.
    };

    class EngineTransformConverter
    {
    public:
        //! O3DE position -> engine position. Both engines swap Y and Z; Godot also
        //! negates the (now) Z-forward so O3DE +Y forward maps to Godot -Z.
        static AZ::Vector3 PositionToEngine(EngineSpace space, const AZ::Vector3& p)
        {
            const float z = (space == EngineSpace::Godot) ? -p.GetY() : p.GetY();
            return AZ::Vector3(p.GetX(), p.GetZ(), z);
        }

        //! Inverse of PositionToEngine (engine position -> O3DE position).
        static AZ::Vector3 PositionFromEngine(EngineSpace space, const AZ::Vector3& p)
        {
            const float y = (space == EngineSpace::Godot) ? -p.GetZ() : p.GetZ();
            return AZ::Vector3(p.GetX(), y, p.GetY());
        }

        //! O3DE rotation -> engine rotation.
        //!
        //! A coordinate-system change is a *similarity* transform of the rotation. We build it at
        //! the matrix level (M_engine = C * M_o3de * C^T) which is correct for both a pure rotation
        //! (Godot: C is a +/-90 deg rotation about X, det +1) and a handedness-flipping reflection
        //! (rbfx: C swaps Y/Z, det -1). Doing it as a matrix similarity avoids the quaternion
        //! component-swap shortcut that previously mis-mapped basis columns (camera forward ended
        //! up in the up column).
        static AZ::Quaternion RotationToEngine(EngineSpace space, const AZ::Quaternion& q)
        {
            const AZ::Matrix3x3 c = ChangeOfBasisMatrix(space);
            const AZ::Matrix3x3 m = AZ::Matrix3x3::CreateFromQuaternion(q);
            const AZ::Matrix3x3 me = c * m * c.GetTranspose();
            return QuaternionFromPossiblyImproper(me);
        }

        //! Inverse of RotationToEngine (engine rotation -> O3DE rotation). C is orthogonal so
        //! C^-1 == C^T; the inverse similarity is C^T * M_engine * C.
        static AZ::Quaternion RotationFromEngine(EngineSpace space, const AZ::Quaternion& q)
        {
            const AZ::Matrix3x3 c = ChangeOfBasisMatrix(space);
            const AZ::Matrix3x3 me = AZ::Matrix3x3::CreateFromQuaternion(q);
            const AZ::Matrix3x3 m = c.GetTranspose() * me * c;
            return QuaternionFromPossiblyImproper(m);
        }

        //! Whole O3DE transform (rotation + translation) -> engine transform. Scale is
        //! 1:1 (plan section 5) and left to the caller's uniform-scale handling.
        static void TransformToEngine(
            EngineSpace space, const AZ::Transform& o3de, AZ::Vector3& outPos, AZ::Quaternion& outRot)
        {
            outPos = PositionToEngine(space, o3de.GetTranslation());
            outRot = RotationToEngine(space, o3de.GetRotation());
        }

        //! Engine transform -> whole O3DE transform (uniform scale applied by caller).
        static AZ::Transform TransformFromEngine(
            EngineSpace space, const AZ::Vector3& enginePos, const AZ::Quaternion& engineRot)
        {
            return AZ::Transform::CreateFromQuaternionAndTranslation(
                RotationFromEngine(space, engineRot), PositionFromEngine(space, enginePos));
        }

        //! O3DE transform -> engine "raw 12" layout (basis 3x3 as three row-major rows, then the
        //! origin x,y,z), i.e. the exact byte layout of a Godot Transform3D. This is the single
        //! home for that conversion (plan §5, "one place, pure math"); backends fill the engine's
        //! transform Variant from out12 without reimplementing the basis change.
        //!
        //! Done as one 3x3 similarity of the rotation basis (C * R * C^T) plus C applied to the
        //! origin, so the basis columns (right/up/forward) stay consistent across the axis remap.
        static void TransformToEngineRaw12(EngineSpace space, const AZ::Transform& o3de, float out12[12])
        {
            const AZ::Matrix3x3 c = ChangeOfBasisMatrix(space);
            const AZ::Matrix3x3 rEngine = c * AZ::Matrix3x3::CreateFromQuaternion(o3de.GetRotation()) * c.GetTranspose();
            for (int row = 0; row < 3; ++row)
            {
                const AZ::Vector3 r = rEngine.GetRow(row);
                out12[row * 3 + 0] = r.GetX();
                out12[row * 3 + 1] = r.GetY();
                out12[row * 3 + 2] = r.GetZ();
            }
            const AZ::Vector3 pos = c * o3de.GetTranslation();
            out12[9] = pos.GetX();
            out12[10] = pos.GetY();
            out12[11] = pos.GetZ();
        }

        //! Inverse of TransformToEngineRaw12: engine "raw 12" -> O3DE transform.
        static AZ::Transform TransformFromEngineRaw12(EngineSpace space, const float in12[12])
        {
            const AZ::Matrix3x3 c = ChangeOfBasisMatrix(space);
            AZ::Matrix3x3 rEngine = AZ::Matrix3x3::CreateIdentity();
            for (int row = 0; row < 3; ++row)
            {
                rEngine.SetRow(row, AZ::Vector3(in12[row * 3 + 0], in12[row * 3 + 1], in12[row * 3 + 2]));
            }
            const AZ::Matrix3x3 rO3de = c.GetTranspose() * rEngine * c;
            const AZ::Vector3 pos = c.GetTranspose() * AZ::Vector3(in12[9], in12[10], in12[11]);
            return AZ::Transform::CreateFromQuaternionAndTranslation(
                QuaternionFromPossiblyImproper(rO3de), pos);
        }

        //! Engine world AABB -> O3DE world AABB. An axis-aligned box is no longer axis-aligned
        //! after the basis change (the +/-90 deg rotation about X tilts it), so we transform all
        //! eight corners and rebuild the enclosing box. Pure static math, exercised by the two
        //! engines' end-to-end picking acceptance (plan §5, no separate test target).
        static AZ::Aabb ConvertAabb(EngineSpace space, const AZ::Aabb& engineAabb)
        {
            if (!engineAabb.IsValid())
            {
                return AZ::Aabb::CreateNull();
            }

            const AZ::Vector3 lo = engineAabb.GetMin();
            const AZ::Vector3 hi = engineAabb.GetMax();

            AZ::Aabb out = AZ::Aabb::CreateNull();
            for (int corner = 0; corner < 8; ++corner)
            {
                const AZ::Vector3 enginePoint(
                    (corner & 1) ? hi.GetX() : lo.GetX(),
                    (corner & 2) ? hi.GetY() : lo.GetY(),
                    (corner & 4) ? hi.GetZ() : lo.GetZ());
                out.AddPoint(PositionFromEngine(space, enginePoint));
            }
            return out;
        }

    private:
        //! Change-of-basis matrix C that maps an O3DE axis vector to the engine axis vector, i.e.
        //! C applied to a column vector performs the same remap as PositionToEngine:
        //!   Godot:  [x, z, -y]  (rotation about X, det +1)
        //!   rbfx:   [x, z,  y]  (reflection swapping Y/Z, det -1 = handedness flip)
        //! Row r of C is the O3DE-space direction that becomes engine axis r.
        static AZ::Matrix3x3 ChangeOfBasisMatrix(EngineSpace space)
        {
            AZ::Matrix3x3 c = AZ::Matrix3x3::CreateZero();
            c.SetElement(0, 0, 1.0f);                                     // x' =  x
            c.SetElement(1, 2, 1.0f);                                     // y' =  z
            c.SetElement(2, 1, (space == EngineSpace::Godot) ? -1.0f : 1.0f); // z' = -y (Godot) / y (rbfx)
            return c;
        }

        //! Extract a unit quaternion from an orthogonal matrix. C*M*C^T is always a proper rotation
        //! (det = det(C)^2 * det(M) = +1) even when C is a reflection, so direct extraction is safe.
        static AZ::Quaternion QuaternionFromPossiblyImproper(const AZ::Matrix3x3& m)
        {
            return AZ::Quaternion::CreateFromMatrix3x3(m).GetNormalized();
        }
    };
} // namespace CrossEngineEditor
