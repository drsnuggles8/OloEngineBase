# Economy, finance and recovery

Status: consolidated v1.0 draft, adopted into the repository 2026-11-27 as documentation only (nothing here is implemented or human-playtested by adoption). Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Manage a business with understandable prices, demand, operating costs, maintenance, debt, investments and seasonal liquidity. Ski passes cover domains rather than individual rides. Multiple viable strategies exist, including small local and premium resorts; utilities and material supply chains should not become mandatory logistics micromanagement.

Keep a consistent accounting ledger for cash, revenue, assets, liabilities, capex, depreciation and consolidated finances. Loan principal is not income or an operating expense. Internal transfers cannot create group wealth; shared passes and concessions cannot double-count revenue. One currency, no FX, gradual inflation; distinguish nominal and real historical comparisons. Exact accounting conventions and ownership models remain open where noted.

Financial recovery helps new players learn, not min-max failure. Support credible conditional restructuring and assistance, with configurable severe consequences and no surprise hostile takeovers.

## Decision records

<a id="e-01"></a>

### E-01

**Disposition:** Accepted.

Combine accessible attraction management in the style of RollerCoaster Tycoon with Transport Fever-style infrastructure, demand and capacity economics. Abstract utilities/materials; no industrial supply chains as a core requirement.

<a id="e-02"></a>

### E-02

**Disposition:** Accepted direction; model open.

Player-built facilities should generate income. Direct operating income and leasing/concession income are candidates; owner suggests a rent-like or tax-like payment from third-party businesses. Exact ownership, revenue split and operating responsibilities remain open. Do not count third-party turnover as operator revenue.

<a id="e-03"></a>

### E-03

**Disposition:** Accepted.

Price passes for the ski resort/domain, not individual lifts. Day, half-day, multi-day, season and other appropriate pass products; group discounts. Support configurable dynamic pricing policies, potentially unlocked by research/expertise; gating is a proposal, not final.

<a id="e-04"></a>

### E-04

**Disposition:** Accepted.

Multiple viable business strategies: affordable high-volume, premium, family, small local, and other context-dependent profiles; visitor count alone does not define success.

<a id="e-05"></a>

### E-05

**Disposition:** Accepted.

Meaningful operating costs and opening/resource allocation decisions, with routine operation delegated to policies.

<a id="e-06"></a>

### E-06

**Disposition:** Accepted.

Maintenance and aging create economic decisions; automated by default, with optional fine-tuning that rewards understanding the system. Keep safety floors and avoid mandatory repetitive clicks.

<a id="e-07"></a>

### E-07

**Disposition:** Accepted.

Investment planning should include forecasts of likely demand, costs, financial outcomes and uncertainty.

<a id="e-08"></a>

### E-08

**Disposition:** Accepted.

Different loan products and public investment applications for specific projects; local/regional and potentially other government funding. Exact products, eligibility, conditions and terms remain open.

<a id="e-09"></a>

### E-09

**Disposition:** Accepted.

Weather and snow conditions affect cash flow; failing to save sufficient reserves before a bad season should create meaningful challenges. Preserve seasonal costs and income even when off-season is accelerated or skipped.

<a id="e-10"></a>

### E-10

**Disposition:** Accepted direction.

Mismanagement must have consequences; banks and government may offer recovery options. Configurable financial failure remains supported; no unconditional bailout guarantee. Specific distress, refinancing, rescue and bankruptcy rules require design/playtesting.

<a id="e-11"></a>

### E-11

**Disposition:** Accepted.

Targeted marketing campaigns: choose audience/location, spending and message (owner references Hollywood Animals as inspiration). Awareness and actual resort experience should interact; no guaranteed guests for spending money.

<a id="e-12"></a>

### E-12

**Disposition:** Accepted.

Abstract and understandable stakeholder/government relationships, project approvals and occasional meaningful decisions rather than political micromanagement. Land rights and protected areas remain meaningful.

<a id="e-13"></a>

### E-13

**Disposition:** Accepted.

One worldwide game currency, no exchange rates. Include some form of gradual inflation or rising prices over time. Exact currency name, nominal/real presentation, rate, treatment of wages/revenue and historical comparison remain open.

<a id="e-14"></a>

### E-14

**Disposition:** Accepted direction; configurable.

Local resort accounts plus empire capital allocation/support, with configurable rules where appropriate. Cross-era transfers and local/global clock semantics remain unresolved.

<a id="e-15"></a>

### E-15

**Disposition:** Accepted.

Conventional management-game financial UI, with accessible summaries and optional details, history and asset/sector breakdowns.

<a id="e-16-e-18"></a>

### E-16–E-18

**Disposition:** Accepted.

Automated balancing/exploit tests and human playtesting are essential. Start with a small playable economy prototype; retain the construction-led economic philosophy.

<a id="aud-06"></a>

### AUD-06

**Disposition:** Accepted.

Rigorous internal accounting for assets, liabilities, depreciation, cash, revenue, capex, debt and consolidated empire finances, with accessible management UI. Lightweight double-entry ledger is preferred. One universal currency and consistent inflation model remain as previously approved.

<a id="aud-11"></a>

### AUD-11

**Disposition:** Accepted with owner emphasis.

Financial distress and recovery are intended to help inexperienced or slower-learning players regain their footing, not to be a min-max strategy. Conditional assistance, refinancing, debt restructuring and credible recovery plans may be available; avoid unconditional repeated bailouts, asset/cash duplication and deliberate insolvency as a dominant strategy. Configurable severe consequences and no surprise hostile takeovers remain in force. Distinguish temporary illiquidity, insolvency and assistance; exact eligibility, terms and recovery mechanics require balancing. Do not penalize learning or assume all poor outcomes indicate mismanagement.

## Validation and acceptance (proposed test focus)

Test liquidity versus profit, shared income, seasonal stress, debt, assistance and repeated distress; verify ledger conservation and no refund/transfer arbitrage.

## Related design decisions

[AUD-01](construction.md#aud-01), [AUD-07](empire-and-acquisitions.md#aud-07), [AUD-08](empire-and-acquisitions.md#aud-08), [AUD-15](snow-and-weather.md#aud-15), [A-04](empire-and-acquisitions.md#a-04), [A-05](empire-and-acquisitions.md#a-05)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
