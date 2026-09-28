#pragma once

#include <filesystem>
#include <string_view>

namespace OloEngine
{
    // The ONE resolver for a stored, relative content path (issue #1496) — a
    // ModelComponent's FilePath, a foliage layer's MeshPath / AlbedoPath / leaf
    // maps, a texture's stored source path. Content authored inside a project is
    // spelled project-relative ("Assets/Models/Sponza/Sponza.gltf"); engine
    // content is spelled relative to the working directory
    // ("assets/models/Fox/Fox.gltf", under OloEditor/). Opening either verbatim
    // reads against the process working directory, which is not the project.
    //
    //   absolute                   -> returned as is (not checked);
    //   relative, under project    -> ActiveProjectDirectory / path, if that file exists;
    //   relative, under the cwd    -> path, if that file exists;
    //   neither                    -> an error naming the path, and an EMPTY path.
    //
    // The empty result is the point: a caller must treat it as "did not load"
    // rather than hand the relative path to a loader anyway, which would read it
    // against the working directory — the silent failure this exists to remove.
    [[nodiscard("an empty result means the path did not resolve")]] std::filesystem::path
    ResolveContentPath(std::string_view storedPath);
} // namespace OloEngine
