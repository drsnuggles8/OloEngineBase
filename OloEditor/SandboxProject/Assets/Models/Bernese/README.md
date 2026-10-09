# Bernese

Approved full-fur OloEngine model. Keep the glTF, external buffer, maps, rig and dedicated eye profile together; its native groom and binding are under `Assets/Grooms/Bernese` and its scene is `Assets/Scenes/Bernese.olo`.

Use Git LFS to retrieve the large assets. Rebuild into a new directory with:

```powershell
python tools/dog-authoring/rebuild.py --breed Bernese --output C:/temp/bernese-rebuild --verify-shipped
```

The command uses versioned intermediate meshes, deterministic refinement scripts and the complete authored Alembic source. It invokes the production native importer/binding cooker and leaves the approved assets intact. Build `OloEngine-Tests` in Release first, or use `--mesh-only` while editing. See `docs/guides/dog-breeds.md` for dependencies, source layout, verification and accepted galleries.
