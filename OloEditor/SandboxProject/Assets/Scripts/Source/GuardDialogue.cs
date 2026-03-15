using OloEngine;

public class GuardDialogue : Entity
{
    public float triggerDistance = 3f;

    private Entity m_Player;
    private bool m_HasStartedDialogue;

    public override void OnCreate()
    {
        m_Player = FindEntityByName("Player");
    }

    public override void OnUpdate(float dt)
    {
        if (m_Player == null)
            return;

        var dialogue = GetComponent<DialogueComponent>();
        if (dialogue == null)
            return;

        if (dialogue.IsActive)
            return;

        var npcTransform = GetComponent<TransformComponent>();
        if (npcTransform == null)
            return;

        var playerTransform = m_Player.GetComponent<TransformComponent>();
        if (playerTransform == null)
            return;

        var npcPos = npcTransform.Translation;
        var playerPos = playerTransform.Translation;
        float dist = Vector3.Distance(npcPos, playerPos);

        if (dist < triggerDistance)
        {
            if (!m_HasStartedDialogue)
            {
                dialogue.StartDialogue();
                m_HasStartedDialogue = true;
            }
        }
        else
        {
            // Player exited range — allow re-triggering
            m_HasStartedDialogue = false;
        }
    }
}
