using OloEngine;

namespace Kestrel
{
    public sealed class StrategyBuildMode : Entity
    {
        private const float GroundBottom = -100;
        private const float GroundTop = 2000;
        private static readonly Vector3 s_Down = new Vector3(0, -1, 0);
        private static readonly BuildingDefinition[] s_Buildings =
        {
            new BuildingDefinition("Ticket office", new Vector3(10, 5, 8), 450, new Vector4(0.44f, 0.23f, 0.11f, 1)),
            new BuildingDefinition("Ski school", new Vector3(15, 7, 11), 800, new Vector4(0.17f, 0.38f, 0.60f, 1)),
            new BuildingDefinition("Restaurant", new Vector3(20, 8, 16), 1600, new Vector4(0.60f, 0.24f, 0.12f, 1)),
            new BuildingDefinition("Parking", new Vector3(32, 0.55f, 24), 600, new Vector4(0.17f, 0.19f, 0.21f, 1)),
            new BuildingDefinition("Hut", new Vector3(7, 4, 7), 250, new Vector4(0.35f, 0.26f, 0.15f, 1))
        };

        private readonly BuildHistory m_History = new BuildHistory();
        private StrategyCamera m_Camera;
        private BuildingSite m_Site;
        private Entity m_Terrain, m_Ghost;
        private UITextComponent m_Status;
        private int m_Selected = -1;
        private int m_PreviousControls;
        private bool m_LeftWasDown;
        private string m_LastStatus;

        private void OnCreate()
        {
            m_Terrain = FindEntityByName("Prototype Massif");
            float height = TryTerrainRay(new Vector3(0, GroundTop, 0), s_Down, GroundTop - GroundBottom,
                                         out RaycastHit start) ? start.Position.Y : 350;
            m_Camera = new StrategyCamera(new StrategyCameraSettings
            {
                MinFocus = new Vector2(-4990),
                MaxFocus = new Vector2(4990),
                MinDistance = 20,
                MaxDistance = 17000,
                GroundBottom = GroundBottom,
                GroundTop = GroundTop
            }, Vector2.Zero, height, 0, 1.0f, 600);
            m_Status = FindEntityByName("Build Status")?.GetComponent<UITextComponent>();
            m_Site = new BuildingSite(10000, SampleNormal);
        }

        private void OnUpdate(float dt)
        {
            Vector2 cursor = Input.GetMousePosition();
            Vector2 viewport = Input.GetWindowSize();
            if (viewport.X <= 0 || viewport.Y <= 0)
                return;

            bool overViewport = StrategyCamera.IsOverViewport(cursor, viewport);
            bool left = Input.IsMouseButtonDown(0);
            bool confirm = overViewport && left && !m_LeftWasDown &&
                           !Input.IsMouseButtonDown(1) && !Input.IsMouseButtonDown(2);
            m_LeftWasDown = left;
            UpdateToolAndHistory(overViewport);

            string detail = "1 Ticket office   2 Ski school   3 Restaurant   4 Parking   5 Hut";
            if (m_Selected >= 0)
                detail = PreviewAndConfirm(cursor, viewport, overViewport, confirm);
            else
                HideGhost();

            string status = $"Balance {m_Site.Balance:N0}   |   Undo {m_History.UndoCount}   Redo {m_History.RedoCount}\n{detail}";
            if (m_Status != null && status != m_LastStatus)
            {
                m_Status.Text = status;
                m_LastStatus = status;
            }

            m_Camera.StepFromInput(this, dt, cursor, viewport);
        }

        private void UpdateToolAndHistory(bool active)
        {
            int controls = 0;
            for (int i = 0; i < s_Buildings.Length; ++i)
            {
                if (Input.IsKeyDown((KeyCode)((int)KeyCode.D1 + i)))
                    controls |= 1 << i;
            }
            bool control = Input.IsKeyDown(KeyCode.LeftControl) || Input.IsKeyDown(KeyCode.RightControl);
            if (Input.IsKeyDown(KeyCode.Escape))
                controls |= 1 << 5;
            if (control && Input.IsKeyDown(KeyCode.Z))
                controls |= 1 << 6;
            if (control && Input.IsKeyDown(KeyCode.Y))
                controls |= 1 << 7;

            int pressed = controls & ~m_PreviousControls;
            m_PreviousControls = controls;
            if (!active)
                return;

            for (int i = 0; i < s_Buildings.Length; ++i)
            {
                if ((pressed & (1 << i)) != 0)
                    m_Selected = i;
            }
            if ((pressed & (1 << 5)) != 0)
                m_Selected = -1;
            if ((pressed & (1 << 6)) != 0)
                m_History.TryUndo();
            else if ((pressed & (1 << 7)) != 0)
                m_History.TryRedo();
        }

        private string PreviewAndConfirm(Vector2 cursor, Vector2 viewport, bool overViewport, bool confirm)
        {
            BuildingDefinition definition = s_Buildings[m_Selected];
            var uv = new Vector2(cursor.X / viewport.X, 1 - cursor.Y / viewport.Y);
            if (!overViewport || !Camera.RenderedScreenToWorldRay(uv, out Vector3 origin, out Vector3 direction) ||
                !TryTerrainRay(origin, direction, 40000, out RaycastHit hit))
            {
                HideGhost();
                return $"{definition.Name}   Cost {definition.Cost:N0}   |   Point at terrain";
            }

            var placement = new BuildingPlacement(m_Site, definition, hit.Position, ShowGhost);
            if (confirm && m_History.TryConfirm(placement))
            {
                HideGhost();
                return $"{definition.Name} placed   |   Paid {definition.Cost:N0}";
            }

            BuildPreview preview = placement.Preview();
            placement.ShowGhost(preview);
            string validity = preview.Validity switch
            {
                BuildValidity.Valid => "Valid",
                BuildValidity.TooSteep => "Too steep",
                BuildValidity.Occupied => "Overlaps a building",
                BuildValidity.Unaffordable => "Insufficient balance",
                _ => "Footprint leaves terrain"
            };
            return $"{definition.Name}   Cost {preview.Cost:N0}   |   {validity}";
        }

        private void ShowGhost(Vector3 center, Vector3 buildingSize, bool valid)
        {
            // A rejected footprint is raised so an existing building cannot
            // hide the feedback at the exact same site.
            var position = center;
            var size = buildingSize;
            if (!valid)
            {
                position.Y += 9.0f - buildingSize.Y * 0.5f;
                size.Y = 0.7f;
            }
            var color = valid ? new Vector4(0.04f, 0.92f, 0.29f, 1) : new Vector4(0.95f, 0.02f, 0.06f, 1);
            if (m_Ghost == null)
            {
                m_Ghost = Scene.CreateBuildBox("Placement preview", position, size, color, runtimeOwned: true);
                return;
            }

            m_Ghost.Translation = position;
            m_Ghost.GetComponent<TransformComponent>().Scale = size;
            m_Ghost.GetComponent<MaterialComponent>().AlbedoColor = color;
        }

        private void HideGhost()
        {
            Entity.Destroy(m_Ghost);
            m_Ghost = null;
        }

        private Vector3? SampleNormal(Vector2 point)
        {
            var origin = new Vector3(point.X, GroundTop, point.Y);
            return TryTerrainRay(origin, s_Down, GroundTop - GroundBottom, out RaycastHit hit) ? hit.Normal : (Vector3?)null;
        }

        private bool TryTerrainRay(Vector3 origin, Vector3 direction, float length, out RaycastHit hit) =>
            Physics.Raycast(origin, direction, length, out hit) && hit.EntityID == m_Terrain?.ID;
    }
}
