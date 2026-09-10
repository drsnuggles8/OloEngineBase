#include "OloCtl/ArgumentBinder.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <map>
#include <utility>

namespace OloCtl
{
    namespace
    {
        using Json = nlohmann::json;

        struct Property
        {
            std::string Name;      // the JSON property name, as sent
            std::string Type;      // declared "type", or empty when none is declared
            std::string ItemsType; // for an array, the declared type of one element
        };

        std::string DeclaredType(const Json& schema)
        {
            if (!schema.is_object())
                return {};
            const auto type = schema.find("type");
            // A union type (`["string","null"]`) is a shape no single coercion rule
            // fits, so it is treated as undeclared and the value goes through the
            // JSON-then-string rule below.
            if (type == schema.end() || !type->is_string())
                return {};
            return type->get<std::string>();
        }

        Property DescribeProperty(const std::string& name, const Json& schema)
        {
            Property property{ name, DeclaredType(schema), {} };
            if (property.Type == "array" && schema.is_object())
            {
                const auto items = schema.find("items");
                if (items != schema.end())
                    property.ItemsType = DeclaredType(*items);
            }
            return property;
        }

        struct FlagMap
        {
            // Every accepted spelling -> the property it names.
            std::map<std::string, Property> Flags;
            // One spelling per property, for the "unknown option" message and for
            // --help. Exactly the spellings that work.
            std::vector<std::string> Accepted;
        };

        // Register both the kebab-cased and the verbatim spelling of every property.
        // The verbatim spelling always wins, so a schema that declares both `since-id`
        // and `sinceId` keeps `--since-id` pointing at the property actually called
        // that, and `--sinceId` at the other.
        FlagMap BuildFlagMap(const Json& inputSchema)
        {
            FlagMap map;
            const auto properties = inputSchema.find("properties");
            if (properties == inputSchema.end() || !properties->is_object())
                return map;

            for (const auto& [name, schema] : properties->items())
                map.Flags[name] = DescribeProperty(name, schema);
            for (const auto& [name, schema] : properties->items())
            {
                const std::string kebab = FlagSpelling(name);
                if (kebab != name)
                    map.Flags.emplace(kebab, DescribeProperty(name, schema));
            }

            // Advertise the spelling that resolves back to this property — which is the
            // kebab form unless another property claimed it verbatim.
            for (const auto& [name, schema] : properties->items())
            {
                const std::string kebab = FlagSpelling(name);
                const auto owner = map.Flags.find(kebab);
                map.Accepted.push_back(owner != map.Flags.end() && owner->second.Name == name ? kebab : name);
            }
            std::sort(map.Accepted.begin(), map.Accepted.end());
            return map;
        }

        std::string AcceptedList(const std::vector<std::string>& accepted)
        {
            if (accepted.empty())
                return "This command takes no arguments.";
            std::string out = "Accepted: ";
            for (std::size_t i = 0; i < accepted.size(); ++i)
            {
                if (i != 0)
                    out += ", ";
                out += "--" + accepted[i];
            }
            return out + '.';
        }

        bool ParseBool(const std::string& text, bool& out)
        {
            if (text == "true" || text == "1" || text == "yes" || text == "on")
            {
                out = true;
                return true;
            }
            if (text == "false" || text == "0" || text == "no" || text == "off")
            {
                out = false;
                return true;
            }
            return false;
        }

        // Coerce one command-line string into the declared JSON type. Returns false
        // with `outError` set. Never guesses a type it was not told about beyond the
        // documented JSON-then-string rule at the bottom.
        bool Coerce(const std::string& type, const std::string& flag, const std::string& text, Json& out,
                    std::string& outError)
        {
            if (type == "string")
            {
                out = text;
                return true;
            }
            if (type == "boolean")
            {
                bool value = false;
                if (!ParseBool(text, value))
                {
                    outError = "--" + flag + " expects a boolean (true/false), got '" + text + "'.";
                    return false;
                }
                out = value;
                return true;
            }
            if (type == "integer")
            {
                errno = 0;
                char* end = nullptr;
                const long long value = std::strtoll(text.c_str(), &end, 10);
                if (end == text.c_str() || *end != '\0' || errno == ERANGE)
                {
                    outError = "--" + flag + " expects an integer, got '" + text + "'.";
                    return false;
                }
                out = value;
                return true;
            }
            if (type == "number")
            {
                errno = 0;
                char* end = nullptr;
                const double value = std::strtod(text.c_str(), &end);
                if (end == text.c_str() || *end != '\0' || errno == ERANGE)
                {
                    outError = "--" + flag + " expects a number, got '" + text + "'.";
                    return false;
                }
                // CLAUDE.md -> Conventions: every float crossing a boundary is
                // finiteness-checked. 'nan' and 'inf' parse cleanly through strtod, so
                // without this they would travel to a renderer uniform.
                if (!std::isfinite(value))
                {
                    outError = "--" + flag + " expects a finite number, got '" + text + "'.";
                    return false;
                }
                out = value;
                return true;
            }
            if (type == "object")
            {
                Json parsed = Json::parse(text, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object())
                {
                    outError = "--" + flag + " expects a JSON object, got '" + text + "'.";
                    return false;
                }
                out = std::move(parsed);
                return true;
            }

            // No declared type, or one this CLI does not model. Try JSON, fall back to
            // the raw string. Documented in --help, so it is a rule rather than a guess.
            Json parsed = Json::parse(text, nullptr, false);
            out = parsed.is_discarded() ? Json(text) : std::move(parsed);
            return true;
        }

