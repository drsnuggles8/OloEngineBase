# Resolve scene registry paths against the project

Resolve `AssetMetadata::FilePath` against the active project directory before
reading or writing a scene source file. Keep absolute paths intact and preserve
the supplied path when no project is mounted.

The editor runs from `OloEditor/`, while registry entries such as
`Assets/Scenes/Level.oloscene` are relative to the project. During #1540 packaging
verification, `SceneAssetSerializer` passed that entry directly to
`SceneSerializer`. Build Game then failed to load its registered scene and produced
no asset pack. The prefab serializer already had the required project resolution;
both serializers now use the same resolver.

`RuntimeAssetPackTest.SceneSourceReadAndWriteUseTheProjectDirectory` reads
independently authored YAML from a project outside the working directory, checks
its entity and handle, writes another project-relative source, and reads that file
through its absolute path. The real Build Game and cooked-runtime launch checks
exercise the registry and pack boundary as well.
