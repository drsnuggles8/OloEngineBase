# A pack round trip goes through the builder and the runtime, then through the game

**Rule.** A test that claims an asset survives the packed path builds its pack with
`AssetPackBuilder::BuildFromRegistry` and reads it back through a `RuntimeAssetManager`. Calling
a serializer's `SerializeToAssetPack` and `DeserializeFromAssetPack` yourself proves the record
format and nothing else: it skips the builder, which places the records and writes the index,
and the dispatcher, which positions the stream before a serializer reads. Then package the game
with Build Game and run `OloRuntime` on it. That last step is what found every bug below.

How to write one (`AssetPackBuilderTest.EveryAssetLoadsBackFromTheOffsetTheBuilderRecorded`):

1. Mount a throwaway project, import the files through its `EditorAssetManager`, and call
   `SerializeAssetRegistry()` before building. The builder's own manager starts from that file;
   a file it finds unregistered it imports under a handle of its own, and your handles then load
   as placeholders.
2. Assert the layout as well as the loads: the first record starts right after the header, the
   index and the script module, and the records tile the file to its last byte.
3. Read at least one record first on a fresh stream, and check each loaded asset is its own (a
   name, a key), not merely "not a placeholder".

**Why.** The showcase dog (#1533) shipped bald. Its packed-path test went through the two groom
serializers directly and set the stream position itself, so it passed through four bugs on the
real path:

- The builder sized the index with `sizeof(AssetPackFile::AssetInfo)`, which is 32 bytes in
  memory, but wrote 28 bytes per entry. Every offset in every pack ran 4 bytes per entry late.
  Records smaller than the shift were read out of their neighbours.
- Six serializers read from wherever the stream stood, and on a fresh stream that is the pack's
  header: groom, groom binding, static mesh, lightmap, light-probe volume, volume.
- `BuildFromRegistry` hung in any process without a free task worker, which is why no test had
  ever called it.
- A cooked BC7 texture could not be read back on the CPU, so the coat lost its colour map.

The planted-downgrade controls are in the commits: with the old index sizing the first record
lands at byte 180 instead of 164, and without the dispatcher seek the lightmap reads
`0x504C4F4F`, the pack's own magic. See also
[packaged-game-content-by-path.md](packaged-game-content-by-path.md) for what a scene opens by
path, and [substituted-seams-compound.md](substituted-seams-compound.md).
