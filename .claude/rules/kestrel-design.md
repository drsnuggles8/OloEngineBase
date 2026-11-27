---
paths:
  - "OloEditor/KestrelProject/**"
---

# KESTREL game design

This file is part of KESTREL, the game. Before changing what a player can do, see, hear or decide, read
[OloEditor/KestrelProject/docs/AGENT_GUIDE.md](../../OloEditor/KestrelProject/docs/AGENT_GUIDE.md). It
classifies the task (gameplay, engine-only or mechanical doc edit), says which design documents and
decision IDs to read, and says what the HANDOVER's *Design context* and the PR's *Design validation*
sections must contain.

- Never change an approved decision, resolve an open question or promote a tunable to a requirement
  without the owner's approval. Propose a revision with evidence instead.
- Agent runs are technical verification, never human playtesting.
- Never edit an archived snapshot (a file listed in `OloEditor/KestrelProject/docs/archive/SHA256SUMS`).
