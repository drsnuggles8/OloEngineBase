# Localization

OloEngine's localization layer covers the spectrum from "swap UI strings"
through "render mixed-script text" — all driven by a small set of YAML
files under `assets/localization/`.

> **Architecture:** see [agent-rules/testing-architecture.md](agent-rules/testing-architecture.md)
> for how the localization tests fit the engine's two test axes. This doc
> is the user-facing reference for shipping a localized game.

---

## TL;DR

1. Drop one `<code>.ololocale` YAML file per language under
   `assets/localization/`.
2. Anywhere user-facing text lives in the engine, replace the literal with
   either a `LocalizationKey` (UI text via `LocalizedTextComponent`) or an
   `@key:<key>` prefix (dialogue / quest / item single-string fields), and
   call `LocalizationManager::Get(key)` from C++ / `Localization.Get(key)`
   from Lua / C#.
3. The first time the game runs, it negotiates the player's OS locale
   against what you shipped. Subsequent runs restore the last selection
   from `userprefs/locale.yaml`.

The rest of this document is the long version.

---

## The `.ololocale` file format

A complete locale file (German, with every supported metadata field):

```yaml
locale: de
name: Deutsch
direction: ltr                  # ltr | rtl (RTL rendering is a documented no-op today, see "Known limitations")
plural_rule: one-other          # one-other | other-only | french | polish | russian | arabic
thousand_separator: "."
decimal_separator: ","
currency_symbol: "€"
currency_symbol_before: false
currency_decimals: 2
list_joiner: ", "
list_last_joiner: " und "
font: "assets/fonts/NotoSans-Regular.ttf"
font_fallbacks:
  - "assets/fonts/NotoSansJP-Regular.ttf"

strings:
  ui.main_menu.play: "Spielen"
  ui.main_menu.settings: "Einstellungen"

  combat.damage_dealt: "Du hast {target} {damage} Schaden zugefügt."
  combat.kills: "Du hast {count} {count:Feind|Feinde} besiegt."

  # Translator-facing per-key metadata. Use the map shape when you want
  # context or max-length; the simple `key: "value"` shape stays valid
  # for keys that don't need extras.
  ui.button.play:
    value: "Wiedergeben"
    context: "Main menu primary CTA"
    max_length: 16
```

Every metadata field is optional except `locale` and `strings`. Sensible
defaults are documented in [LocaleDefinition.h](../OloEngine/src/OloEngine/Localization/LocaleDefinition.h).

---

## Pattern syntax in `strings:`

`TextFormatter` understands four token shapes inside a value:

| Token                       | Meaning                                          | Example                                        |
|-----------------------------|--------------------------------------------------|------------------------------------------------|
| `{name}`                    | Substitute `params["name"]`                      | `"You dealt {damage} damage."`                  |
| `{name:a\|b\|c\|...}`         | Plural selection (integer value) / gender selection (string value) | `"{count:enemy\|enemies}"` / `"{gender:le\|la}"` |
| `{name:label=form\|...\|else=fallback}` | `select` token — labelled dispatch     | `"{role:warrior=knight\|mage=wizard\|else=hero}"`|
| `{{` / `}}`                  | Literal `{` / `}`                                | `"Press {{Esc}} to exit"`                      |

Disambiguation: if any `|`-separated segment contains `=`, the token is
parsed as a labelled `select`; otherwise it's positional (plurals / gender).

**Plural rules** map a numeric value to a form-list index per CLDR
conventions. The supported rules (see [LocaleDefinition.h](../OloEngine/src/OloEngine/Localization/LocaleDefinition.h)):

| Rule          | Forms                          | Languages              |
|---------------|--------------------------------|------------------------|
| `one-other`   | `[one, other]`                 | en, de, it, es, pt     |
| `other-only`  | `[other]`                      | ja, zh, ko, vi, th     |
| `french`      | `[one, other]` (0+1 collapse)  | fr                     |
| `polish`      | `[one, few, many]`             | pl, cs, sk             |
| `russian`     | `[one, few, many, other]`      | ru, uk, sr, hr         |
| `arabic`      | `[zero, one, two, few, many, other]` | ar                |

