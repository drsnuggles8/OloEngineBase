using OloEngine;

namespace Kestrel
{
    public class StartupSmoke : Entity
    {
        void OnCreate()
        {
            Debug.Log("KESTREL startup script ran.");
        }
    }
}
