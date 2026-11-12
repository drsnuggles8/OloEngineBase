# OloEngine vcpkg triplet — Linux, static libraries. vcpkg's stock x64-linux plus one flag.
#
# -ffp-contract=off for every port, the engine's own setting (root CMakeLists.txt, #1473)
# carried to the code it links. Under the GNU dialects GCC defaults to -ffp-contract=fast,
# which fuses a*b+c into an FMA wherever the target has one, and Rocky Linux 10's GCC
# targets x86-64-v3 by default. So the AMD box's meshoptimizer computed its overdraw
# cluster keys with FMAs while every other compiler did not, and one of the Samoyed's ten
# submeshes (DogNose) came out of meshopt_optimizeOverdraw in a different triangle order
# on that box alone. A groom binding addresses its body by triangle, so the shipped Samoyed
# binding refused the Linux import with TargetTopologyMismatch (#1674). Every port that does
# float math the engine hashes, caches or compares across machines (meshoptimizer, Assimp,
# Jolt) has the same exposure; off makes a float expression mean the same thing everywhere.
#
# The overlay keeps the stock name, so build trees, cache paths and workflows still say
# x64-linux; it only takes effect through VCPKG_OVERLAY_TRIPLETS, which the Linux presets
# and .github/actions/setup-vcpkg pass and the root CMakeLists.txt checks. The file's
# contents feed every port's ABI hash: an edit rebuilds every Linux port once.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_C_FLAGS "-ffp-contract=off")
set(VCPKG_CXX_FLAGS "-ffp-contract=off")