**Gender** is selected against well-known string values: `masculine` /
`male` / `m` / `M` → form 0, `feminine` / `female` / `f` / `F` → form 1,
`neuter` / `none` / `other` / `n` / `N` → form 2. Same positional form-list
as plural.

---

## C++ API surface

All of this is in [LocalizationManager.h](../OloEngine/src/OloEngine/Localization/LocalizationManager.h). The hot path is
`Get` / `Format` — everything else is editor or shipping support.

```cpp
// Lookup + formatting
LocalizationManager::Get(key)
LocalizationManager::Format(key, params)
LocalizationManager::FormatPlural(key, countParam, count, params={})
LocalizationManager::HasKey(key)

// Per-locale overrides (bypass active-locale, mainly for editor tooling)
LocalizationManager::Get(key, localeCode)
LocalizationManager::HasKey(key, localeCode)
LocalizationManager::GetAllKeys(localeCode)
LocalizationManager::GetMetadata(key, localeCode)

// Locale management
LocalizationManager::Initialize(localizationDir)
LocalizationManager::LoadLocale(filePath)
LocalizationManager::SetCurrentLocale(code)
LocalizationManager::GetCurrentLocale()
LocalizationManager::GetAvailableLocales()
LocalizationManager::ReloadCurrentLocale()
LocalizationManager::NegotiateLocale(preferences={})  // empty → GetSystemPreferredLocales
LocalizationManager::GetSystemPreferredLocales()
LocalizationManager::SaveActiveLocaleToFile(path)
LocalizationManager::LoadActiveLocaleFromFile(path)

// Editor tooling
LocalizationManager::SetKey(localeCode, key, value)
LocalizationManager::SaveLocaleToFile(localeCode, pathOverride={})
LocalizationManager::GetGeneration()  // bumps on any state change

// Runtime QA
LocalizationManager::GetMissingKeysSnapshot()
LocalizationManager::ClearMissingKeys()
LocalizationManager::GeneratePseudoLocale(sourceCode="en", pseudoCode="pseudo")
LocalizationManager::SetMissingKeyFallback(fallback)

// Number / currency / list
LocalizationManager::FormatNumber(i64 value, localeCode={})
LocalizationManager::FormatNumber(f64 value, decimals=2, localeCode={})
LocalizationManager::FormatCurrency(amount, localeCode={}, symbolOverride={})
LocalizationManager::FormatList(items, localeCode={})

// Date / time
LocalizationManager::FormatDate(tp, DateStyle, localeCode={})
LocalizationManager::FormatTime(tp, TimeStyle, localeCode={})
LocalizationManager::FormatRelativeTime(tp, localeCode={})

// Generic "string-or-key" dispatch for Quest/Item-style POD fields
LocalizationManager::ResolveLocalizedText(value)  // "@key:foo" → looked up; else passes through
LocalizationManager::ResolveLocalizedAssetPath(basePath, localeCode={})
```

---

## ECS integration

`LocalizedTextComponent` flags a UI text entity for auto-localization:

```yaml
- LocalizedTextComponent:
    LocalizationKey: ui.main_menu.play
```

[LocalizationSystem::UpdateLocalizedText](../OloEngine/src/OloEngine/Localization/LocalizationSystem.cpp) runs each tick (cheap O(1) check; only
walks entities when the manager generation has bumped). On change, it
rewrites the entity's `TextComponent.TextString` and, if the locale
declares a font, swaps `TextComponent.FontAsset` to the locale's primary
font (with the fallback chain already attached).

For single-string fields on POD types (`Quest::Title`, `Item::DisplayName`,
dialogue choice port names), use the `@key:` prefix convention:

```yaml
QuestDefinition:
  QuestID: main_01
  Title: "@key:quest.main_01.title"
  Description: "@key:quest.main_01.desc"
```

