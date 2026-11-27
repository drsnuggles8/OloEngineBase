# Snow, mountain operations and weather

Status: consolidated v1.0 draft for repository adoption, 2026-11-27. Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Snow is a signature simulation system: persistent spatial conditions, snowfall, ice/powder, compaction, melt, grooming and traffic wear. Weather varies across a mountain; forecast uncertainty and climate evolution affect investments. Snowmaking has physical coverage, water and weather constraints; detailed utility networks are not required by default. Grooming uses fleets, terrain capabilities and automatic priorities; preserve deliberately ungroomed areas.

No destructive natural disasters. Avalanche mechanics emphasize abstract protection and off-piste guest risk; exact restrictions are open. Configurable wind/realism restrictions and environmental settings preserve player choice. Keep environmental and operational effects independent of rendering. Use gameplay-driven multiresolution state and conserve snow mass/history during refinement; benchmark alternatives, including piste-aligned fields with no double ownership of mass.

Competent automation is the baseline; optional manual optimization rewards better decisions, not more clicks.

## Decision records

<a id="w-03"></a>

### W-03

**Disposition:** Accepted.

Correlated regional weather and local microclimate variation. Use climate data and, where obtainable and suitable, historical resort snow reports to establish and validate regional historical inputs. Sources, rights, quality, coverage and resolution must be checked; no source is yet selected.

<a id="w-04"></a>

### W-04

**Disposition:** Accepted.

In-depth persistent spatial snow simulation is a priority: different snow types, accumulation, depth, density, surface state, local conditions, usage and history. Scientific fidelity is subordinate to gameplay and computational budgets, but snow should not be reduced to a cosmetic or global quality scalar.

<a id="w-05"></a>

### W-05

**Disposition:** Accepted.

Skier traffic changes snow condition and wear; width, traffic distribution and grooming matter. Authoritative wear is independent of camera/rendering LOD.

<a id="w-06"></a>

### W-06

**Disposition:** Accepted.

Intermediate snowmaking model: physical equipment and reservoirs, water availability and weather-dependent production; pipes, electricity and detailed engineering abstract initially.

<a id="w-07"></a>

### W-07

**Disposition:** Accepted.

Automated grooming using fleets, capacity and priorities. Players may explicitly leave areas ungroomed. Some terrain exceeds the capabilities of available grooming technology; later equipment/expertise may make more terrain feasible. Exact thresholds and equipment types are open.

<a id="w-08"></a>

### W-08

**Disposition:** Accepted.

Useful forecasts with uncertainty and operational implications; historical technology may improve information. Same information constraints for AI and human operators.

<a id="w-09"></a>

### W-09

**Disposition:** Accepted direction; configurable.

Local wind and equipment-specific operating limits matter; allow players to disable or simplify realism-related weather interruptions. Exact settings and scope remain open.

<a id="w-10"></a>

### W-10

**Disposition:** Accepted with modification.

Abstract avalanche control/protection and risk assessment rather than destructive avalanche simulation. Avalanche hazard is particularly relevant to off-piste guests. Owner prefers not to have avalanches damage or directly affect lifts. Closures/guest exposure/mitigation are gameplay candidates; the exact scope of piste closures and safe fallback is to be prototyped.

<a id="w-11"></a>

### W-11

**Disposition:** Accepted.

Configurable long-term regional climate evolution, distinct from year-to-year variability; affects strategies across decades.

<a id="w-13"></a>

### W-13

**Disposition:** Accepted direction.

Optional night skiing with suitable lighting and operating hours; potential tech-tree unlock. Exact gating and economics remain hypotheses.

<a id="w-15"></a>

### W-15

**Disposition:** Accepted.

Environmental and historical planning overlays, strategic suitability, current conditions and forecasts, consistent with earlier resort-map decisions.

<a id="w-16"></a>

### W-16

**Disposition:** Accepted with explicit exclusion.

Configurable environmental rules and fair presets; no destructive natural disasters, including as an advanced setting. Weather challenges should remain understandable and manageable.

<a id="w-17-w-18"></a>

### W-17–W-18

**Disposition:** Accepted.

Prototype interactions early; weather should present meaningful, fair choices and clear responses. Teach interventions through tutorials, documentation, contextual warnings and SimCity-style advisors (e.g. economy, traffic, mountain operations), not just raw statistics.

<a id="aud-05"></a>

### AUD-05

**Disposition:** Accepted direction.

Gameplay-driven multiresolution snow state independent of camera; preserve snow mass/history across refinement, and keep visual tracks separate from authoritative wear. Benchmark adaptive spatial grids and possible piste-aligned operational fields; avoid double ownership of snow mass. Numerical representation and resolution are engineering experiments, not product decisions.

<a id="aud-15"></a>

### AUD-15

**Disposition:** Accepted.

Competent default automation, policy customization and optional expert intervention; reward improved decisions rather than repeated clicking. Preserve strategic/creative/tactical agency and reduce administrative repetition. Respect delegated authority and explain effective inherited settings. A successful mature resort may legitimately require less attention. Exact default policy competence, optimization advantages and override interactions remain playtest questions.

## Validation and acceptance (proposed test focus)

Compare snow resolution/cost/accuracy, vulnerable return routes, scarce water, steep grooming, powder days and weather fairness; verify mass and camera independence.

## Related design decisions

[C-11](construction.md#c-11), [G-09](guests-and-networks.md#g-09), [G-17](guests-and-networks.md#g-17), [AUD-02](time-and-history.md#aud-02), [AUD-19](../validation/automated-balancing.md#aud-19), [U-11](../experience/advisors-and-onboarding.md#u-11)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