        // Does this token read as an option rather than a value?
        //
        // A VALUE MAY NEVER BEGIN WITH `--`. The bare-boolean branch has always
        // needed that test to avoid eating the next option; applying it to every
        // type closes the same hole for the rest, which is where it actually bites:
        // `--names --verbose-output` used to bind `names` to `["--verbose-output"]`,
        // silently drop `verboseOutput`, run the command with a wrong argument and
        // exit 0. A wrong answer wearing a right answer's exit code.
        //
        // Stricter than "is it a KNOWN flag": an unknown `--typo` after an option is
        // far more likely a mistyped flag than a value that happens to start with two
        // dashes, and swallowing it is the same silent failure. A genuine value with
        // a leading `--` stays reachable through `--flag=--value`.
        bool LooksLikeOption(const std::string& token)
        {
            return token.size() >= 3 && token.compare(0, 2, "--") == 0;
        }

        // One `--flag value` for an array-typed property. A whole JSON array replaces
        // the accumulator's contents-so-far element by element; anything else is one
        // element, coerced by the declared `items` type.
        bool AppendArrayValue(const Property& property, const std::string& flag, const std::string& text, Json& slot,
                              std::string& outError)
        {
            Json parsed = Json::parse(text, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_array())
            {
                for (Json& element : parsed)
                    slot.push_back(std::move(element));
                return true;
            }
            Json element;
            if (!Coerce(property.ItemsType, flag, text, element, outError))
                return false;
            slot.push_back(std::move(element));
            return true;
        }
    } // namespace

    std::string FlagSpelling(const std::string& property)
    {
        std::string out;
        out.reserve(property.size() + 4);
        for (std::size_t i = 0; i < property.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(property[i]);
            if (c == '_')
            {
                out.push_back('-');
                continue;
            }
            if (std::isupper(c) != 0)
            {
                if (i != 0 && !out.empty() && out.back() != '-')
                    out.push_back('-');
                out.push_back(static_cast<char>(std::tolower(c)));
                continue;
            }
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    BindResult BindArguments(const Json& inputSchema, const std::vector<std::string>& tokens, Json base)
    {
        BindResult result;
        result.Arguments = base.is_object() ? std::move(base) : Json::object();

        const FlagMap map = BuildFlagMap(inputSchema);
        std::vector<std::string> seen;

        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            const std::string& token = tokens[i];
            if (token.size() < 3 || token.compare(0, 2, "--") != 0)
            {
                result.Error = "Unexpected argument '" + token + "'. Arguments are named: --<option> <value>. " +
                               AcceptedList(map.Accepted);
                return result;
            }

            std::string flag = token.substr(2);
            std::string inlineValue;
            bool hasInlineValue = false;
            if (const std::size_t equals = flag.find('='); equals != std::string::npos)
            {
                inlineValue = flag.substr(equals + 1);
                flag = flag.substr(0, equals);
                hasInlineValue = true;
            }

            bool negated = false;
            auto found = map.Flags.find(flag);
            if (found == map.Flags.end() && flag.compare(0, 3, "no-") == 0)
            {
                const auto positive = map.Flags.find(flag.substr(3));
                if (positive != map.Flags.end() && positive->second.Type == "boolean")
                {
                    found = positive;
                    negated = true;
                }
            }
            if (found == map.Flags.end())
            {
                result.Error = "Unknown option '--" + flag + "'. " + AcceptedList(map.Accepted);
                return result;
            }

            const Property& property = found->second;
            const bool repeated = std::find(seen.begin(), seen.end(), property.Name) != seen.end();
            if (repeated && property.Type != "array")
            {
                result.Error = "--" + flag + " was given more than once, and it is not an array argument.";
                return result;
            }
            if (!repeated)
                seen.push_back(property.Name);

            // Resolve the value text first; the two valueless spellings short-circuit.
            std::string text;
            bool hasText = false;
            if (negated)
            {
                if (hasInlineValue)
                {
                    result.Error = "--no-" + FlagSpelling(property.Name) + " takes no value.";
                    return result;
                }
            }
            else if (property.Type == "boolean" && !hasInlineValue &&
                     (i + 1 >= tokens.size() || LooksLikeOption(tokens[i + 1])))
            {
                // A bare boolean flag means true. The next token is only consumed when
                // it is plainly a value rather than the next option.
            }
            else if (hasInlineValue)
            {
                text = inlineValue;
                hasText = true;
            }
            else if (i + 1 < tokens.size() && !LooksLikeOption(tokens[i + 1]))
            {
                text = tokens[++i];
                hasText = true;
            }
            else
            {
                result.Error = "--" + flag + " expects a value";
                if (i + 1 < tokens.size())
                {
                    result.Error += ", and '" + tokens[i + 1] +
                                    "' reads as another option. Write --" + flag + "=" + tokens[i + 1] +
                                    " if it really is the value";
                }
                result.Error += '.';
                return result;
            }

            if (property.Type == "array")
            {
                Json& slot = result.Arguments[property.Name];
                // An explicit flag owns the key: whatever --arguments-json put there is
                // replaced by the flags, which then accumulate among themselves.
                if (!repeated || !slot.is_array())
                    slot = Json::array();
                if (!AppendArrayValue(property, flag, text, slot, result.Error))
                    return result;
                continue;
            }

            Json value;
            if (!hasText)
            {
                value = !negated;
            }
            else if (!Coerce(property.Type, flag, text, value, result.Error))
            {
                return result;
            }
            result.Arguments[property.Name] = std::move(value);
        }

        result.Ok = true;
        return result;
    }
} // namespace OloCtl
