# User experience and interaction

Status: consolidated v1.0 draft for repository adoption, 2026-11-27. Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

The mountain is the main interface. Preserve proportions, terrain-aware navigation and an unobtrusive HUD. The previous large slate HUD mockup was rejected; do not use it as a design baseline. Contextual inspection, comparison, bulk editing, clear construction previews and progressive disclosure make depth accessible.

Interactive maps and overlays answer decisions about access, traffic, costs, snow, history and forecasts, with selectable map styles and sharing. Forecasts communicate uncertainty. Optional inspection aids alter presentation only. Camera bookmarks, guest/lift following, photo mode and historical comparisons support attachment. Localization and color-independent status/overlay cues are foundational.

## Decision records

<a id="u-02"></a>

### U-02

**Disposition:** Accepted.

Preserve realistic proportions; adapt information and presentation to viewing distance without changing simulation.

<a id="u-03"></a>

### U-03

**Disposition:** Accepted; high priority.

Excellent terrain-aware strategy camera, top-down precision, object following, bookmarks and optional cinematic/guest perspectives.

<a id="u-04"></a>

### U-04

**Disposition:** Accepted direction; mockup rejected.

Minimal HUD with essential configurable indicators and contextual panels. Owner dislikes the proposed space-heavy layout and colors; do not adopt that mockup as a requirement. Explore historically evolving UI themes.

<a id="u-05"></a>

### U-05

**Disposition:** Accepted.

Progressive disclosure: approachable summaries and optional operational, financial and historical detail.

<a id="u-06"></a>

### U-06

**Disposition:** Accepted.

Inspect meaningful simulation entities, relationships, comparison and bulk edits where appropriate.

<a id="u-07"></a>

### U-07

**Disposition:** Accepted; high priority.

Intuitive responsive construction, clear contextual validation, optional snapping/precision and consistent controls.

<a id="u-08"></a>

### U-08

**Disposition:** Accepted direction; explore.

Interactive resort map, selectable styles, useful construction/management interaction and shareable exports. Explore as a design problem rather than assuming the mockup is final.

<a id="u-09"></a>

### U-09

**Disposition:** Accepted.

Prioritize decision-useful, intuitive overlays, including historical and forecast views; avoid exposing variables merely because they exist.

<a id="u-14"></a>

### U-14

**Disposition:** Accepted.

Optional architectural customization and reusable templates, alongside ready-made buildings and resort identity.

<a id="u-16"></a>

### U-16

**Disposition:** Accepted direction.

Realistic conditions with optional inspection aids; allow players to reduce cumbersome visual effects or aids. No authoritative gameplay changes from viewing options.

<a id="u-19"></a>

### U-19

**Disposition:** Accepted.

Guest following, lift-passenger and ground-level perspectives; playable skiing remains a future decision.

<a id="u-20"></a>

### U-20

**Disposition:** Accepted.

Photo mode, saved viewpoints, historical comparison and sharing.

<a id="u-21"></a>

### U-21

**Disposition:** Accepted.

Accessibility, localization readiness, configurable controls, scaling and color-independent information.

## Validation and acceptance (proposed test focus)

Unassisted lift/piste construction, problem diagnosis, navigation, map export, accessibility and visibility across presets.

## Related design decisions

[AUD-14](art-direction.md#aud-14), [U-22](art-direction.md#u-22), [C-10](../gameplay/construction.md#c-10), [G-15](../gameplay/guests-and-networks.md#g-15), [P-15](../gameplay/progression-and-research.md#p-15)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
