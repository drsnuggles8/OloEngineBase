# Geography and land development

Status: consolidated v1.0 draft for repository adoption, 2026-11-27. Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Base official regions on real mountains and available environmental data wherever practical; fictional maps are valid. Elevation, slope, aspect, wind, geology, vegetation, water and regional regulations change what a resort can achieve. Land rights, concessions and technology govern expansion. Region extent follows geographical requirements, not an assumed fixed prototype size. Preserve real terrain proportions and restrict major earthworks through cost and feasibility.

Use data provenance and authored adjustments transparently. Do not present synthetic local snowfall or estimated historical prices as measured facts.

## Decision records

<a id="w-01"></a>

### W-01

**Disposition:** Accepted.

Geography is a strategic resource: realistic local snowfall, slope, elevation, aspect, wind exposure, geology, water and avalanche susceptibility affect development and operations.

<a id="w-02"></a>

### W-02

**Disposition:** Accepted.

Official maps should have distinct gameplay identities and be modeled on actual mountain data where possible; this builds on the existing real-terrain roadmap.

<a id="w-12"></a>

### W-12

**Disposition:** Accepted.

Abstract environmental impact and government/development relationships, with region-specific rules and expectations (e.g. Austria versus USA); avoid excessive micromanagement. No assertion that a particular country's actual laws have been modeled.

<a id="w-14"></a>

### W-14

**Disposition:** Accepted.

Real geographical data as the preferred starting point, with authored gameplay adjustments and fictional maps also supported. Data-driven maps and eventual community content are compatible; release/editor scope open.

## Validation and acceptance (proposed test focus)

Compare two contrasting regions; validate geography-based cost and operational differences, data coverage, licensing and reproducible processing.

## Related design decisions

[AUD-20](../content/regional-and-historical-data.md#aud-20), [C-04](construction.md#c-04), [C-06](construction.md#c-06), [A-03](empire-and-acquisitions.md#a-03), [U-02](../experience/UX.md#u-02)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
