#include "OloEnginePCH.h"
#include "Automation/AutomationSchemaValidation.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace OloEngine::Automation::AutomationSchema
{
    namespace
    {
        using Json = nlohmann::json;

        // ---- argument validation (issue #357 / #306) --------------------
        //
        // A deliberately small JSON-Schema-subset validator covering exactly what
        // the schema-builder DSL (McpSchemaBuilder.h) can emit — see
        // AutomationSchema::ValidateArguments for the public contract. Everything here is
        // pure (Json + stdlib only) so the dispatch test binary exercises it
        // without an editor.

        // Render a JSON-Schema `type` token as an English noun phrase for messages.
        std::string SchemaTypeNoun(std::string_view type)
        {
            if (type == "integer")
                return "an integer";
            if (type == "number")
                return "a number";
            if (type == "string")
                return "a string";
            if (type == "boolean")
                return "a boolean";
            if (type == "array")
                return "an array";
            if (type == "object")
                return "an object";
            if (type == "null")
                return "null";
            return std::string(type);
        }

        // True if `value` satisfies the single JSON-Schema `type` token. "integer"
        // is strict (a JSON integer, not a 5.0-style float) — deliberate hardening:
        // every integer field the DSL declares (count / page / pageSize / bounds)
        // is sent as an integer by any sane client, and a strict check needs no
        // float-equality test (which SonarQube flags). "number" accepts either.
        bool ValueMatchesSchemaType(const Json& value, std::string_view type)
        {
            if (type == "object")
                return value.is_object();
            if (type == "array")
                return value.is_array();
            if (type == "string")
                return value.is_string();
            if (type == "boolean")
                return value.is_boolean();
            if (type == "number")
                return value.is_number(); // integer or float
            if (type == "integer")
                return value.is_number_integer();
            if (type == "null")
                return value.is_null();
            return true; // an unmodelled type keyword: don't reject what we can't check
        }

        // "'parent.child'"-style dotted path for nested fields; just "child" at the
        // top level (empty parent). Keeps an error pointing at the exact field.
        std::string JoinFieldPath(const std::string& parent, std::string_view name)
        {
            if (parent.empty())
                return std::string(name);
            return parent + "." + std::string(name);
        }

        // A quoted subject for a message: "'count'", or "value" for the unnamed
        // top-level object (which in practice always matches, so it rarely shows).
        std::string FieldSubject(const std::string& label)
        {
            return label.empty() ? std::string("value") : "'" + label + "'";
        }

        // Validate `value` against `schema` (`label` names `value` for messages),
        // returning the first error or nullopt. Forward-declared so the per-keyword
        // checks below can recurse into nested properties / array items. A non-object
        // schema is permissive (nothing modelled to enforce). Each keyword lives in
        // its own small checker so the orchestrator stays flat and easy to audit.
        std::optional<std::string> ValidateValueAgainstSchema(const Json& schema, const Json& value,
                                                              const std::string& label);

        // `type`: a single token, or an array of tokens (a union — match any one).
        std::optional<std::string> CheckType(const Json& schema, const Json& value, const std::string& label)
        {
            const auto it = schema.find("type");
            if (it == schema.end())
                return std::nullopt;

            if (it->is_string())
            {
                const std::string type = it->get<std::string>();
                if (!ValueMatchesSchemaType(value, type))
                    return FieldSubject(label) + " must be " + SchemaTypeNoun(type);
                return std::nullopt;
            }
            if (!it->is_array())
                return std::nullopt; // malformed `type` — not modelled

            bool matched = false;
            std::string nouns;
            for (const auto& entry : *it)
            {
                if (!entry.is_string())
                    continue;
                const std::string type = entry.get<std::string>();
                matched = matched || ValueMatchesSchemaType(value, type);
                if (!nouns.empty())
                    nouns += " or ";
                nouns += SchemaTypeNoun(type);
            }
            if (matched || nouns.empty())
                return std::nullopt;
            return FieldSubject(label) + " must be " + nouns;
        }

        // `enum`: the value must equal one of the allowed entries.
        std::optional<std::string> CheckEnum(const Json& schema, const Json& value, const std::string& label)
        {
            const auto it = schema.find("enum");
            if (it == schema.end() || !it->is_array())
                return std::nullopt;
            for (const auto& allowed : *it)
            {
                if (value == allowed)
                    return std::nullopt;
            }
            return FieldSubject(label) + " must be one of " + it->dump();
        }

        // `minimum` / `maximum` / `exclusiveMinimum` — only meaningful once the value
        // is a number. The comparisons are relational, not equality, so float-clean.
        std::optional<std::string> CheckNumericBounds(const Json& schema, const Json& value, const std::string& label)
        {
            if (!value.is_number())
                return std::nullopt;
            const double n = value.get<double>();
            // A non-finite number (NaN / ±Inf) slips past the relational bound checks
            // below — every comparison against NaN is false, and +Inf clears any
            // schema without a `maximum` — and is never valid input. Reject it up
            // front (the project rule to isfinite-validate floats from JSON/network).
            if (!std::isfinite(n))
                return FieldSubject(label) + " must be a finite number";
            if (const auto it = schema.find("minimum"); it != schema.end() && it->is_number() && n < it->get<double>())
                return FieldSubject(label) + " must be >= " + it->dump();
            if (const auto it = schema.find("maximum"); it != schema.end() && it->is_number() && n > it->get<double>())
                return FieldSubject(label) + " must be <= " + it->dump();
            if (const auto it = schema.find("exclusiveMinimum");
                it != schema.end() && it->is_number() && n <= it->get<double>())
                return FieldSubject(label) + " must be > " + it->dump();
            return std::nullopt;
        }

        // `minLength` / `maxLength` for string-valued tool arguments.
        std::optional<std::string> CheckStringConstraints(const Json& schema, const Json& value,
                                                          const std::string& label)
        {
            if (!value.is_string())
                return std::nullopt;

            // nlohmann validates UTF-8 while parsing; counting non-continuation
            // bytes therefore yields JSON Schema's Unicode code-point length.
            const std::string& text = value.get_ref<const std::string&>();
            const auto length = static_cast<std::int64_t>(std::ranges::count_if(
                text, [](char byte)
                { return (static_cast<unsigned char>(byte) & 0xC0u) != 0x80u; }));
            if (const auto it = schema.find("minLength");
                it != schema.end() && it->is_number_integer() && length < it->get<std::int64_t>())
                return FieldSubject(label) + " must have at least " + it->dump() + " characters";
            if (const auto it = schema.find("maxLength");
                it != schema.end() && it->is_number_integer() && length > it->get<std::int64_t>())
                return FieldSubject(label) + " must have at most " + it->dump() + " characters";
            return std::nullopt;
        }

        // `required` + `additionalProperties` + per-property recursion.
        std::optional<std::string> CheckObjectConstraints(const Json& schema, const Json& value,
                                                          const std::string& label)
        {
            if (!value.is_object())
                return std::nullopt;

            const Json* properties = nullptr;
            if (const auto it = schema.find("properties"); it != schema.end() && it->is_object())
                properties = &(*it);

            if (const auto it = schema.find("required"); it != schema.end() && it->is_array())
            {
                for (const auto& entry : *it)
                {
                    if (!entry.is_string())
                        continue;
                    const std::string name = entry.get<std::string>();
                    if (!value.contains(name))
                        return "missing required property '" + JoinFieldPath(label, name) + "'";
                }
            }

            const auto addlIt = schema.find("additionalProperties");
            const bool closed = addlIt != schema.end() && addlIt->is_boolean() && !addlIt->get<bool>();
            const Json* addlSchema = (addlIt != schema.end() && addlIt->is_object()) ? &(*addlIt) : nullptr;

            for (const auto& [key, member] : value.items())
            {
                if (properties != nullptr && properties->contains(key))
                {
                    if (auto err = ValidateValueAgainstSchema((*properties)[key], member, JoinFieldPath(label, key)))
                        return err;
                }
                else if (closed)
                    return "unexpected property '" + JoinFieldPath(label, key) + "'";
                else if (addlSchema != nullptr)
                {
                    if (auto err = ValidateValueAgainstSchema(*addlSchema, member, JoinFieldPath(label, key)))
                        return err;
                }
            }
            return std::nullopt;
        }

        // `minItems` / `maxItems` + per-element `items` recursion.
        std::optional<std::string> CheckArrayConstraints(const Json& schema, const Json& value,
                                                         const std::string& label)
        {
            if (!value.is_array())
                return std::nullopt;

            const auto count = static_cast<std::int64_t>(value.size());
            if (const auto it = schema.find("minItems");
                it != schema.end() && it->is_number_integer() && count < it->get<std::int64_t>())
                return FieldSubject(label) + " must have at least " + it->dump() + " items";
            if (const auto it = schema.find("maxItems");
                it != schema.end() && it->is_number_integer() && count > it->get<std::int64_t>())
                return FieldSubject(label) + " must have at most " + it->dump() + " items";

            const auto it = schema.find("items");
            if (it == schema.end() || !it->is_object())
                return std::nullopt;
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                const std::string elemLabel =
                    (label.empty() ? std::string("value") : label) + "[" + std::to_string(i) + "]";
                if (auto err = ValidateValueAgainstSchema(*it, value[i], elemLabel))
                    return err;
            }
            return std::nullopt;
        }

        std::optional<std::string> ValidateValueAgainstSchema(const Json& schema, const Json& value,
                                                              const std::string& label)
        {
            if (!schema.is_object())
                return std::nullopt;
            if (auto err = CheckType(schema, value, label))
                return err;
            if (auto err = CheckEnum(schema, value, label))
                return err;
            if (auto err = CheckNumericBounds(schema, value, label))
                return err;
            if (auto err = CheckStringConstraints(schema, value, label))
                return err;
            if (auto err = CheckObjectConstraints(schema, value, label))
                return err;
            return CheckArrayConstraints(schema, value, label);
        }
    } // namespace

    std::optional<std::string> ValidateArguments(const Json& schema, const Json& args)
    {
        // An absent / non-object / empty schema declares no constraints, so there is
        // nothing to enforce — treat it as permissive (callers also skip such
        // schemas, but guarding here keeps the helper safe to call unconditionally).
        if (!schema.is_object() || schema.empty())
            return std::nullopt;
        return ValidateValueAgainstSchema(schema, args, std::string{});
    }
} // namespace OloEngine::Automation::AutomationSchema
