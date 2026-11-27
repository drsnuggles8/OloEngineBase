# Construction and terrain editing

Status: consolidated v1.0 draft for repository adoption, 2026-11-27. Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

The player creates functional infrastructure directly on the mountain. Free-form lifts and pistes receive live geometry/cost/rights validation and optional precision aids; buildings support attractive ready-made variants and optional customization. Preserve the terrain as a meaningful constraint.

The operating lifecycle is red (closed), yellow (test run), green (open). Construction may be corrected before first opening only where the underlying transaction is safe. Opening is the irreversible operational refund boundary; later closure or retesting never restores the original refund. Keep structural modification and demolition separate from preview editing. Handle terrain, rights, reservations and dependent objects transactionally even before opening. No ordinary passenger revenue during testing.

Blueprints describe designs, not paid assets or reservations. Revalidate all dependencies and prices against the destination at placement. Do not implement persistent project-management commitments as an accepted requirement.

## Decision records

<a id="c-01"></a>

### C-01

**Disposition:** Accepted.

Hybrid construction: free-form infrastructure and accessible functional buildings, with optional creative customization.

<a id="c-02"></a>

### C-02

**Disposition:** Accepted direction; representation provisional.

Editable splines are the preferred authoring approach unless a demonstrably better interaction or representation exists. Pistes may split, join and offer independently rated alternatives (e.g. a black shortcut branching off a longer red route). The owner accepts the corridor/branching approach, including locally adjustable boundaries. Splines remain the preferred authoring representation rather than a mandatory internal format.

<a id="c-03"></a>

### C-03

**Disposition:** Accepted.

Lift construction must be realistic but intuitive, with automatic engineering assistance and optional fine-tuning. Equipment and alignment choices must have genuine capacity and operational consequences. No mandatory detailed ropeway engineering.

<a id="c-04"></a>

### C-04

**Disposition:** Accepted.

Terrain reshaping is very limited in ordinary play, primarily constrained by meaningful costs. Preserve geography as a challenge; do not make bulldozing a cheap universal solution. Allow useful localized grading, foundations and other engineering works, including reservoirs for snowmaking. The owner approves volume-based cut/fill pricing and wants substrate (soil versus hard rock) to affect cost. Exact coefficients, physical limits and creative-mode exceptions remain open.

<a id="c-05"></a>

### C-05

**Disposition:** Accepted.

Functional hotels, restaurants and other buildings with optional variants, decorations and modular customization; focus on resort building rather than compulsory architectural micromanagement.

<a id="c-06"></a>

### C-06

**Disposition:** Accepted.

Land/construction rights and available technology together determine expansion opportunities. A map can contain multiple resorts; ownership is distinct from physical connectivity. The owner proposes an abstract government-influence mechanism for obtaining land rights. The exact mechanism, eligibility and research gates remain open; transparent permit/relationship rules are a design hypothesis, not an approved implementation.

<a id="c-07"></a>

### C-07

**Disposition:** Accepted; superseded details by AUD-01.

Construction undo and recoverable percentage are important, with configurable defaults. Free preview editing; before first opening, transaction-safe corrections and configurable refunds. First green/open operation ends construction undo; closure or retesting does not restore it. Paid modifications/demolition afterward. Exact eligibility for pre-opening side effects, rates and test interaction remain open.

<a id="c-08"></a>

### C-08

**Disposition:** Preferred direction; validate.

Owner leans toward immediate construction and prioritizes fun over realism. Competitor comparison supports instant default construction with optional planning/animation. Delayed projects are an optional hypothesis, not a requirement. See T-05.

<a id="c-09"></a>

### C-09

**Disposition:** Accepted direction.

Piste difficulty must account for more than steepness: geometry, visibility, snow and weather conditions, informed by real-world practices. Support regional rating conventions including black/double-black diamonds where appropriate. Separate assessed baseline difficulty, advertised rating and current conditions. The owner supports consequences for misrating: advertising a run as easier than it really is can increase accidents; overstating challenge can disappoint experienced guests. Exact override range, safety rules and behavioral model remain open.

<a id="c-10"></a>

### C-10

**Disposition:** Accepted direction; elaborated by U-08 and P-21.

Provide readable interactive lift and piste maps with selectable visual styles, useful planning/management views and export/sharing. Derive them from the live network. Exact map generation and supported construction interactions require UX experiments.

<a id="c-11"></a>

### C-11

**Disposition:** Accepted direction.

Include meaningful snowmaking infrastructure such as water reservoirs. The player should be able to invest in cheaper or more reliable snow production through infrastructure placement. The owner prefers a somewhat abstract utility model and is unsure whether precise electricity prices/energy calculations add enough gameplay; test before committing. Reservoirs, water availability, snowmaking coverage and plausible weather constraints are the desired direction; physical pipe routing, detailed pressure/energy and electricity price exposure remain hypotheses. No free or unlimited snow.

<a id="aud-01"></a>

### AUD-01

**Disposition:** Accepted direction; prototype details open.

Owner proposes RollerCoaster Tycoon-style red (closed/construction), yellow (test run), green (open) operating states. Before the first opening, construction corrections and configurable undo refunds remain possible where transactionally safe. Yellow is a meaningful visible test without ordinary paying guests; green activates normal operational participation. Once opened, subsequent structural changes and removal use ordinary modification/demolition costs; returning to red or yellow must not reset refund eligibility. The sticky ever-opened state, short test runs, bulk opening, and simple lifecycle for decorations/buildings are proposed refinements, not separately owner-approved details. Pre-opening terrain, land, demand and dependency effects must be deferred or handled transactionally; no duplicate refunds, income or restoration. Exact refund rates and testing duration remain experimental.

<a id="aud-13"></a>

### AUD-13

**Disposition:** Accepted.

Blueprints are reusable editable construction descriptions, not reservations, assets or financial commitments. Revalidate terrain, rights, technology, collisions, dependencies and current prices at placement; use ordinary authoritative construction commands. Elaborate persistent multi-project management remains deferred.

## Validation and acceptance (proposed test focus)

Build/test/open/close/remove; undo/redo with price changes, land rights and dependent edits; save/load; no duplicate recovery; assess test-run UX and paid modifications.

## Related design decisions

[T-05](time-and-history.md#t-05), [AUD-04](guests-and-networks.md#aud-04), [AUD-06](economy.md#aud-06), [AUD-07](empire-and-acquisitions.md#aud-07), [U-07](../experience/UX.md#u-07), [P-03](progression-and-research.md#p-03), [P-21](../content/modding-and-sharing.md#p-21)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