Callers that render those fields wrap the read in
`LocalizationManager::ResolveLocalizedText(field)`, which returns the
key-resolved value when the prefix is present and the literal otherwise.

---

## Scripting API

Both bindings mirror the C++ surface 1:1.

**Lua:**

```lua
Localization.Get(key)
Localization.Format(key, { damage = 42 })
Localization.FormatPlural(key, "count", 5, {})
Localization.SetLocale("de")
Localization.GetCurrentLocale()
Localization.HasKey(key)
Localization.GetAvailableLocales()
Localization.ResolveLocalizedText(value)
Localization.FormatNumber(1234)
Localization.FormatCurrency(1234.5)
Localization.FormatList({"apples", "oranges", "pears"})
Localization.FormatDate(0, 1)              -- 0 = now, 1 = DateStyle::Medium
Localization.FormatTime(0, 0)              -- 0 = now, 0 = TimeStyle::Short
Localization.FormatRelativeTime(epoch_seconds)
Localization.GetMissingKeys()
Localization.ClearMissingKeys()
Localization.GeneratePseudoLocale()
```

**C#:**

```csharp
Localization.Get(key)
Localization.Format(key, new Dictionary<string,string>{ ["damage"]="42" })
Localization.FormatPlural(key, "count", 5)
Localization.SetLocale("de")
Localization.GetCurrentLocale()
Localization.HasKey(key)
Localization.ResolveLocalizedText(value)
Localization.FormatNumber(1234L)
Localization.FormatCurrency(1234.5)
Localization.FormatList(items)
Localization.FormatDate(DateTimeOffset.Now, Localization.DateStyle.Medium)
Localization.FormatTime(DateTimeOffset.Now, Localization.TimeStyle.Short)
Localization.FormatRelativeTime(when)
Localization.ClearMissingKeys()
Localization.GeneratePseudoLocale()
```

---

## Workflow

### Authoring a new locale

1. Copy `en.ololocale` to `<code>.ololocale`.
2. Set `locale:` and `name:` to the new code/display name.
3. Set the right `plural_rule:` (see table above).
4. Adjust `thousand_separator` / `decimal_separator` / `currency_*` /
   `list_*joiner` if they differ from defaults.
5. Translate every string. Leave keys you haven't tackled yet *out* —
   `LocalizationManager` returns the fallback (default `"???"`) and the
   editor's reports tab will tell you what's missing.

### Translator workflow (CSV)

`LocalizationCsv::ExportToCsv(path)` flattens every loaded locale into a
single CSV with one column per locale (`key, en, de, ...`). The translator
opens it in a spreadsheet, fills the empty cells, and you import via
`LocalizationCsv::ImportFromCsv(path)`. RFC-4180 quoting handles commas /
quotes / newlines inside cell content. UTF-8 BOM tolerated.

### Editor

`Tools → Localization` opens [LocalizationPanel](../OloEditor/src/Panels/LocalizationPanel.cpp). It provides:

- Active-locale dropdown
- Source-locale dropdown (the reference for missing-key highlighting)
- Substring key filter + "missing only" toggle
- Inline cell editing of any translation (writes through `SetKey` —
  visible everywhere immediately via the generation counter)
- "Save edits to disk" button that round-trips back to the per-locale
  `.ololocale` file
