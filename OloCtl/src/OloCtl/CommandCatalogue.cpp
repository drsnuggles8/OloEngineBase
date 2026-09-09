#include "OloCtl/CommandCatalogue.h"

#include <algorithm>
#include <utility>

namespace OloCtl
{
    namespace
    {
        using Json = nlohmann::json;

        // Must match Automation::kToolsetMetaKey. Spelled out rather than included
        // from the Automation header because this translation unit is the boundary
        // where oloctl stops depending on the engine tree at all — the executable
        // compiles against nothing but the standard library and nlohmann/json.
        // OloCtlCatalogueKeysTest pins the two spellings together.
        constexpr const char* kToolsetMetaKey = "io.oloengine/toolset";
        constexpr const char* kProjectWriteKey = "projectWrite";

        std::string StringField(const Json& entry, const char* key)
        {
            const auto it = entry.find(key);
            if (it == entry.end() || !it->is_string())
                return {};
            return it->get<std::string>();
        }

        std::string ReadToolset(const Json& entry)
        {
            const auto meta = entry.find("_meta");
            if (meta == entry.end() || !meta->is_object())
                return {};
            const auto toolset = meta->find(kToolsetMetaKey);
            if (toolset == meta->end() || !toolset->is_string())
                return {};
            return toolset->get<std::string>();
        }

        AuthorityClass ReadAuthority(const Json& entry)
        {
            const auto it = entry.find(kProjectWriteKey);
            if (it == entry.end() || !it->is_boolean())
                return AuthorityClass::Unknown;
            return it->get<bool>() ? AuthorityClass::ProjectWrite : AuthorityClass::ReadOnly;
        }
    } // namespace

    const char* ToString(AuthorityClass authority)
    {
        switch (authority)
        {
            case AuthorityClass::ReadOnly:
                return "read-only";
            case AuthorityClass::ProjectWrite:
                return "project-write";
            case AuthorityClass::Unknown:
                break;
        }
        return "unknown";
    }

    const CatalogueEntry* Catalogue::FindByName(const std::string& name) const
    {
        const auto it = std::find_if(Entries.begin(), Entries.end(),
                                     [&name](const CatalogueEntry& entry)
                                     { return entry.Name == name; });
        return it == Entries.end() ? nullptr : &*it;
    }

    Catalogue ParseCatalogue(const Json& entries, std::string source)
    {
        Catalogue catalogue;
        catalogue.Source = std::move(source);

        if (!entries.is_array())
        {
            catalogue.Rejected.push_back(
                { 0, {}, "the host returned a " + std::string(entries.type_name()) + " where an array of commands "
                                                                                     "was expected" });
            return catalogue;
        }

        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const Json& source_entry = entries[index];
            if (!source_entry.is_object())
            {
                catalogue.Rejected.push_back({ index, {}, "entry is a " + std::string(source_entry.type_name()) + ", not an object" });
                continue;
            }

            CatalogueEntry entry;
            entry.Name = StringField(source_entry, "name");
            if (entry.Name.empty())
            {
                catalogue.Rejected.push_back({ index, {}, "entry has no string `name`" });
                continue;
            }

            // A duplicate name would give the CLI two commands that dispatch to the
            // same place, and the host resolves by FIRST match (AutomationRegistry::Find)
            // — so keeping the first and reporting the rest mirrors what a call would
            // actually reach.
            if (catalogue.FindByName(entry.Name) != nullptr)
            {
                catalogue.Rejected.push_back(
                    { index, entry.Name, "duplicate name; the host resolves by first match, so this entry is "
                                         "unreachable" });
                continue;
            }

            entry.Title = StringField(source_entry, "title");
            entry.Description = StringField(source_entry, "description");
            entry.Toolset = ReadToolset(source_entry);
            entry.Authority = ReadAuthority(source_entry);

            const auto inputSchema = source_entry.find("inputSchema");
            entry.InputSchema = (inputSchema != source_entry.end() && inputSchema->is_object())
                                    ? *inputSchema
                                    : Json{ { "type", "object" } };

            const auto outputSchema = source_entry.find("outputSchema");
            if (outputSchema != source_entry.end() && outputSchema->is_object())
                entry.OutputSchema = *outputSchema;

            // `listed` is added by the host's registry-introspection surfaces
            // (olo_tool_search / olo_tool_describe). A plain tools/list carries no
            // such flag, and everything in a tools/list is by definition listed.
            const auto listed = source_entry.find("listed");
            entry.ListedByHost = (listed == source_entry.end() || !listed->is_boolean()) || listed->get<bool>();

            catalogue.Entries.push_back(std::move(entry));
        }

        return catalogue;
    }
} // namespace OloCtl
