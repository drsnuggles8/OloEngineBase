# Vision and product identity

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Build and manage ski resorts on geographically distinct mountains. The core loop is construct → operate → observe guests and finances → improve → unlock new opportunities to build. The player may remain on one mountain or grow an international group. Curated scenarios, historical development and sporting events provide different contexts for the same simulation.

The full-game vision is deliberately ambitious. The current engineering north star remains a separate scope/time contract: June 30, 2027 internal vertical slice, one real Alpine mountain (approximately 10 × 10 km), 1950 start, one seasonal cycle, early technology circa 1950–1965, sandbox only, representative 1080p60 p95 on GTX 1660/RX 5600-class hardware, 6 GB VRAM, 16 GB RAM and six-core CPU. These details are recorded in the workshop history, not reverified against current code. No new milestone or release date is set here. The repository states that contract in its [north star](../../../docs/process/north-star.md). The owner settled the frame budget on 2026-11-27: the north star now gives 60 fps as a 95th-percentile frame time of 16.6 ms or less on the slice's busiest view, matching the p95 above.

Scale is part of the vision ([AUD-21](gameplay/guests-and-networks.md#aud-21)): the vertical slice simulates at least 20,000 concurrent, individually represented skiers, and the full game's engineering ambition is about 100,000 to 200,000 concurrent guests in a large region, with no arbitrary player-facing population cap.

## Decision records

<a id="d-001"></a>

### D-001

**Disposition:** Owner-stated direction.

KESTREL is a resort construction and management game, primarily focused on building resorts. The game is not designed around the trailer.

<a id="d-002"></a>

### D-002

**Disposition:** Owner-stated direction.

Multiple maps are essential to the full game. Each resort in the empire can be played as a sandbox; local resort play and long-term portfolio growth belong together.

<a id="d-003"></a>

### D-003

**Disposition:** Owner-stated direction.

Include curated scenarios in the spirit of RollerCoaster Tycoon, including rescue and expansion of existing resorts.

<a id="d-004"></a>

### D-004

**Disposition:** Owner-stated direction.

Buying and merging neighboring resorts into a larger connected ski domain is important and should be represented in a scenario.

<a id="d-005"></a>

### D-005

**Disposition:** Owner-stated direction.

Geography and weather must have gameplay consequences, not just visual effects.

<a id="d-006"></a>

### D-006

**Disposition:** Owner-stated direction.

Content and rules should be data-driven.

<a id="d-007"></a>

### D-007

**Disposition:** Owner-stated direction.

Difficulty must not rely on arbitrary cost/demand bonuses or AI opponents cheating. Competition should be fair.

<a id="d-008"></a>

### D-008

**Disposition:** Owner-stated direction.

Establish proper human playtesting and feedback loops early.

<a id="d-009"></a>

### D-009

**Disposition:** Owner-stated direction.

Major winter sporting events are a desired scenario direction (owner proposed Olympics/X Games-style events). Exact event types, rules and licensed branding remain undecided.

## Validation and acceptance (proposed test focus)

Core-loop enjoyment, differences between mountains and appeal of self-directed expansion.

## Related design decisions

[P-13](gameplay/progression-and-research.md#p-13), [S-22](gameplay/scenarios-and-events.md#s-22), [AUD-18](validation/production-checkpoints.md#aud-18), [V-22](validation/playtesting.md#v-22)

See [decision index](DECISIONS.md) and [open questions](OPEN_QUESTIONS.md).
