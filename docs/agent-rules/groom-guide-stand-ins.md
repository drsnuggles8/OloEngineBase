# Guide stand-ins keep their neighbours' drape (#1533)

Read before touching `ExpandGroomGuideDisplacements` or `BuildGroomGuideStandIns` in
`Groom/GroomGuideInfluence.cpp`, a coat's per-role guide budget (`GroomSimulationComponent::
m_MaxGuides*`), or the simulation steps of `GroomLodComponent`. The solver itself is
[groom-guide-simulation.md](groom-guide-simulation.md)'s.

## The rules

1. **Most of a coat is stand-ins, so the stand-in blend decides the coat's drape.** A guide slot the
   budget does not simulate is published as a weighted blend of up to four simulated guides of its
   own group and role (#1509). At a full budget the long-coated horse simulates 1,118 of its 7,815
   slots and the showcase dog 1,472 of its 16,580. So 86% and 91% of those coats move by the blend,
   not by the solver.

2. **Take the direction from the linear blend, and the length from the neighbours where they
   agree.** A linear blend of displacements that point different ways averages away the part of
   each that points its own way. Two guides that sag the same distance 90 degrees apart give a
   stand-in 0.71 of that distance, so it sags less than either of them. `KeepNeighbourLength` keeps
   the blend's direction and takes the weighted mean of the neighbours' lengths. It fades that in by
   coherence, the blend's length over that mean length, which is cos(angle / 2) for two equal
   guides. The mean length is taken in full up to ~106 degrees apart and not at all past ~150.
   Where the neighbours cancel, at a part or a whorl, no direction is right, so the short linear
   blend stays.

3. **A budget change must not change the drape.** The LOD ladder halves the simulated guides with
   distance: 1,118, then 561, then 281 on the long coat. With the linear blend, each halving moved
   the stand-ins further from their neighbours' drape. The long coat's strand tier read 14% less
   shadowed at 14 m than the full coat: seen energy 1.156 against a +-10% contract. With the length
   kept it reads 1.039.

## How it was found

- **Print the budget before blaming it.** Print `GuidesSimulated` and the slot count at each LOD
  stop. Even the full-budget arm was mostly stand-ins.
- **A/B the blend, not the solver.** Simulated guides are independent of each other, so a smaller
  budget simulates the guides it keeps exactly as before, and only the stand-ins differ. Stand-ins
  that copy their nearest guide read 1.031 at 14 m, which passes and proves the cause, but it snaps
  neighbouring stand-ins to different guides. The length-keeping blend reads 1.039 and stays
  smooth.
- **Pinned by** `GroomGuideInfluence.AStandInBetweenDivergingGuidesSagsAsFarAsTheyDo`, which reads
  0.707 with the linear blend, and `OpposedGuidesLeaveTheLinearBlend`.
