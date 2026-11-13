#pragma once

// The one bridge from a MeshSource and its Skeleton to the GroomSurfaceView the
// binder, the runtime deformation and the topology check all read. The rest of
// the groom binding code stays free of renderer and animation headers; this is
// where the two meet, so the scene and every test that signs a body build the
// view the same way. Five tests used to carry their own copy of the scene's
// private helper, and one had already dropped its empty-mesh guard.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"

namespace OloEngine
{
    class MeshSource;
    class Skeleton;

    /// Identity of the skeleton behind a surface, for
    /// GroomBindingTargetSignature::SkeletonNameHash: HashGroomSkeletonNames over
    /// its bone names, or 0 without a skeleton.
    [[nodiscard]] u64 HashGroomSkeletonIdentity(const Skeleton* skeleton);

    /// The surface as the binder and the deformation both see it.
    ///
    /// The positions are the mesh's LIVE vertex array, which the morph pass has
    /// already deformed in place: that is what makes a facial morph reach the
    /// coat with no morph-specific code in the groom at all. A caller that signs
    /// the view every frame takes the index hash from the same array the
    /// deformation reads, which is why nothing is cached beside it. An empty
    /// mesh yields a default (unusable) view.
    [[nodiscard]] GroomSurfaceView MakeGroomSurfaceView(const MeshSource& surface, const Skeleton* skeleton);
} // namespace OloEngine
