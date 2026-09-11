#pragma once

// The asset REFERENCE INDEX: who points at whom, derived from the project's
// files on disk (issue #1128, slice 1).
//
// WHY THIS EXISTS RATHER THAN A CALL INTO AssetManager. EditorAssetManager
// already keeps a dependency graph (m_AssetDependencies / m_AssetDependents,
// reachable via GetDependencies / GetAllDependencies). It cannot answer
// "what breaks if I move this texture", for four independent reasons:
//
//   1. SceneSerializer registers NOTHING. Only six AssetSerializer call sites
//      ever register an edge (material->texture, mesh->meshsource,
//      staticmesh->{meshsource,material}, meshcollider->collidermesh,
//      animation->{source,mesh}). Scenes are the majority of referrers, and the
//      acceptance criterion for #1128 is about scenes specifically.
//   2. It is LOAD-GATED: an edge appears only when an asset is deserialized, so
//      an asset nobody opened this session contributes nothing and a referrer
//      set built from it is silently short.
//   3. It is a HANDLE-SET graph. Rewriting a reference on a move needs the
//      file, the line and the exact text of the mention. A set of handles
//      cannot produce an edit.
//   4. AssetRegistry.oar is binary and stores only handle/type/path/status/
//      mtime -- no reference data at all, so it cannot be scraped either.
//
// So the index is DERIVED: scan the project's text asset files, collect every
// candidate reference, and resolve each one exactly.
//
// THE EXTRACTION RULE, and why it is deliberately generous. Resolution is the
// filter, not extraction: a candidate that does not resolve to the asset being
// asked about is never reported as a referrer of it. Over-collecting therefore
// costs an entry in the unresolved count; under-collecting silently loses a
// referrer and breaks somebody's scene. So path candidates are recognised by
// their VALUE (any scalar ending in a known asset extension), not by a list of
// key names that would go stale the moment a component gains a field.
//
// THE RESOLUTION RULE, and why it is anchored rather than fuzzy. Checked-in
// content spells the same kind of reference at least five ways:
//
//     Assets/Textures/x.png              project-relative -- the documented one
//     Scripts/LuaScripts/x.lua           asset-directory-relative (Project::GetAssetFileSystemPath)
//     SandboxProject/Assets/Models/x.obj the legacy project-prefixed spelling (#1098)
//     assets/textures/x.png              relative to the working directory
//     ../../assets/models/y/z.gltf       ...which is also how these resolve
//
// Checkerboard.png exists under BOTH OloEditor/assets/textures/ and the project's
// own Assets/Textures/, so matching on filename or path suffix would invent a
// referrer for an asset nothing points at. Every raw value is therefore resolved
// against an ORDERED set of anchors and compared as a canonical filesystem path.
// A value that resolves to no file on disk is reported as unresolved, never
// dropped -- with one stated exception: a value with NO directory separator is
// ambiguous between a reference and a name that happens to end in an asset
// extension (`Scene: Courtyard.olo` is a title), so a bare name counts only when
// it resolves. Reporting the other 99 as broken references would bury the one
// that is.
//
// Editor-free and AssetManager-free on purpose (only the stdlib and the engine's
// extension map), so the whole index is unit-testable against a temp directory
// with no project, no registry and no editor -- which is what the tests do.

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Automation
{
    // How one reference names its target.
    enum class AssetReferenceKind : u8
    {
        Path = 0, // a path-shaped string value, e.g. "Assets/Textures/x.png".
        Handle,   // a nonzero decimal asset handle under a handle-shaped key.
    };

    // Which base a path reference resolved against. This is NOT an invention of
    // this index: it mirrors EditorAssetManager::ImportAsset's own ordered
    // resolution (issues #887 and #1098) step for step, so a referrer answer
    // agrees with what the engine will actually load. Recording WHICH step won
    // is what lets a move re-spell the value in the style the file already used
    // instead of rewriting every reference into one canonical form.
    enum class AssetReferenceAnchor : u8
    {
        Unresolved = 0,  // resolves to no file on disk -- broken today.
        Absolute,        // the value is already an absolute path.
        ProjectRelative, // ProjectRoot / value. The documented spelling.
        // AssetDirectory / value. NOT a guess: Project::GetAssetFileSystemPath is
        // exactly this, and it is what Scene.cpp resolves a LuaScriptComponent's
        // ScriptFile with, and what SceneSerializer resolves several component
        // paths with. Checked-in content uses it heavily -- "Scripts/LuaScripts/x.lua",
        // "Audio/ding.wav" -- and omitting it drops those references silently,
        // which is the direction that loses data.
        AssetDirectoryRelative,
        LegacyProjectPrefixed, // ProjectRoot / (value minus its leading component).
        BaseDirectory,         // one of AssetIndexScope::BaseDirectories.
    };

    // One reference found in one file, kept with enough context to REWRITE it:
    // the span of the raw value is what a move edits, so a rewrite preserves
    // whatever spelling that file happened to use.
    struct AssetReference
    {
        std::filesystem::path SourceFile; // absolute path of the file containing it.
        u32 Line = 0;                     // 1-based line number, for reporting.
        std::string Key;                  // the YAML key it sits under.
        std::string RawValue;             // EXACTLY the text in the file, quotes stripped.
        AssetReferenceKind Kind = AssetReferenceKind::Path;
        u64 HandleValue = 0; // Kind == Handle: the decimal handle.
        // Kind == Path: the canonical absolute file the value resolves to, or
        // empty when it resolves to nothing on disk (a broken reference).
        std::filesystem::path ResolvedFile;
        AssetReferenceAnchor Anchor = AssetReferenceAnchor::Unresolved;
        // Anchor == BaseDirectory: which entry of AssetIndexScope::BaseDirectories
        // won, so a re-spell stays relative to that same base.
        u32 BaseDirectoryIndex = 0;
        // Byte offset of RawValue within its line, so a rewrite edits the value
        // and not a coincidentally equal substring of the key or a comment.
        u32 ValueColumn = 0;

        [[nodiscard]] bool IsResolved() const
        {
            return Kind == AssetReferenceKind::Handle ? HandleValue != 0 : !ResolvedFile.empty();
        }
    };

    // What the scan did NOT see. Every referrer answer carries this, because an
    // empty referrer list is only meaningful next to the set it was drawn from:
    // "nothing found in what I scanned" is a different claim from "nothing
    // references this", and conflating them is how a delete silently breaks a
    // scene (CLAUDE.md, *no silent fallbacks*).
    struct AssetIndexCoverage
    {
        u32 FilesScanned = 0;
        u32 ReferencesFound = 0;
        // Files whose text could not be read. Named, not just counted.
        std::vector<std::string> UnreadableFiles;
        // Binary/opaque files under the project that were skipped, by extension.
        // A .glb can carry external references this scan cannot see.
        u32 BinaryFilesSkipped = 0;
        std::vector<std::string> BinaryExtensionsSkipped;
        // Path values that resolved to no file on disk. These are broken
        // references today, and they are reported rather than dropped.
        u32 UnresolvedReferences = 0;
        // True when MaxFiles stopped the walk. A truncated index may be missing
        // referrers, so a caller MUST refuse a destructive operation on it.
        bool Truncated = false;
        // False when the scan could not see everything it was asked to for any
        // OTHER reason: an unusable project root, a walk that could not start or
        // could not finish, a file it could not read.
        //
        // Separate from Truncated only in what caused it -- both mean the referrer
        // set may be SHORT, and a short referrer set is indistinguishable from an
        // empty one at the call site. Without this, a scan that failed before it
        // read a single file returned zero references with Truncated false, and a
        // destructive command gating on Truncated alone would read that as
        // "nothing references this asset" and go ahead. Use Reliable() rather than
        // testing either flag by hand.
        bool Complete = true;
        // Why Complete is false, for the result. Empty when it is true.
        std::string IncompleteReason;

        // Whether this index may be trusted to answer "what references X"
        // NEGATIVELY. Every destructive command must check it; a read-only query
        // may still report what it found, alongside the coverage that says so.
        [[nodiscard]] bool Reliable() const
        {
            return Complete && !Truncated;
        }
    };

    struct AssetIndex
    {
        std::vector<AssetReference> References;
        AssetIndexCoverage Coverage;
    };

    // Where the scan looks and how it anchors a relative path.
    struct AssetIndexScope
    {
        // The directory holding the project file; the walk root.
        std::filesystem::path ProjectRoot;
        // ProjectRoot / ProjectConfig::AssetDirectory. Several engine consumers
        // resolve against this rather than the project root (see
        // AssetReferenceAnchor::AssetDirectoryRelative). Empty disables that anchor.
        std::filesystem::path AssetDirectory;
        // Ordered base directories a relative reference is resolved against, most
        // specific first. The referring file's own directory is always tried too
        // (for the "../../assets/..." spelling) and does not belong here.
        std::vector<std::filesystem::path> BaseDirectories;
        // Bounds the walk. Exceeding it sets Coverage.Truncated rather than
        // quietly returning a short index.
        u32 MaxFiles = 20000;
    };

    // True for a file this scan can read as text (the YAML-shaped asset formats
    // plus the script sources scenes point at). Exposed so a caller can report
    // the scanned set instead of describing it in prose.
    [[nodiscard]] bool IsScannableAssetFile(const std::filesystem::path& path);

    // Every extension IsScannableAssetFile accepts, lowercase and dotted, sorted.
    [[nodiscard]] std::vector<std::string> ScannableExtensions();

    // Walk ProjectRoot and build the index. Never throws for a file it cannot
    // read -- that lands in Coverage.UnreadableFiles.
    [[nodiscard]] AssetIndex BuildAssetIndex(const AssetIndexScope& scope);

    // References whose target is targetFile (canonical absolute) or, when
    // targetHandle is nonzero, that handle. A reference from targetFile to
    // itself is excluded -- an asset is not its own referrer.
    [[nodiscard]] std::vector<AssetReference> FindReferrers(const AssetIndex& index,
                                                            const std::filesystem::path& targetFile,
                                                            u64 targetHandle);

    // References CONTAINED IN sourceFile -- the other graph direction, i.e. what
    // this asset needs.
    [[nodiscard]] std::vector<AssetReference> FindDependencies(const AssetIndex& index,
                                                               const std::filesystem::path& sourceFile);

    // Re-spell one reference's raw value so it points at newTarget instead of
    // oldTarget, keeping the original's style: a value written relative to some
    // base directory stays relative to that same base, one written relative to
    // its own file stays file-relative, and the original separator is preserved.
    // Returns an empty string when the reference needs no rewrite (a handle
    // reference) or cannot be re-spelled.
    [[nodiscard]] std::string RespellReference(const AssetReference& reference,
                                               const std::filesystem::path& oldTarget,
                                               const std::filesystem::path& newTarget,
                                               const AssetIndexScope& scope);

    // Apply replacement to the reference's value span in fileText and return the
    // new text. Line/column addressed rather than search-and-replace, so a value
    // that also appears in a comment or under another key is untouched. Returns
    // false when the span does not match reference.RawValue -- i.e. the file
    // changed under the index, which a caller must treat as a refusal, never as
    // a reason to fall back to a textual replace.
    [[nodiscard]] bool ApplyReferenceEdit(std::string& fileText, const AssetReference& reference,
                                          const std::string& replacement);
} // namespace OloEngine::Automation
