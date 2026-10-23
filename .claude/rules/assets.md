---
paths:
  - "OloEngine/src/OloEngine/Asset/**"
  - "OloEngine/src/OloEngine/Serialization/**"
---

# Assets and serialization

- A new asset type needs a loader, registration, YAML serialization and hot-reload handling.
- Validate every float read from YAML, JSON or the network with `std::isfinite`.
- Asset loads belong to the game thread; a worker touching the frame's queue loses the Vulkan
  device ([asset-loads-belong-to-the-game-thread.md](../../docs/agent-rules/asset-loads-belong-to-the-game-thread.md)).
- A serialization change is verified on `{scene YAML, asset pack, save-game}`, plus a prior on-disk
  version when the format is versioned. There are no external users: break formats freely and
  delete the old path rather than keeping legacy readers.
