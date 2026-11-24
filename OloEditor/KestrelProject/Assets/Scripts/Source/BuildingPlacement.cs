using System;
using System.Collections.Generic;
using OloEngine;

namespace Kestrel
{
    internal sealed class BuildingDefinition
    {
        public string Name { get; }
        public Vector3 Size { get; }
        public long Cost { get; }
        public Vector4 Color { get; }

        public BuildingDefinition(string name, Vector3 size, long cost, Vector4 color)
        {
            Name = name;
            Size = size;
            Cost = cost;
            Color = color;
        }
    }

    internal sealed class BuildingSite
    {
        private readonly List<BuildingPlacement> m_Placements = new List<BuildingPlacement>();
        private readonly Func<Vector2, Vector3?> m_SampleNormal;
        public long Balance { get; private set; }

        public BuildingSite(long balance, Func<Vector2, Vector3?> sampleNormal)
        {
            Balance = balance;
            m_SampleNormal = sampleNormal;
        }

        public BuildValidity Check(BuildFootprint footprint, long cost)
        {
            const float cos18Degrees = 0.9510565f;
            BuildValidity slope = footprint.CheckSlope(m_SampleNormal, cos18Degrees);
            if (slope != BuildValidity.Valid)
                return slope;

            foreach (BuildingPlacement placement in m_Placements)
            {
                if (footprint.Overlaps(placement.Footprint))
                    return BuildValidity.Occupied;
            }
            return cost <= Balance ? BuildValidity.Valid : BuildValidity.Unaffordable;
        }

        public void Record(BuildingPlacement placement, long cost)
        {
            m_Placements.Add(placement);
            Balance -= cost;
        }

        public void Remove(BuildingPlacement placement, long cost)
        {
            m_Placements.Remove(placement);
            Balance += cost;
        }
    }

    internal sealed class BuildingPlacement : IBuildCommand
    {
        private readonly BuildingSite m_Site;
        private readonly BuildingDefinition m_Definition;
        private readonly Action<Vector3, Vector3, bool> m_ShowGhost;
        private Entity m_Entity;
        private bool m_Applied;

        public BuildFootprint Footprint { get; }
        private Vector3 Center { get; }

        public BuildingPlacement(BuildingSite site, BuildingDefinition definition, Vector3 groundPoint,
                                 Action<Vector3, Vector3, bool> showGhost)
        {
            m_Site = site;
            m_Definition = definition;
            m_ShowGhost = showGhost;
            Center = new Vector3(groundPoint.X, groundPoint.Y + definition.Size.Y * 0.5f, groundPoint.Z);
            Footprint = new BuildFootprint(new Vector2(groundPoint.X, groundPoint.Z),
                                          new Vector2(definition.Size.X * 0.5f, definition.Size.Z * 0.5f));
        }

        public BuildPreview Preview() => new BuildPreview(m_Site.Check(Footprint, m_Definition.Cost), m_Definition.Cost);

        public void ShowGhost(BuildPreview preview) => m_ShowGhost(Center, m_Definition.Size, preview.CanCommit);

        public bool TryCommit(long cost)
        {
            if (m_Applied || m_Site.Check(Footprint, cost) != BuildValidity.Valid)
                return false;

            Entity entity = Scene.CreateBuildBox(m_Definition.Name, Center, m_Definition.Size,
                                                 m_Definition.Color, m_Entity?.ID ?? 0);
            if (entity == null)
                return false;

            m_Entity = entity;
            m_Applied = true;
            m_Site.Record(this, cost);
            return true;
        }

        public bool TryRevert(long cost)
        {
            if (!m_Applied)
                return false;

            m_Entity.Destroy();
            m_Applied = false;
            m_Site.Remove(this, cost);
            return true;
        }
    }
}
