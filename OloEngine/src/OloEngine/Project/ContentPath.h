#pragma once

#include <filesystem>
#include <string_view>

namespace OloEngine
{
    // The ONE resolver for a stored, relative content path (issue #1496) — a
    // ModelComponent's FilePath, a foliage layer's MeshPath / AlbedoPath / leaf
    // maps, a terrain layer's textures, a texture's stored source path. Content
    // authored inside a project is spelled project-relative and starts with the
    // project's asset directory ("Assets/Models/Sponza/Sponza.gltf"); engine
    // content is spelled relative to the working directory
    // ("assets/models/Fox/Fox.gltf", under OloEditor/).
    //
    // The spelling picks exactly one base; the resolver never probes a second one.
    // Probing let one base shadow the other: on a case-insensitive filesystem
    // "assets/models/CesiumMan/CesiumMan.gltf" also names the project's
    // "Assets/Models/CesiumMan/CesiumMan.gltf", and whichever base was tried
    // first won.
    //
    //   absolute                                   -> returned as is (not checked);
    //   "<AssetDirectory>/..." (exact case) or "../..."
    //                                              -> ActiveProjectDirectory / path;
    //                                                 "../..." is how the asset registry
    //                                                 keys content outside the project;
    //   anything else                              -> path, against the working directory;
    //   not there                                  -> an error naming the path and the
    //                                                 base, and an EMPTY path.
    //
    // The empty result is the point: a caller must treat it as "did not load"
    // rather than hand the relative path to a loader anyway, which would read it
    // against the working directory — the silent failure this exists to remove.
    [[nodiscard("an empty result means the path did not resolve")]] std::filesystem::path
    ResolveContentPath(std::string_view storedPath);
} // namespace OloEngine
