using OloEngine;

public class GuardDialogue : Entity
{
    private Entity m_Player;

    public override void OnCreate()
    {
        m_Player = FindEntityByName("Player");
    }

    public override void OnUpdate(float ts)
    {
        var dialogue = GetComponent<DialogueComponent>();
        if (dialogue.IsActive)
            return;

        var npcPos = GetComponent<TransformComponent>().Translation;
        var playerPos = m_Player.GetComponent<TransformComponent>().Translation;
        float dist = Vector3.Distance(npcPos, playerPos);

        if (dist < 3.0f)
            dialogue.StartDialogue();
    }
}