- "Generate pseudo-locale" button (synthesizes the dev-only "pseudo"
  locale; see [Pseudo-localization](#pseudo-localization))
- "Reports" section showing both the parameter-drift lint and the
  runtime missing-key accumulator

### Hot-reload

The editor's filewatch listens for `.ololocale` changes in the project's
asset directory and routes them through
`LocalizationManager::LoadLocale(absolutePath)`. The generation counter
bumps, so all observers (editor panel, runtime UI) refresh on the next
tick. No restart required.

---

## QA tooling

### Pseudo-localization

[GeneratePseudoLocale](../OloEngine/src/OloEngine/Localization/LocalizationManager.cpp) snapshots the source locale, wraps every value with
`[!! Ḧëļļö !!]` markers, and substitutes ASCII letters with Latin-Extended
diacritics. Parameter tokens (`{name}`, `{count:a|b}`) pass through verbatim
so formatting still works.

What this catches:

1. **Hardcoded strings.** Any user-facing text that doesn't have the
   `[!! ... !!]` wrapper is *not* going through `LocalizationManager`.
2. **Length expansion bugs.** Diacritic substitution adds ~30 % bytes per
   character — close to the typical English → German ratio. Run the game
   with `pseudo` active and you'll spot every UI widget that clips.
3. **Encoding bugs.** Forces the renderer through its UTF-8 multi-byte
   path; ASCII-only assumptions break visibly.

Toolbar button in the LocalizationPanel; also accessible via
`LocalizationManager::GeneratePseudoLocale()`.

### Missing-key accumulator

Every `Get` / `Format` call that misses the active locale records the key
in an internal set. The Reports tab shows the live list; the
"Clear missing-key list" button resets it between QA passes.

### Parameter-drift lint

[LocalizationLint::RunParameterDriftLint](../OloEngine/src/OloEngine/Localization/LocalizationLint.cpp) extracts the `{name}` token set
from each translation and diffs against the source locale. Translators
who drop `{target}` or accidentally add `{n}` show up immediately. The
panel's Reports tab surfaces the findings.

### Max-length lint

[LocalizationLint::RunMaxLengthLint](../OloEngine/src/OloEngine/Localization/LocalizationLint.cpp) walks every key the source locale
declares a `max_length` for, measures the corresponding translation in
**Unicode codepoints** (not bytes), and reports overflows.

---

## Shipping

### Asset-pack bundling

`AssetPackBuilder::BuildSettings::m_IncludeLocalizationFiles` (default
`true`) copies every `.ololocale` from `assets/localization/` into
`<output dir>/assets/localization/` alongside the `.olopack`. The shipped
game calls `LocalizationManager::Initialize("assets/localization")` at
boot and finds them there.

### First-launch locale

```cpp
LocalizationManager::Initialize("assets/localization");
if (!LocalizationManager::LoadActiveLocaleFromFile("userprefs/locale.yaml"))
{
    // First launch — negotiate against the OS locale list.
    const std::string negotiated = LocalizationManager::NegotiateLocale();
    if (!negotiated.empty())
        LocalizationManager::SetCurrentLocale(negotiated);
}
```

At shutdown:

```cpp
LocalizationManager::SaveActiveLocaleToFile("userprefs/locale.yaml");
```

The editor does both automatically.

### Locale-aware asset variants

`LocalizationManager::ResolveLocalizedAssetPath("assets/ui/logo.png")`
returns `assets/ui/logo.de.png` if it exists for the active locale,
otherwise the base path unchanged. BCP-47 region codes fall back to the
language code (`de-AT` → `de.png`). Used for art assets with baked-in
text and localized voice-over.

---

## Known limitations

Nothing here blocks shipping a Latin / Cyrillic / Greek / Hebrew / CJK
game; they're flagged so an Arabic or Indic project knows the work it'd
take to land.

- **Full Unicode Bidirectional Algorithm.** `TextDirection::RTL` and
  `Renderer2D::TextParams::RightToLeft` are wired through but not applied.
  The renderer iterates codepoints left-to-right in storage order. Hebrew
  works because it doesn't need joining; Arabic doesn't because the
  shaping pass (isolated/initial/medial/final forms) isn't implemented.
- **OpenType shaping.** Kerning across fallback fonts, contextual
  alternates, ligatures beyond simple advance pairs — none are honoured.
  Vendoring HarfBuzz would address both this and the BiDi gap.
- **`AssetManager` integration.** `.ololocale` files aren't registered as
  asset-manager assets — `LocalizationManager` owns them directly. The
  trade-off: they don't participate in `AssetHandle` lookup, but they
  also don't pay the asset-registry round-trip overhead and have their
  own dedicated hot-reload path.
