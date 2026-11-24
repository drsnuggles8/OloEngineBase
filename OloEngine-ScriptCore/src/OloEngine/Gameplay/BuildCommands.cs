using System;
using System.Collections.Generic;

namespace OloEngine
{
    public enum BuildValidity
    {
        NoGround,
        Valid,
        TooSteep,
        Occupied,
        Unaffordable
    }

    public readonly struct BuildPreview
    {
        public readonly BuildValidity Validity;
        public readonly long Cost;
        public bool CanCommit => Validity == BuildValidity.Valid;

        public BuildPreview(BuildValidity validity, long cost)
        {
            if (cost < 0)
                throw new ArgumentOutOfRangeException(nameof(cost));
            Validity = validity;
            Cost = cost;
        }
    }

    /// <summary>A prepared edit at a fixed location. A failed operation must leave the world unchanged.</summary>
    public interface IBuildCommand
    {
        BuildPreview Preview();

        /// <summary>Show or update the tool's ghost using the current quote. Own the rendering
        /// in the tool so it can reuse the ghost and clear it on cancel.</summary>
        void ShowGhost(BuildPreview preview);

        /// <summary>Apply the edit at the quoted cost. Redo receives the original successful quote.</summary>
        bool TryCommit(long cost);

        /// <summary>Revert the edit and refund the original quote.</summary>
        bool TryRevert(long cost);
    }

    /// <summary>Own one history per playing scene. Previewing or cancelling leaves history untouched.</summary>
    public sealed class BuildHistory
    {
        private readonly Stack<(IBuildCommand Command, long Cost)> m_Undo =
            new Stack<(IBuildCommand, long)>();
        private readonly Stack<(IBuildCommand Command, long Cost)> m_Redo =
            new Stack<(IBuildCommand, long)>();

        public int UndoCount => m_Undo.Count;
        public int RedoCount => m_Redo.Count;

        public bool TryConfirm(IBuildCommand command)
        {
            if (command == null)
                throw new ArgumentNullException(nameof(command));
            BuildPreview quote = command.Preview();
            if (!quote.CanCommit || !command.TryCommit(quote.Cost))
                return false;

            m_Undo.Push((command, quote.Cost));
            m_Redo.Clear();
            return true;
        }

        public bool TryUndo()
        {
            if (m_Undo.Count == 0)
                return false;
            var edit = m_Undo.Peek();
            if (!edit.Command.TryRevert(edit.Cost))
                return false;
            m_Redo.Push(m_Undo.Pop());
            return true;
        }

        public bool TryRedo()
        {
            if (m_Redo.Count == 0)
                return false;
            var edit = m_Redo.Peek();
            if (!edit.Command.TryCommit(edit.Cost))
                return false;
            m_Undo.Push(m_Redo.Pop());
            return true;
        }
    }

    /// <summary>One axis-aligned piece of a build footprint on the XZ plane.</summary>
    public readonly struct BuildFootprint
    {
        public readonly Vector2 Center;
        public readonly Vector2 HalfSize;

        public BuildFootprint(Vector2 center, Vector2 halfSize)
        {
            if (!Finite(center.X) || !Finite(center.Y) ||
                !Finite(halfSize.X) || !Finite(halfSize.Y) || halfSize.X <= 0 || halfSize.Y <= 0)
                throw new ArgumentOutOfRangeException(nameof(halfSize));
            Center = center;
            HalfSize = halfSize;
        }

        public bool Overlaps(BuildFootprint other)
        {
            return Math.Abs(Center.X - other.Center.X) < HalfSize.X + other.HalfSize.X &&
                   Math.Abs(Center.Y - other.Center.Y) < HalfSize.Y + other.HalfSize.Y;
        }

        /// <summary>Validate the centre and four corners against a terrain normal sampler.</summary>
        public BuildValidity CheckSlope(Func<Vector2, Vector3?> sampleNormal, float minimumUpDot)
        {
            BuildValidity center = CheckNormal(sampleNormal(Center), minimumUpDot);
            if (center != BuildValidity.Valid)
                return center;

            for (int x = -1; x <= 1; x += 2)
            {
                for (int z = -1; z <= 1; z += 2)
                {
                    var point = new Vector2(Center.X + x * HalfSize.X, Center.Y + z * HalfSize.Y);
                    BuildValidity corner = CheckNormal(sampleNormal(point), minimumUpDot);
                    if (corner != BuildValidity.Valid)
                        return corner;
                }
            }
            return BuildValidity.Valid;
        }

        private static BuildValidity CheckNormal(Vector3? normal, float minimumUpDot)
        {
            if (!normal.HasValue || !Finite(normal.Value.Y))
                return BuildValidity.NoGround;
            return normal.Value.Y >= minimumUpDot ? BuildValidity.Valid : BuildValidity.TooSteep;
        }

        private static bool Finite(float value) => !float.IsNaN(value) && !float.IsInfinity(value);
    }
}
