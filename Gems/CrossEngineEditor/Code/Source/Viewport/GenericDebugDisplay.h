/*
 * Copyright (c) Cross-Engine Editor Project.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

//! Engine-agnostic implementation of AzFramework::DebugDisplayRequests (plan §3.1).
//!
//! Every high-level draw call (~70 methods used by manipulators/component modes) is
//! tessellated here, on the CPU, into two immediate-mode vertex batches: a line-list
//! and a triangle-list. At the end of a frame the batches are handed to the active
//! backend through ISceneRenderer's 3 primitives (SubmitLines / SubmitTriangles /
//! SetDepthTest). This keeps the per-engine cost tiny while every editor overlay
//! (gizmos, grid, selection) is drawn identically across engines.
//!
//! The tessellation math (arc/circle generation, world-space transform stack) mirrors
//! O3DE's AtomDebugDisplayViewportInterface so curve orientation matches the original
//! editor exactly; the few shapes Atom hid inside AuxGeom (cone/cylinder/ball/disk)
//! are tessellated here with standard formulas.

#include <BackendAPI/BackendTypes.h>

#include <AzFramework/Entity/EntityDebugDisplayBus.h>

#include <AzCore/Math/Color.h>
#include <AzCore/Math/Matrix3x4.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/containers/span.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>

namespace CrossEngineEditor
{
    class ISceneRenderer;

    //! A world-space text label queued during a frame. The GL renderer only draws lines and
    //! triangles, so text is collected here and painted by the Qt viewport (QPainter) after the
    //! geometry flush - both Blender and Unreal show numeric drag readouts as 2D overlay text.
    struct DebugTextLabel
    {
        AZ::Vector3 m_worldPosition;
        AZ::Color m_color;
        float m_size;
        bool m_center;
        AZStd::string m_text;
    };

    //! Blender-inspired visual tuning shared by every overlay draw. Kept as a small,
    //! central struct so the whole editor gets a consistent, modern gizmo look.
    struct DebugDisplayStyle
    {
        //! Segments used to approximate a full circle. Higher = smoother (Blender-grade).
        int m_circleSegments = 64;
        //! Segments around the base of cones/cylinders/spheres.
        int m_capSegments = 32;

        //! Remap the engine's hard-coded manipulator colors to Blender's default theme
        //! (source: userdef_default_theme.c). The manipulator geometry (arrow / ring /
        //! box) already matches Blender; only the axis + highlight colors differ, and
        //! every draw call funnels through SetColor, so a single remap here re-skins all
        //! gizmos for free without forking the engine's manipulator code.
        bool m_blenderGizmoPalette = true;
        //! Blender theme.xaxis / yaxis / zaxis.
        AZ::Color m_axisXColor = AZ::Color(1.0f, 0.200f, 0.322f, 1.0f);  // #ff3352
        AZ::Color m_axisYColor = AZ::Color(0.545f, 0.863f, 0.0f, 1.0f);  // #8bdc00
        AZ::Color m_axisZColor = AZ::Color(0.157f, 0.565f, 1.0f, 1.0f);  // #2890ff
        //! Blender theme.gizmo_hi (mouse-over / selected highlight).
        AZ::Color m_highlightColor = AZ::Color(1.0f, 1.0f, 1.0f, 1.0f);  // #ffffff
    };

    class GenericDebugDisplay final
        : public AzFramework::DebugDisplayRequests
    {
    public:
        GenericDebugDisplay();

        //! Discard all batched geometry and reset render state for a new frame.
        void ClearFrame();

        //! Flush the accumulated line/triangle batches to the backend. Split by depth
        //! state so gizmos submitted with DepthTestOff stay drawn-in-front (Blender look).
        void Flush(ISceneRenderer& renderer);

        DebugDisplayStyle& Style() { return m_style; }

        // AzFramework::DebugDisplayRequests (only the subset editor overlays actually use;
        // the rest keep the base-class no-op default) ...
        void SetColor(const AZ::Color& color) override;
        void SetAlpha(float a) override;
        void SetLineWidth(float width) override;
        float GetLineWidth() override;

        void DepthTestOn() override;
        void DepthTestOff() override;
        void CullOn() override {}
        void CullOff() override {}

        void PushMatrix(const AZ::Transform& tm) override;
        void PopMatrix() override;
        void PushPremultipliedMatrix(const AZ::Matrix3x4& matrix) override;
        AZ::Matrix3x4 PopPremultipliedMatrix() override;

        void DrawLine(const AZ::Vector3& p1, const AZ::Vector3& p2) override;
        void DrawLine(const AZ::Vector3& p1, const AZ::Vector3& p2, const AZ::Vector4& col1, const AZ::Vector4& col2) override;
        void DrawLines(const AZStd::vector<AZ::Vector3>& lines, const AZ::Color& color) override;
        using AzFramework::DebugDisplayRequests::DrawPolyLine;
        void DrawPolyLine(AZStd::span<const AZ::Vector3> points, bool cycled) override;

        using AzFramework::DebugDisplayRequests::DrawQuad;
        void DrawQuad(const AZ::Vector3& p1, const AZ::Vector3& p2, const AZ::Vector3& p3, const AZ::Vector3& p4) override;
        void DrawTri(const AZ::Vector3& p1, const AZ::Vector3& p2, const AZ::Vector3& p3) override;
        void DrawTriangles(const AZStd::vector<AZ::Vector3>& vertices, const AZ::Color& color) override;
        void DrawTrianglesIndexed(
            const AZStd::vector<AZ::Vector3>& vertices, const AZStd::vector<AZ::u32>& indices, const AZ::Color& color) override;

        void DrawWireBox(const AZ::Vector3& min, const AZ::Vector3& max) override;
        void DrawSolidBox(const AZ::Vector3& min, const AZ::Vector3& max) override;
        void DrawSolidOBB(
            const AZ::Vector3& center, const AZ::Vector3& axisX, const AZ::Vector3& axisY, const AZ::Vector3& axisZ,
            const AZ::Vector3& halfExtents) override;

        void DrawPoint(const AZ::Vector3& p, int nSize) override;
        void DrawCircle(const AZ::Vector3& pos, float radius, int nUnchangedAxis) override;
        void DrawHalfDottedCircle(const AZ::Vector3& pos, float radius, const AZ::Vector3& viewPos, int nUnchangedAxis) override;
        using AzFramework::DebugDisplayRequests::DrawArc;
        void DrawArc(
            const AZ::Vector3& pos, float radius, float startAngleDegrees, float sweepAngleDegrees, float angularStepDegrees,
            int referenceAxis) override;

        void DrawWireCone(const AZ::Vector3& pos, const AZ::Vector3& dir, float radius, float height) override;
        void DrawSolidCone(const AZ::Vector3& pos, const AZ::Vector3& dir, float radius, float height, bool drawShaded) override;
        void DrawWireCylinder(const AZ::Vector3& center, const AZ::Vector3& axis, float radius, float height) override;
        void DrawSolidCylinder(const AZ::Vector3& center, const AZ::Vector3& axis, float radius, float height, bool drawShaded) override;

        using AzFramework::DebugDisplayRequests::DrawWireSphere;
        void DrawWireSphere(const AZ::Vector3& pos, float radius) override;
        void DrawBall(const AZ::Vector3& pos, float radius, bool drawShaded) override;
        void DrawWireDisk(const AZ::Vector3& pos, const AZ::Vector3& dir, float radius) override;
        void DrawDisk(const AZ::Vector3& pos, const AZ::Vector3& dir, float radius, bool drawShaded) override;

        void DrawArrow(const AZ::Vector3& src, const AZ::Vector3& trg, float headScale, bool twoSided) override;

        using AzFramework::DebugDisplayRequests::DrawTextLabel;
        void DrawTextLabel(
            const AZ::Vector3& pos, float size, const char* text, bool bCenter, int srcOffsetX, int srcOffsetY) override;

        //! World-space text labels collected this frame (painted by the Qt viewport after flush).
        const AZStd::vector<DebugTextLabel>& TextLabels() const { return m_textLabels; }

    private:
        enum class Axis
        {
            X = 0,
            Y = 1,
            Z = 2,
        };

        const AZ::Matrix3x4& CurrentTransform() const { return m_transformStack.back(); }
        AZ::Vector3 ToWorld(const AZ::Vector3& v) const { return CurrentTransform() * v; }
        AZ::Vector3 ToWorldVector(const AZ::Vector3& v) const { return CurrentTransform().Multiply3x3(v); }

        AZ::Color CurrentColor() const;

        //! Push a world-space line segment (already transformed) into the line batch.
        void EmitLine(const AZ::Vector3& worldStart, const AZ::Vector3& worldEnd, const AZ::Color& color);
        //! Push a world-space triangle (already transformed) into the triangle batch.
        void EmitTriangle(
            const AZ::Vector3& w0, const AZ::Vector3& w1, const AZ::Vector3& w2, const AZ::Color& color);

        //! Two orthonormal basis vectors spanning the plane perpendicular to a unit axis.
        static void BasisFromAxis(const AZ::Vector3& axis, AZ::Vector3& outU, AZ::Vector3& outV);
        //! Generate a ring of world-space points around a center on an arbitrary axis.
        void BuildRing(
            const AZ::Vector3& worldCenter, const AZ::Vector3& worldAxis, float radius, int segments,
            AZStd::vector<AZ::Vector3>& outRing) const;

        DebugDisplayStyle m_style;

        AZ::Color m_color = AZ::Colors::White;
        float m_lineWidth = 1.0f;
        bool m_depthTest = true;

        AZStd::vector<AZ::Matrix3x4> m_transformStack;

        // Batches accumulated for the current frame. Depth-tested and drawn-in-front
        // geometry is kept apart so the backend can render gizmos over the scene.
        AZStd::vector<DebugVertex> m_depthLines;
        AZStd::vector<DebugVertex> m_depthTriangles;
        AZStd::vector<DebugVertex> m_overlayLines;
        AZStd::vector<DebugVertex> m_overlayTriangles;
        AZStd::vector<DebugTextLabel> m_textLabels;
    };
} // namespace CrossEngineEditor
