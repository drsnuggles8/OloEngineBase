# Empire, ownership and acquisitions

Status: consolidated v1.0 draft for repository adoption, 2026-11-27. Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Separate geography, resort business, ski domain and corporate ownership. Ownership, operation, development rights and guest access are independent. Acquisition transfers a business and agreed liabilities, not a copy of terrain or guests. Integration into a mega-resort is a potentially multi-year capital project: physical links, access products, operating policies and branding need deliberate coordination. Shared passes and partnerships need not involve an acquisition.

Test three empire configurations: living (global calendar/detailed delegated operation), relaxed (global calendar/simplified operations), independent regional sandbox (local calendars/frozen inactive regions). One clock per connected or directly competing region. Cross-era transfers, obligations, shared expertise and transitions require explicit semantics before shipping; no accidental profit from switching fidelity.

Global opportunities emerge over time through a globe interface; reputation helps but no arbitrary map unlock levels. Players may choose a single excellent resort. Managers must respect delegated authority, and switching resorts should not be forced by routine problems.

## Decision records

<a id="a-01"></a>

### A-01

**Disposition:** Accepted.

Distinguish geographical regions, individual resorts, physically connected ski domains and owning resort groups. Ownership, access rights, commercial integration and physical connectivity are separate.

<a id="a-02"></a>

### A-02

**Disposition:** Accepted.

Empire saves may begin in different eras and starting conditions: greenfield development, an existing resort, distressed acquisition or an established portfolio.

<a id="a-03"></a>

### A-03

**Disposition:** Accepted.

Geographic expansion through land sectors, concessions, acquisition and new base areas. Region sizes follow geography; the initial 10 × 10 km map is not a universal limit.

<a id="a-04"></a>

### A-04

**Disposition:** Accepted with refinement.

Buy operating businesses, not empty plots: infrastructure, condition, accounts, obligations, reputation and development potential matter. Negotiation, rival bids and limited availability make acquisition opportunities meaningful. Provide due diligence before commitment.

<a id="a-05"></a>

### A-05

**Disposition:** Accepted.

Dynamic valuations and simplified negotiation, grounded in resort assets, liabilities, operations and development potential; no repetitive negotiation minigame.

<a id="a-06"></a>

### A-06

**Disposition:** Accepted.

Integration is a substantial, potentially multi-year and capital-intensive project: physical links, commercial access, operations, branding and capacity. Purchasing alone does not deliver a functional mega-resort. No mandatory lengthy construction timers implied; integration can require successive investments and operating results.

<a id="a-07"></a>

### A-07

**Disposition:** Accepted.

Cooperation without ownership changes: shared passes, joint infrastructure and other commercial arrangements are valid options. Scope and revenue allocation to prototype.

<a id="a-11"></a>

### A-11

**Disposition:** Accepted with refinement.

Globe/world interface inspired by Planet Zoo. Investment opportunities appear over time as land becomes available or sellers offer resorts. Access depends on capital, opportunity and business credibility/reputation, not arbitrary map unlock levels. An operator's demonstrated management record should help acquire resorts. Exact selection and bidding rules remain open.

<a id="a-12"></a>

### A-12

**Disposition:** Accepted direction; configurable.

Delegated managers and configurable authority; players may instead choose inactive/frozen resorts or simplified automatic revenue for unattended resorts. Full simulation, frozen and simplified modes have different semantics to define and test. No unapproved permanent construction/demolition by default.

<a id="a-13"></a>

### A-13

**Disposition:** Accepted starting point; revisitable.

Global or independent regional calendars. Directly connected or competing resorts within a region share a clock. Cross-era finance and technology sharing configurable; exact rules and defaults are open and may be revised.

<a id="a-14"></a>

### A-14

**Disposition:** Accepted.

Research and organizational expertise generally shared across an empire to avoid repeating the same progression. Respect historical availability and local adoption where appropriate.

<a id="a-15"></a>

### A-15

**Disposition:** Accepted.

Geographic, seasonal and customer-market diversification should provide genuine financial resilience through underlying economics, not an artificial stability multiplier.

<a id="a-16"></a>

### A-16

**Disposition:** Accepted.

Voluntary resort sales; no surprise hostile takeovers. Loss of a resort may follow severe mismanagement under explicitly enabled insolvency rules, with clear consequences and recovery opportunities.

<a id="a-17"></a>

### A-17

**Disposition:** Accepted with refinement.

Players establish empire identity/branding at save creation (name, logo, colors, positioning) and can revise it over time. Individual resorts retain or adapt their identity and marketing; acquisition includes deciding how the new destination fits the brand. Historical records and milestones desirable. No arbitrary cosmetic bonuses.

<a id="a-18a"></a>

### A-18a

**Disposition:** Accepted.

Empire growth introduces genuinely different resort experiences and new opportunities to build, while specializing in one outstanding resort remains an equally valid playstyle.

<a id="a-18"></a>

### A-18

**Disposition:** Accepted.

Empire dashboard and exception-driven notifications. Avoid interrupting active resort construction or forcing switches; escalate genuinely important situations and allow direct navigation.

<a id="a-20"></a>

### A-20

**Disposition:** Accepted.

Prototype competition, integration, multi-region management, autonomous builders and portfolio growth progressively; use human playtests to verify additional enjoyment rather than repetitive administration.

<a id="aud-07"></a>

### AUD-07

**Disposition:** Accepted.

Ownership, operating responsibility, construction rights and commercial access independently representable. Acquisitions transfer relationships and agreed liabilities, not physical copies of regions/assets.

<a id="aud-08"></a>

### AUD-08

**Disposition:** Accepted for prototyping.

Compare all three empire configurations: living empire (global calendar, detailed delegated operation), relaxed empire (global calendar, simplified operations), independent regional sandbox (local calendars, frozen inactive regions). Physically connected/directly competing resorts within one region share a clock. Cross-era transfers, expertise restrictions, debt/maintenance and mode transitions need explicit contracts. No accidental profit from changing simulation fidelity. Exact defaults remain open pending tests.

## Validation and acceptance (proposed test focus)

Two resorts and connection; shared pass and bid; ownership transfer with debts; two regions in each preset; switch/reload and cross-era accounting; compare workload and enjoyment.

## Related design decisions

[AUD-06](economy.md#aud-06), [AUD-10](competition.md#aud-10), [AUD-11](economy.md#aud-11), [AUD-15](snow-and-weather.md#aud-15), [T-08](time-and-history.md#t-08), [P-08](progression-and-research.md#p-08)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
