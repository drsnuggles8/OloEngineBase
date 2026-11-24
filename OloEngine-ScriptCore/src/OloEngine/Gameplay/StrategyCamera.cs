using System;
using System.Runtime.InteropServices;

namespace OloEngine
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct StrategyCameraState
    {
        internal Vector2 Focus, TargetFocus;
        internal float Height, Yaw, TargetYaw, Pitch, TargetPitch, LogDistance, TargetLogDistance;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct StrategyCameraIntent
    {
        internal Vector2 Pan, Drag, Orbit;
        internal float LogZoom;
    }

    /// <summary>World XZ focus limits, eye-to-focus distances, and the vertical ground search range, in metres.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct StrategyCameraSettings
    {
        public Vector2 MinFocus, MaxFocus;
        public float MinDistance, MaxDistance, GroundBottom, GroundTop;
    }

    /// <summary>A controller for an unparented perspective camera in a 3D physics scene.</summary>
    public sealed class StrategyCamera
    {
        private StrategyCameraState m_State;
        private StrategyCameraSettings m_Settings;
        private Vector2 m_PreviousCursor;
        private bool m_HadPointer, m_WasOrbiting, m_WasDragging;
        private double m_PreviousWheel;

        public StrategyCamera(StrategyCameraSettings settings, Vector2 focus, float height,
                              float yawRadians, float pitchRadians, float distance)
        {
            if (!Finite(distance) || !Finite(height) || !Finite(focus.X) || !Finite(focus.Y) ||
                !Finite(yawRadians) || !Finite(pitchRadians) || distance <= 0)
                throw new ArgumentOutOfRangeException(nameof(distance), "Camera pose must be finite with a positive distance.");
            m_Settings = settings;
            m_PreviousWheel = Input.GetMouseScrollYTotal();
            float logDistance = (float)Math.Log(distance);
            m_State = new StrategyCameraState
            {
                Focus = focus,
                TargetFocus = focus,
                Height = height,
                Yaw = yawRadians,
                TargetYaw = yawRadians,
                Pitch = pitchRadians,
                TargetPitch = pitchRadians,
                LogDistance = logDistance,
                TargetLogDistance = logDistance
            };
        }

        /// <summary>Advance the camera. Pan is a right/forward rate. Drag (viewport fractions),
        /// orbit (radians) and zoom (natural log of the distance multiplier) are displacements per call.</summary>
        public bool Step(Entity camera, float dt, Vector2 pan, Vector2 drag, Vector2 orbit, float logZoom)
        {
            if (camera == null)
                throw new ArgumentNullException(nameof(camera));
            var intent = new StrategyCameraIntent { Pan = pan, Drag = drag, Orbit = orbit, LogZoom = logZoom };
            return InternalCalls.StrategyCamera_Step(camera.ID, ref m_State, ref m_Settings, dt, ref intent);
        }

        public static bool IsOverViewport(Vector2 cursor, Vector2 viewport) =>
            cursor.X >= 0 && cursor.Y >= 0 && cursor.X < viewport.X && cursor.Y < viewport.Y;

        /// <summary>Drive the camera from WASD or the arrows, viewport edges, middle drag,
        /// right drag, wheel, Q/E and Page Up/Down. Keyboard and mouse controls require the pointer over Play.</summary>
        public bool StepFromInput(Entity camera, float dt, Vector2 cursor, Vector2 viewport)
        {
            bool inside = IsOverViewport(cursor, viewport);
            bool orbiting = inside && Input.IsMouseButtonDown(1);
            bool dragging = inside && Input.IsMouseButtonDown(2);
            Vector2 pan = Vector2.Zero, drag = Vector2.Zero, orbit = Vector2.Zero;
            double wheel = Input.GetMouseScrollYTotal();
            float zoom = 0;
            if (inside)
            {
                const float radiansPerPixel = 0.006f;
                float dx = cursor.X - m_PreviousCursor.X;
                float dy = cursor.Y - m_PreviousCursor.Y;
                // Drop pointer jumps of more than 90 degrees' worth of displacement.
                bool consecutive = m_HadPointer && Math.Abs(dx) < 260 && Math.Abs(dy) < 260;
                if (consecutive && orbiting && m_WasOrbiting)
                    orbit = new Vector2(-dx * radiansPerPixel, -dy * radiansPerPixel);
                if (consecutive && dragging && m_WasDragging)
                    drag = new Vector2(-dx / viewport.Y, dy / viewport.Y);

                pan = new Vector2(Axis(KeyCode.D, KeyCode.Right, KeyCode.A, KeyCode.Left),
                                  Axis(KeyCode.W, KeyCode.Up, KeyCode.S, KeyCode.Down));
                if (!orbiting && !dragging)
                {
                    pan.X += Edge(cursor.X, viewport.X);
                    pan.Y -= Edge(cursor.Y, viewport.Y);
                }
                orbit.X += dt * (Down(KeyCode.Q) - Down(KeyCode.E));
                zoom = dt * (Down(KeyCode.PageDown) - Down(KeyCode.PageUp)) * 2.5f;
                if (m_HadPointer)
                    zoom -= (float)(wheel - m_PreviousWheel) * 0.22f;
            }

            m_PreviousWheel = wheel;
            m_PreviousCursor = cursor;
            m_HadPointer = inside;
            m_WasOrbiting = orbiting;
            m_WasDragging = dragging;
            return Step(camera, dt, pan, drag, orbit, zoom);
        }

        private static float Edge(float position, float length)
        {
            float margin = Math.Min(24, length * 0.2f);
            return Math.Max(0, (position - (length - margin)) / margin) -
                   Math.Max(0, (margin - position) / margin);
        }

        private static int Down(KeyCode key) => Input.IsKeyDown(key) ? 1 : 0;
        private static int Axis(KeyCode positive, KeyCode positiveAlt, KeyCode negative, KeyCode negativeAlt) =>
            Math.Max(Down(positive), Down(positiveAlt)) - Math.Max(Down(negative), Down(negativeAlt));
        private static bool Finite(float value) => !float.IsNaN(value) && !float.IsInfinity(value);
    }
}
