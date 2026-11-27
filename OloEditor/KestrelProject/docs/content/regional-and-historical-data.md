# Regional and historical data

Status: consolidated v1.0 draft for repository adoption, 2026-11-27. Approved directions below are full-game design unless explicitly scoped; candidate algorithms, exact values and validation plans are not approved implementation mandates.

## Design contract

Use reproducible data processing for geography, climate, weather observations, snow reports, equipment history, architecture and cultural identity. Record source/version, spatial reference/resolution where relevant, transforms, attribution/license, uncertainties and authored changes. Distinguish documented facts from estimates and gameplay-authored values. Real-data preference does not imply exact reconstruction of existing ski resorts. Verify actual rights and coverage before shipping; fictional names, maps and event organizations are valid.

## Decision records

<a id="aud-20"></a>

### AUD-20

**Disposition:** Accepted.

Reproducible regional and historical content pipeline with source provenance, dataset versions, processing, uncertainty, documented gameplay adjustments and verified usage/redistribution rights. Real geography and data preferred where practical; fictional/inspired maps remain valid. Distinguish documented, estimated and gameplay-authored characteristics. Verify actual data and commercial asset rights before shipping; no selected additional dataset, map, brand or license implied.

## Validation and acceptance (proposed test focus)

Regenerate a region from recorded inputs, check source rights/attribution and assess climate calibration uncertainty.

## Related design decisions

[W-01](../gameplay/geography.md#w-01), [W-02](../gameplay/geography.md#w-02), [W-03](../gameplay/snow-and-weather.md#w-03), [W-14](../gameplay/geography.md#w-14), [U-13](../experience/art-direction.md#u-13)

See [decision index](../DECISIONS.md) and [open questions](../OPEN_QUESTIONS.md).
