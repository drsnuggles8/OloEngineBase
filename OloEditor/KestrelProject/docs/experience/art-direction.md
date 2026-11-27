# Art direction, animation and audio

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Romantic Alpine realism: grandeur, comfort, activity and transformation, with region- and era-specific research. Preserve old infrastructure’s identity until refurbishment/replacement. Skiing, snowboarding, sledding and lift boarding/operation must look convincing; visible snow and queues reflect actual simulation. Spatial machinery, guests, weather and music reinforce atmosphere.

Expected gameplay graphics preset should look exceptional. Lowest presets may sacrifice fidelity, but not essential decision information or simulation correctness. Shared UI components and interaction semantics underpin potentially multiple historical visual themes; exact themes and layouts remain exploratory. Historical appearance must not make controls inconsistent.

## Decision records

<a id="u-01"></a>

### U-01

**Disposition:** Accepted.

Romantic Alpine realism: physically believable photoreal environments with grandeur, comfort, activity and transformation; adapt to regional character.

<a id="u-13"></a>

### U-13

**Disposition:** Accepted direction.

Researched historical and regional visual identities, with existing assets retaining their character. Explore UI themes evolving with historical era too.

<a id="u-15"></a>

### U-15

**Disposition:** Accepted; high priority.

Believable skier and lift operation and visible consequences are core visual requirements; snowboarding, sledding and other supported activities must feel convincing too.

<a id="u-17"></a>

### U-17

**Disposition:** Accepted with qualification.

Expected gameplay graphics preset must look amazing; lowest presets may sacrifice visual detail and fidelity. Preserve essential decision information and simulation semantics. Existing target hardware/fps remains unchanged. Exact quality tier and acceptance imagery need validation.

<a id="u-18"></a>

### U-18

**Disposition:** Accepted; high priority.

Audio is fundamental to atmosphere: spatial ambience, machinery, guests, weather and configurable music.

<a id="u-22"></a>

### U-22

**Disposition:** Accepted direction.

Formal reusable UI design system; potentially multiple historically appropriate themes. Shared interaction conventions should remain consistent.

<a id="aud-14"></a>

### AUD-14

**Disposition:** Accepted.

Share authoritative definitions and state across management UI, advisors, encyclopedia, maps and overlays. Advisors may disagree in priorities, not in facts. Historical UI themes retain stable interaction semantics; automated checks and reviewed explanatory documentation support encyclopedia accuracy. Rejected HUD mockup is not the design baseline. Theme variants, fixed-theme choice and detailed layouts remain UX experiments.

## Validation and acceptance (proposed test focus)

Reference scenes: morning, busy day, snowfall, night, summer, guest perspective. Compare expected/low presets and conduct human visual/audio review.

## Related design decisions

[U-04](UX.md#u-04), [U-16](UX.md#u-16), [U-21](UX.md#u-21), [V-18](../validation/automated-balancing.md#v-18), [AUD-03](../gameplay/time-and-history.md#aud-03)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
