/*
Copyright (c) 2025, 2026 acrion innovations GmbH
Authors: Stefan Zipproth, s.zipproth@acrion.ch

This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org

zelph is offered under a commercial and under the AGPL license.
For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.

AGPL licensing:

zelph is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

zelph is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with zelph. If not, see <https://www.gnu.org/licenses/>.
*/

#include "wikidata.hpp"

#include "import_diagnostics.hpp"
#include "io/read_async.hpp"
#include "platform/platform_utils.hpp"
#include "string/node_to_string.hpp"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <kj/io.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip> // for std::setprecision
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace zelph::wikidata;
using namespace std::string_literals;
using std::chrono::duration_cast;

// #define SINGLE_THREADED_IMPORT

class Wikidata::Impl
{
public:
    Impl(network::Zelph* n, const std::filesystem::path& original_source_path)
        : _n(n)
        , _original_source_path(original_source_path)
    {
    }

    network::Zelph*       _n{nullptr};
    std::filesystem::path _original_source_path;
    std::recursive_mutex  _mtx_read;
#ifdef SINGLE_THREADED_IMPORT
    std::mutex _mtx_network;
#endif
    bool                  _logging{true};
    std::filesystem::path _bin_path;
};

Wikidata::Wikidata(network::Zelph* n, const std::filesystem::path& input_path)
    : DataManager(n, input_path)
    , _pImpl(new Impl(n, resolve_original_source_path(input_path))) // Use the base class static helper
{
    // Logic from previous constructor regarding .bin path calculation
    // Validate input logic is partly handled by factory, but we double check or handle specific logic here.

    // Calculate the path for the binary cache (.bin).
    std::filesystem::path bin_path = input_path;

    if (bin_path.extension() == ".bz2")
    {
        bin_path.replace_extension("");
    }

    bin_path.replace_extension(".bin");
    _pImpl->_bin_path = bin_path;
}

Wikidata::~Wikidata()
{
    delete _pImpl;
}

void Wikidata::load()
{
    import_all();
}

void Wikidata::import_all(const std::string& constraints_dir)
{
    const bool export_constraints = !constraints_dir.empty();

    if (!export_constraints)
    {
        _pImpl->_n->diagnostic_stream() << "Number of nodes prior import: " << _pImpl->_n->count() << std::endl;
    }

    std::filesystem::path cache_file = _pImpl->_bin_path;

    bool cache_loaded = false;
    // Attempt to load from cache if we are not just exporting constraints
    if (!export_constraints)
    {
        if (std::filesystem::exists(cache_file))
        {
            try
            {
                _pImpl->_n->diagnostic("Loading network from " + cache_file.string() + "...", true);
                _pImpl->_n->load_from_file(_pImpl->_bin_path.string());
                _pImpl->_n->diagnostic("Cache loaded successfully.", true);
                cache_loaded = true;
            }
            catch (std::exception& ex)
            {
                _pImpl->_n->diagnostic("Failed to load cache: "s + ex.what(), true);
            }
        }
    }

    // If we need to process the original source (either for export or because cache failed/missing)
    if (export_constraints || !cache_loaded)
    {
        // Check if source exists
        if (_pImpl->_original_source_path.empty())
        {
            // This happens if initialized with a .bin file, but the corresponding source file wasn't found.
            throw std::runtime_error("Operation requires original source file, but it could not be located based on the input path.");
        }

        const std::string additional_language_to_import = "en";

        if (export_constraints)
        {
            _pImpl->_n->diagnostic("Exporting constraints from file " + _pImpl->_original_source_path.string(), true);
        }
        else
        {
            _pImpl->_n->diagnostic("Importing file " + _pImpl->_original_source_path.string(), true);
        }

        const unsigned int num_threads = 4; // std::thread::hardware_concurrency();

        io::ReadAsync read_async(_pImpl->_original_source_path, num_threads * 2, [this](const std::string& msg)
                                 { _pImpl->_n->diagnostic(msg, true); });

        if (!read_async.error_text().empty())
        {
            throw std::runtime_error(read_async.error_text());
        }

        const std::streamsize total_size = read_async.get_total_size();

        size_t baseline_memory = zelph::platform::get_process_memory_usage();

        // Atomic counters for thread coordination and progress tracking
        std::atomic<std::streamoff> bytes_read{0};
        std::atomic<unsigned int>   active_threads{num_threads};
        std::vector<std::thread>    workers;
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        ImportDiagnostics import_diag;
#endif
        const bool log = false;

        // Worker function: each thread processes lines from ReadAsync
        auto worker_func = [&]()
        {
            std::vector<std::pair<std::string, std::streamoff>> batch;
            ImportThreadStats                                   local_diag;

            while (true)
            {
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                const auto wait_begin = SteadyClock::now();
#endif
                if (!read_async.get_batch(batch))
                    break;
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                local_diag.wait_batch_ns += to_ns(SteadyClock::now() - wait_begin);
                local_diag.batches += 1;
#endif

                for (auto& [line, streampos] : batch)
                {
                    auto old = bytes_read.load(std::memory_order_relaxed);
                    while (streampos > old
                           && !bytes_read.compare_exchange_weak(old, streampos, std::memory_order_relaxed))
                        ;

                    process_entry(line, additional_language_to_import, log, constraints_dir, &local_diag);
                }

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                import_diag.merge(local_diag, batch.size());
                local_diag.reset();
#endif
            }

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
            import_diag.merge(local_diag, 0);
#endif
            active_threads.fetch_sub(1, std::memory_order_relaxed);
        };

        // Start worker threads
        workers.reserve(num_threads);
        for (unsigned int i = 0; i < num_threads; ++i)
        {
            workers.emplace_back(worker_func);
        }

        // Progress reporting in main thread
        std::chrono::steady_clock::time_point start_time       = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_update_time = start_time;

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        std::chrono::steady_clock::time_point last_diag_time     = start_time;
        ImportDiagSnapshot                    last_diag_snapshot = import_diag.snapshot();
        io::ReadAsync::StatsSnapshot          last_read_snapshot = read_async.get_stats_snapshot();
        uint64_t                              last_cpu_ns        = zelph::platform::get_process_cpu_time_ns();
#endif

        while (active_threads.load(std::memory_order_relaxed) > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            auto current_time           = std::chrono::steady_clock::now();
            auto time_since_last_update = std::chrono::duration<double>(current_time - last_update_time).count();

            if (time_since_last_update >= 60.0)
            {
                std::streamoff current_bytes      = bytes_read.load(std::memory_order_relaxed);
                double         current_percentage = (static_cast<double>(current_bytes) / static_cast<double>(total_size)) * 100.0;
                double         progress_fraction  = static_cast<double>(current_bytes) / static_cast<double>(total_size);
                auto           elapsed_seconds    = std::chrono::duration_cast<std::chrono::seconds>(
                                                        current_time - start_time)
                                                        .count();
                double         speed              = 0;
                int            eta_seconds        = 0;

                if (elapsed_seconds > 0 && current_bytes > 0)
                {
                    speed       = static_cast<double>(current_bytes) / static_cast<double>(elapsed_seconds);
                    eta_seconds = static_cast<int>(static_cast<double>(total_size - current_bytes) / speed);
                }

                size_t current_memory = zelph::platform::get_process_memory_usage();
                size_t memory_used    = (current_memory > baseline_memory) ? current_memory - baseline_memory : 0;

                size_t estimated_memory = 0;
                if (progress_fraction > 0.0)
                {
                    estimated_memory = static_cast<size_t>(
                        static_cast<double>(memory_used) / progress_fraction);
                }

                int eta_minutes = eta_seconds / 60;
                eta_seconds %= 60;
                int eta_hours = eta_minutes / 60;
                eta_minutes %= 60;
                const int decimal_places = 2;

                _pImpl->_n->diagnostic_stream() << "Progress: " << std::fixed << std::setprecision(decimal_places)
                                                << current_percentage << "% " << current_bytes << "/" << total_size << " bytes";

                if (!export_constraints)
                {
                    _pImpl->_n->diagnostic_stream() << " | Nodes: " << _pImpl->_n->count();
                }

                _pImpl->_n->diagnostic_stream() << " | ETA: ";
                if (eta_hours > 0) _pImpl->_n->diagnostic_stream() << eta_hours << "h ";
                if (eta_minutes > 0) _pImpl->_n->diagnostic_stream() << eta_minutes << "m ";
                _pImpl->_n->diagnostic_stream() << eta_seconds << "s";

                _pImpl->_n->diagnostic_stream() << " | Memory Used: " << std::fixed << std::setprecision(1) << (static_cast<double>(memory_used) / (1024 * 1024 * 1024)) << " GiB"
                                                << " | Estimated Total Memory: " << std::fixed << std::setprecision(1) << (static_cast<double>(estimated_memory) / (1024 * 1024 * 1024)) << " GiB"
                                                << std::endl;

                last_update_time = current_time;
            }

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
            const auto diag_seconds = std::chrono::duration_cast<std::chrono::seconds>(current_time - last_diag_time).count();
            if (diag_seconds >= ZELPH_WIKIDATA_IMPORT_DIAGNOSTIC_INTERVAL_SEC)
            {
                const auto diag_snapshot = import_diag.snapshot();
                const auto read_snapshot = read_async.get_stats_snapshot();
                const auto cpu_ns        = zelph::platform::get_process_cpu_time_ns();

                log_import_window(
                    _pImpl->_n,
                    diag_snapshot,
                    last_diag_snapshot,
                    read_snapshot,
                    last_read_snapshot,
                    current_time,
                    last_diag_time,
                    cpu_ns,
                    last_cpu_ns,
                    bytes_read.load(std::memory_order_relaxed),
                    total_size,
                    export_constraints,
                    export_constraints ? 0ull : static_cast<uint64_t>(_pImpl->_n->count()),
                    num_threads);

                last_diag_snapshot = diag_snapshot;
                last_read_snapshot = read_snapshot;
                last_cpu_ns        = cpu_ns;
                last_diag_time     = current_time;
            }
#endif
        }

        // Wait for all workers to complete
        for (auto& worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        if (read_async.error_text().empty())
        {
            if (export_constraints)
            {
                _pImpl->_n->diagnostic_stream() << "Constraints export completed." << std::endl;
            }
            else
            {
                _pImpl->_n->diagnostic_stream() << "Import completed successfully (" << _pImpl->_n->count() << " nodes)." << std::endl;

                try
                {
                    _pImpl->_n->diagnostic("Saving network to cache " + cache_file.string() + "...", true);
                    _pImpl->_n->save_to_file(_pImpl->_bin_path.string());
                    _pImpl->_n->diagnostic("Cache saved.", true);
                }
                catch (std::exception& ex)
                {
                    _pImpl->_n->error("Failed to save cache: "s + ex.what(), true);
                }
            }
        }
        else
        {
            throw std::runtime_error(read_async.error_text());
        }
    }
}

struct ConstraintInfo
{
    std::string                                                                            short_desc;
    std::string                                                                            long_desc;
    std::function<std::string(const std::string& /*json*/, const std::string& /*id_str*/)> generator;

    ConstraintInfo() = default; // Default constructor

    ConstraintInfo(std::string sd, std::string ld, std::function<std::string(const std::string& /*json*/, const std::string& /*id_str*/)> gen)
        : short_desc(sd), long_desc(ld), generator(gen) {}
};

// Helper to extract ids from qualifier arrays (searches for "id":"Pxx" or "id":"Qxx")
std::vector<std::string> extract_ids(const std::string& str, const std::string& qualifier_key)
{
    std::vector<std::string> ids;
    size_t                   pos    = 0;
    std::string              id_tag = "\"id\":\"";
    while ((pos = str.find(qualifier_key, pos)) != std::string::npos)
    {
        size_t start = str.find(id_tag, pos);
        if (start == std::string::npos) break;
        start += id_tag.size();
        size_t end = str.find('\"', start);
        if (end == std::string::npos) break;
        std::string id = str.substr(start, end - start);
        if (id.find('$') == std::string::npos)
        {
            ids.push_back(id);
        }
        pos = end;
    }
    return ids;
}

std::map<std::string, ConstraintInfo> get_supported_constraints()
{
    std::map<std::string, ConstraintInfo> constraints;

    // current constraints: https://query.wikidata.org/?#SELECT%20%3Fconstraint%20%3FconstraintLabel%20%0AWHERE%20%7B%0A%20%20%3Fconstraint%20wdt%3AP279%2a%20wd%3AQ21502402%20.%0A%20%20SERVICE%20wikibase%3Alabel%20%7B%20bd%3AserviceParam%20wikibase%3Alanguage%20%22%5BAUTO_LANGUAGE%5D%2Cen%22.%20%7D%0A%7D

    constraints["Q19474404"] = ConstraintInfo("single-value constraint (single value | single value constraint)", "type of constraint for Wikidata properties: used to specify that this property generally contains a single value per item", nullptr);
    constraints["Q21502404"] = ConstraintInfo("format constraint (regex constraint | format)", "type of constraint for Wikidata properties: used to specify that the value for this property has to correspond to a given pattern", nullptr);
    constraints["Q21502410"] = ConstraintInfo("distinct-values constraint (unique value | distinct values | distinct values constraint for Wikidata properties | unique value constraint | unique values constraint | unique-value constraint | unique-values constraint | distinct-value constraint | distinct value constraint | distinct values constraint)", "type of constraint for Wikidata properties: used to specify that the value for this property is likely to be different from all other items", nullptr);
    constraints["Q21502838"] = ConstraintInfo(
        "conflicts-with constraint (incompatible-with constraint | item requires none of this statement | item must not contain statement | inconsistent-with constraint)",
        "type of constraint for Wikidata properties: used to specify that an item must not have a given statement",
        [](const std::string& json, const std::string& id_str) -> std::string
        {
            std::stringstream result;
            result << "# Constraint: Q21502838" << std::endl;

            // Extract conflict property from P2306 (assume first/only one)
            auto conflict_props = extract_ids(json, "\"P2306\"");
            if (conflict_props.empty())
            {
                return "# No P2306 (conflict property) found";
            }
            const std::string& conflict_p = conflict_props[0]; // e.g. "P31"

            // Extract conflict values from P2305 (can be multiple or none)
            auto conflict_qs = extract_ids(json, "\"P2305\"");

            if (conflict_qs.empty())
            {
                // No specific value: conflict with presence of conflict_p
                result << "I " << id_str << " Y, I " << conflict_p << " Z => !" << std::endl;
            }
            else
            {
                // One rule per forbidden value
                for (const auto& q : conflict_qs)
                {
                    result << "I " << id_str << " Y, I " << conflict_p << " " << q << " => !" << std::endl;
                }
            }

            return result.str();
        });

    constraints["Q21503247"] = ConstraintInfo("item-requires-statement constraint (item constraint | requires claim constraint | item requires claim constraint | required statement constraint | statement required constraint | requires statement constraint | required claim constraint | subject requires statement constraint | item-has-statement constraint | item has statement constraint | item-has-claim constraint | item has claim constraint | item-requires-claim constraint | requires-claim constraint | has claim constraint | has-claim constraint | has statement constraint | has-statement constraint | claim required constraint | subject-requires-statement constraint | subject has statement constraint | subject requires claim constraint | subject has claim constraint | subject-has-statement constraint | subject-requires-claim constraint | subject-has-claim constraint | required-statement constraint | statement-required constraint)", "type of constraint for Wikidata properties: used to specify that an item with this property should also have another given property", nullptr);
    constraints["Q21503250"] = ConstraintInfo("subject type constraint (domain constraint | subject class constraint | type constraint | subject-type constraint | subject-class constraint)", "type of constraint for Wikidata properties: used to specify that the item described by such properties should be a subclass or instance of a given type", nullptr);
    constraints["Q21510851"] = ConstraintInfo("allowed qualifiers constraint (use qualifiers constraint | qualifiers constraint | optional qualifiers constraint)", "type of constraint for Wikidata properties: used to specify that only the listed qualifiers should be used. \" Novalue\" disallows any qualifier", nullptr);
    constraints["Q21510852"] = ConstraintInfo("Commons link constraint (Wikimedia Commons link constraint)", "type of constraint for Wikidata properties: used to specify that the value must link to an existing Wikimedia Commons page", nullptr);
    constraints["Q21510854"] = ConstraintInfo("difference-within-range constraint (difference within range constraint)", "type of constraint for Wikidata properties: used to specify that the value of a given statement should only differ in the given way. Use with qualifiers minimum quantity/maximum quantity", nullptr);
    constraints["Q21510856"] = ConstraintInfo("required qualifier constraint (mandatory qualifier)", "type of constraint for Wikidata properties: used to specify that the listed qualifier has to be used", nullptr);
    constraints["Q21510857"] = ConstraintInfo("multi-value constraint (multiple value constraint | multiple-value constraint | multi value constraint | multiple values constraint | multiple-values constraint)", "type of constraint for Wikidata properties: used to specify that a property generally contains more than one value per item", nullptr);
    constraints["Q21510859"] = ConstraintInfo("one-of constraint (one of constraint)", "type of constraint for Wikidata properties: used to specify that the value for this property has to be one of a given set of items", nullptr);
    constraints["Q21510860"] = ConstraintInfo("range constraint (value range constraint | value-within-range constraint | value-within-bounds constraint | value within range constraint | value within bounds constraint)", "type of constraint for Wikidata properties: used to specify that the value must be between two given values", nullptr);
    constraints["Q21510862"] = ConstraintInfo("symmetric constraint (Wikidata symmetric constraint | symmetry constraint)", "type of constraint for Wikidata properties: used to specify that the referenced entity should also link back to this entity", nullptr);
    constraints["Q21510863"] = ConstraintInfo("used as qualifier constraint (use as qualifier constraint | use as a qualifier)", "type of constraint for Wikidata properties: used to specify that a property must only be used as a qualifier", nullptr);
    constraints["Q21510864"] = ConstraintInfo("value-requires-statement constraint (value requires statement constraint | target required claim constraint)", "type of constraint for Wikidata properties: used to specify that the referenced item should have a statement with a given property", nullptr);
    constraints["Q21510865"] = ConstraintInfo("value-type constraint (allowed values | codomain constraint | value class constraint | value type constraint | value-class constraint | object type constraint | range constraint)", "type of constraint for Wikidata properties: used to specify that the value item should be a subclass or instance of a given type", nullptr);
    constraints["Q21514353"] = ConstraintInfo("allowed units constraint", "type of constraint for Wikidata properties: used to specify that only listed units may be used", nullptr);
    constraints["Q21528958"] = ConstraintInfo("used for values only constraint (value-only constraint | used as claims only | used as base properties in statement only)", "type of constraint for Wikidata properties: used to specify that a property can only be used as a property for values, not as a qualifier or reference", nullptr);
    constraints["Q21528959"] = ConstraintInfo("used as reference constraint (source-only constraint | reference-only constraint)", "type of constraint for Wikidata properties: used to specify that a property must only be used in references or instances of citation", nullptr);
    constraints["Q25796498"] = ConstraintInfo("contemporary constraint (coincide or coexist at some point of history)", "type of constraint for Wikidata properties: used to specify that the subject and the object have to coincide or coexist at some point of history", nullptr);
    constraints["Q42750658"] = ConstraintInfo("value constraint", "class of constraints on the value of a statement with a given property. For constraint: use specific items (e.g. \"value type constraint\", \"value requires statement constraint\", \"format constraint\", etc.)", nullptr);
    constraints["Q51723761"] = ConstraintInfo("no-bounds constraint (no bounds constraint)", "type of constraint for Wikidata properties: specifies that a property must only have values without validity bounds", nullptr);
    constraints["Q52004125"] = ConstraintInfo("allowed-entity-types constraint (entity types constraint | allowed entity types constraint)", "type of constraint for Wikidata properties: used to specify that a property may only be used on a certain listed entity type: Wikibase item, Wikibase property, lexeme, form, sense, Wikibase MediaInfo", nullptr);
    constraints["Q52060874"] = ConstraintInfo("single-best-value constraint (single best value | single best value constraint | single-preferred-value constraint | single preferred value | single preferred value constraint)", "type of constraint for Wikidata properties: used to specify that this property generally contains a single “best” value per item, though other values may be included as long as the “best” value is marked with preferred rank", nullptr);

    constraints["Q52558054"] = ConstraintInfo(
        "none-of constraint (none of constraint)",
        "constraint specifying values that should not be used for the given property",
        [](const std::string& json, const std::string& id_str) -> std::string
        {
            std::stringstream result;
            result << "# Constraint: Q52558054" << std::endl;

            // Extract forbidden values from P2305
            auto forbidden_qs = extract_ids(json, "\"P2305\"");
            if (forbidden_qs.empty())
            {
                return "# No forbidden values (P2305) found";
            }

            // One rule per forbidden value
            for (const auto& q : forbidden_qs)
            {
                result << "I " << id_str << " " << q << " => !" << std::endl;
            }

            return result.str();
        });
    constraints["Q52712340"] = ConstraintInfo("one-of qualifier value property constraint", "constraint that specifies which values can be used for a given qualifier when used on a specific property of an Item Declaration", nullptr);
    constraints["Q52848401"] = ConstraintInfo("integer constraint", "constraint type used when values have to be integer only", nullptr);
    constraints["Q53869507"] = ConstraintInfo(
        "property scope constraint (scope constraint | scope of property)",
        "constraint to define the scope of the property (as main property, as qualifier, as reference, or combination). Qualify with \"property scope\" (P5314)",
        [](const std::string& json, const std::string& id_str) -> std::string
        {
            // Find the second numeric-id (the one in P5314)
            static const std::string numeric_id_tag("\"numeric-id\":");
            size_t                    first_numeric = json.find(numeric_id_tag);
            if (first_numeric == std::string::npos) return "# No numeric-id found";

            size_t numeric_start = json.find(numeric_id_tag, first_numeric + 1);
            if (numeric_start == std::string::npos) return "# No second numeric-id found for qualifier";

            numeric_start += numeric_id_tag.size();
            size_t numeric_end = json.find(',', numeric_start);
            if (numeric_end == std::string::npos) numeric_end = json.find('}', numeric_start);

            if (numeric_end == std::string::npos) return "# Invalid numeric-id end";

            std::string numeric_id = json.substr(numeric_start, numeric_end - numeric_start);
            std::string scope_qid  = "Q" + numeric_id;

            // Generate symbolic rule based on scope
            // For Q54828448 ("as main value"), symbolic rule: disallow as qualifier (since zelph has no qualifiers, symbolic)
            if (scope_qid == "Q54828448")
            {
                return "# " + id_str + " as main value => !";
            }
            else if (scope_qid == "Q54828449")
            {
                return "# " + id_str + " as qualifier => !";
            }
            else if (scope_qid == "Q54828450")
            {
                return "# " + id_str + " as reference => !";
            }

            return ""; });
    constraints["Q54554025"]  = ConstraintInfo("citation-needed constraint (citation needed constraint | reference-needed constraint | reference needed constraint | source-needed constraint | source needed constraint | citation-required constraint | citation required constraint | reference-required constraint | reference required constraint | source-required constraint | source required constraint)", "type of constraint for Wikidata properties: specifies that a property must have at least one reference", nullptr);
    constraints["Q54718960"]  = ConstraintInfo("Wikidata constraint scope", "", nullptr);
    constraints["Q55819078"]  = ConstraintInfo("lexeme requires lexical category constraint (lexical category constraint)", "type of constraint for Wikidata properties: used to specify that the referenced lexeme should have a given lexical category", nullptr);
    constraints["Q55819106"]  = ConstraintInfo("lexeme requires language constraint (language required by this lexeme | language required constraint)", "property constraint for restricting the use of a property to lexemes in a particular language", nullptr);
    constraints["Q64006792"]  = ConstraintInfo("lexeme-value-requires-lexical-category constraint (target required lexical category)", "type of constraint for Wikidata properties: used to specify that the referenced lexeme should have a given lexical category", nullptr);
    constraints["Q102745616"] = ConstraintInfo("complex constraint", "constraint with two or more elements", nullptr);
    constraints["Q108139345"] = ConstraintInfo("label in language constraint (requires label constraint)", "constraint to ensure items using a property have label in the language (Use qualifier \"Wikimedia language code\" (P424) to define language)", nullptr);
    constraints["Q111204896"] = ConstraintInfo("description in language constraint", "constraint to ensure items using a property have description in the language. Use qualifier \" WMF language code \" (P424) to define language.", nullptr);
    constraints["Q21510855"]  = ConstraintInfo("inverse constraint", "type of constraint for Wikidata properties: used to specify that the referenced item has to refer back to this item with the given inverse property", nullptr);
    constraints["Q110262746"] = ConstraintInfo("string value length constraint", "the constraint on Wikidata String value length of 1,500 characters", nullptr);
    constraints["Q100883797"] = ConstraintInfo("complex constraint value label template", "qualify with regex to match by label of property label. $1 to be replaced by subject label", nullptr);
    constraints["Q100884525"] = ConstraintInfo("complex constraint value label (value label constraint)", "qualify with regex to match by label", nullptr);
    constraints["Q102173107"] = ConstraintInfo("complex constraint recency (recency)", "qualify with duration for maximum age", nullptr);
    constraints["Q102746314"] = ConstraintInfo("complex constraint label language", "qualify with language in which the entity would generally have a label. Requires {{subst:Define label language constraint}} on property talk pages to work", nullptr);

    return constraints;
}

void Wikidata::process_constraints(const std::string& line, std::string id_str, const std::string& dir)
{
    // Create directory if not exists
    std::filesystem::create_directory(dir);

    // Output file path
    std::string   filename = dir + "/" + id_str + ".zph";
    std::ofstream out(filename);

    if (out)
    {
        out << ".lang wikidata" << std::endl
            << std::endl;

        // Get supported constraints map
        auto constraints_map = get_supported_constraints();

        // Parse each constraint statement using the fixed string
        static const std::string constraint_start_tag("{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P2302\",\"datavalue\":");
        size_t                   pos = 0;
        while ((pos = line.find(constraint_start_tag, pos)) != std::string::npos)
        {
            // Find matching end of the constraint object using bracket counting
            size_t stmt_start    = pos;
            size_t stmt_end      = stmt_start + constraint_start_tag.size();
            int    bracket_count = 2; // We start inside the object
            bool   in_string     = false;
            while (stmt_end < line.size() && bracket_count > 0)
            {
                char c = line[stmt_end];
                if (c == '"')
                {
                    in_string = !in_string;
                }
                else if (!in_string)
                {
                    if (c == '{')
                        ++bracket_count;
                    else if (c == '}')
                        --bracket_count;
                }
                ++stmt_end;
            }
            if (bracket_count != 0) break; // Mismatched brackets

            --stmt_end; // Point to closing }

            // Extract the full statement JSON substring
            std::string stmt_json = line.substr(stmt_start, stmt_end - stmt_start + 1);

            // Extract the constraint type (Q-ID) from mainsnak
            static const std::string mainsnak_type_tag("\"datavalue\":{\"value\":{\"entity-type\":\"item\",\"numeric-id\":");
            size_t                   type_start = stmt_json.find(mainsnak_type_tag);
            if (type_start != std::string::npos)
            {
                type_start += mainsnak_type_tag.size();
                size_t type_end = stmt_json.find(',', type_start);
                if (type_end != std::string::npos)
                {
                    std::string numeric_id     = stmt_json.substr(type_start, type_end - type_start);
                    std::string constraint_qid = "Q" + numeric_id;

                    out << "# Constraint: " << constraint_qid << std::endl;

                    auto it = constraints_map.find(constraint_qid);
                    if (it != constraints_map.end())
                    {
                        out << "# Short description: " << it->second.short_desc << std::endl;
                        out << "# Long description: " << it->second.long_desc << std::endl;
                    }
                    else
                    {
                        out << "# Unsupported constraint: " << constraint_qid << std::endl;
                        out << "# This constraint is not in the supported list but is included as a comment block." << std::endl;
                    }

                    out << "# Raw JSON block:" << std::endl;
                    out << "# " << stmt_json << std::endl;

                    if (it != constraints_map.end() && it->second.generator)
                    {
                        std::string rules = it->second.generator(stmt_json, id_str);
                        if (rules.empty())
                        {
                            out << "# (Generator delivered empty rule set)" << std::endl;
                        }
                        else
                        {
                            out << rules << std::endl;
                        }
                    }
                    else
                    {
                        out << "# (no existing zelph rule generator for this constraint type)" << std::endl;
                    }

                    out << std::endl;
                }
            }

            pos = stmt_end + 1; // Move past this constraint
        }
    }
    else
    {
        // Error handling if file can't be opened
        _pImpl->_n->error("Failed to open file: " + filename, true);
    }
}

void Wikidata::process_import(const std::string& line,
                              const std::string& id_str,
                              const std::string& additional_language_to_import,
                              const bool         log,
                              size_t             id1,
                              ImportThreadStats* diag)
{
    thread_local std::unordered_map<std::string, network::Node> property_cache;
    thread_local std::unordered_set<network::Node>              typed_properties;
    network::Node                                               subject = 0;
    std::string                                                 name_in_additional_language;

    if (!additional_language_to_import.empty())
    {
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        const auto label_begin = SteadyClock::now();
#endif
        static const std::string language_tag("{\"language\":\"" + additional_language_to_import + "\",\"value\":\"");
        const size_t             language0 = line.find(language_tag, id1 + 7);
        if (language0 != std::string::npos)
        {
            if (language0 > line.find("\"labels\":{"))
            {
                const size_t aliases = line.find("\"aliases\":{", id1 + 7);

                if (aliases == std::string::npos || language0 < aliases)
                {
                    const size_t descriptions = line.find("\"descriptions\":{", id1 + 7);

                    if (descriptions == std::string::npos || language0 < descriptions)
                    {
                        id1                         = line.find('\"', language0 + language_tag.size() + 1);
                        name_in_additional_language = line.substr(language0 + language_tag.size(), id1 - language0 - language_tag.size());
                    }
                }
            }
        }
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        if (diag) diag->parse_label_ns += to_ns(SteadyClock::now() - label_begin);
#endif
    }

    size_t                   property0;
    static const std::string property_tag(R"(":[{"mainsnak":{"snaktype":"value","property":")");
    static const std::string numeric_id_tag(R"(","datavalue":{"value":{"entity-type":"item","numeric-id":)");
    static const std::string object_tag(R"("id":")");
    // Prefix shared by all claims in a property array
    static const std::string mainsnak_value_prefix(R"("mainsnak":{"snaktype":"value","property":")");

#ifdef SINGLE_THREADED_IMPORT
    std::unique_lock lock(_pImpl->_mtx_network, std::defer_lock);
#endif

    while ((property0 = line.find(property_tag, id1 + 1)) != std::string::npos)
    {
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        const auto scan_begin = SteadyClock::now();
#endif
        size_t      property1    = line.find('\"', property0 + property_tag.size());
        std::string property_str = line.substr(property0 + property_tag.size(), property1 - property0 - property_tag.size());

        if (property_str.empty() || property_str[0] != 'P')
        {
            throw std::runtime_error("Invalid property '" + property_str + "' in " + line);
        }

        // Pattern to match each claim's mainsnak item value for this property.
        // This matches: "mainsnak":{"snaktype":"value","property":"Pxxx","datavalue":{"value":{"entity-type":"item","numeric-id":
        // It does not match qualifier or reference snaks (they lack the "mainsnak":{} wrapper).
        std::string claim_value_tag = mainsnak_value_prefix + property_str + numeric_id_tag;

        // Boundary: start of the next property's array (next property_tag occurrence)
        size_t next_property0 = line.find(property_tag, property0 + property_tag.size());
        size_t boundary       = (next_property0 != std::string::npos) ? next_property0 : line.size();

        size_t search_pos = property0;

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        if (diag) diag->claim_scan_ns += to_ns(SteadyClock::now() - scan_begin);
#endif

        while ((search_pos = line.find(claim_value_tag, search_pos)) != std::string::npos && search_pos < boundary)
        {
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
            const auto inner_scan_begin = SteadyClock::now();
#endif
            size_t id0 = search_pos + claim_value_tag.size();

            bool success = true;
            while (++id0 < line.size() && line[id0] != ',')
            {
                if (line[id0] < '0' || line[id0] > '9')
                {
                    success = false;
                    break;
                }
            }

            if (success)
            {
                if (line.substr(id0 + 1, object_tag.size()) == object_tag)
                {
                    id0 += object_tag.size() + 1;
                    id1                    = line.find('\"', id0);
                    std::string object_str = line.substr(id0, id1 - id0);

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                    if (diag) diag->claim_scan_ns += to_ns(SteadyClock::now() - inner_scan_begin);
#endif

                    try
                    {
#ifdef SINGLE_THREADED_IMPORT
                        if (!lock.owns_lock()) lock.lock();
#endif

                        if (subject == 0)
                        {
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                            const auto t = SteadyClock::now();
#endif
                            subject = _pImpl->_n->node(id_str, "wikidata");
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                            if (diag) diag->subject_ns += to_ns(SteadyClock::now() - t);
#endif
                        }

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                        const auto obj_begin = SteadyClock::now();
#endif
                        network::Node object_node_handle = _pImpl->_n->node(object_str, "wikidata");
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                        if (diag) diag->object_node_ns += to_ns(SteadyClock::now() - obj_begin);
                        const auto prop_begin = SteadyClock::now();
#endif
                        network::Node property_node_handle;
                        auto          pit = property_cache.find(property_str);
                        if (pit != property_cache.end())
                        {
                            property_node_handle = pit->second;
                        }
                        else
                        {
                            property_node_handle = _pImpl->_n->node(property_str, "wikidata");
                            property_cache.emplace(property_str, property_node_handle);
                        }

                        // Typing only once per thread/property. Even if two threads want to type the same
                        // property twice,this is harmless in the trusted import path and very rare.
                        if (typed_properties.insert(property_node_handle).second)
                        {
                            _pImpl->_n->fact_import_trusted_single_object(
                                property_node_handle,
                                _pImpl->_n->core.IsA,
                                _pImpl->_n->core.RelationTypeCategory);
                        }
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                        if (diag) diag->property_node_ns += to_ns(SteadyClock::now() - prop_begin);
#endif

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                        const auto fact_begin = SteadyClock::now();
#endif
                        auto fact = _pImpl->_n->fact_import_trusted_single_object(
                            subject,
                            property_node_handle,
                            object_node_handle);
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
                        if (diag)
                        {
                            diag->fact_ns += to_ns(SteadyClock::now() - fact_begin);
                            diag->claims += 1;
                            diag->facts += 1;
                        }
#endif

                        if (log)
                        {
                            std::string output;
                            string::node_to_string(_pImpl->_n, output, "en", fact, 3);
                            _pImpl->_n->diagnostic(id_str + ":       en> " + output, true);
                            string::node_to_string(_pImpl->_n, output, "wikidata", fact, 3);
                            _pImpl->_n->diagnostic(id_str + ": wikidata> " + output, true);
                        }
                    }
                    catch (std::exception& ex)
                    {
                        _pImpl->_n->error(ex.what(), true);
                    }

                    search_pos = id1;
                }
                else
                {
                    search_pos = id0 + 1;
                }
            }
            else
            {
                search_pos = id0;
            }
        }

        id1 = (next_property0 != std::string::npos) ? next_property0 - 1 : line.size() - 1;
    }

    if (subject == 0)
    {
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        const auto t = SteadyClock::now();
#endif
#ifdef SINGLE_THREADED_IMPORT
        if (!lock.owns_lock()) lock.lock();
#endif
        subject = _pImpl->_n->node(id_str, "wikidata");
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        if (diag) diag->subject_ns += to_ns(SteadyClock::now() - t);
#endif
    }

    if (!name_in_additional_language.empty())
    {
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        const auto t = SteadyClock::now();
#endif
#ifdef SINGLE_THREADED_IMPORT
        assert(lock.owns_lock());
#endif
        _pImpl->_n->set_name(subject, name_in_additional_language, additional_language_to_import, false);
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        if (diag)
        {
            diag->set_name_ns += to_ns(SteadyClock::now() - t);
            diag->named_nodes += 1;
        }
#endif
    }
}

void Wikidata::process_entry(const std::string& line,
                             const std::string& additional_language_to_import,
                             const bool         log,
                             const std::string& constraints_dir,
                             ImportThreadStats* diag)
{
    const bool export_constraints = !constraints_dir.empty();

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
    const auto entry_begin = SteadyClock::now();
    const auto id_begin    = SteadyClock::now();
#endif

    static const std::string id_tag("\"id\":\"");
    size_t                   id0 = line.find(id_tag);

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
    if (diag) diag->parse_id_ns += to_ns(SteadyClock::now() - id_begin);
#endif

    if (id0 != std::string::npos)
    {
        size_t      id1 = line.find('\"', id0 + id_tag.size() + 1);
        std::string id_str(line.substr(id0 + id_tag.size(), id1 - id0 - id_tag.size()));

        if (export_constraints)
        {
            if (id_str.size() > 1 && id_str[0] == 'P')
            {
                process_constraints(line, id_str, constraints_dir);
            }
        }
        else
        {
            process_import(line, id_str, additional_language_to_import, log, id1, diag);
        }
    }

#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
    if (diag)
    {
        diag->lines += 1;
        diag->process_entry_ns += to_ns(SteadyClock::now() - entry_begin);
    }
#endif
}

void Wikidata::set_logging(bool do_log)
{
    _pImpl->_logging = do_log;
}

void Wikidata::export_entities(const std::vector<std::string>& entity_ids)
{
    if (entity_ids.empty()) return;

    std::unordered_set<std::string> remaining;
    for (const auto& wid : entity_ids)
        remaining.insert(wid);
    const size_t total_requested = remaining.size();

    const auto& source = _pImpl->_original_source_path;
    if (source.empty() || !std::filesystem::exists(source))
        throw std::runtime_error("No original Wikidata JSON found.");

    _pImpl->_n->diagnostic_stream() << "Exporting " << total_requested << " entities from " << source << " ..." << std::endl;

    // Buffer size doesn't matter much with batching, just needs to be enough to keep busy
    io::ReadAsync read_async(source, 100);

    if (!read_async.error_text().empty())
        throw std::runtime_error(read_async.error_text());

    // Total size of the compressed file
    const std::streamsize total_compressed_size = read_async.get_total_size();

    auto start_time  = std::chrono::steady_clock::now();
    auto last_update = start_time;

    size_t found = 0;

    // Track BOTH compressed pos (for %) and decompressed bytes (for Speed)
    std::streamoff compressed_pos     = 0;
    size_t         decompressed_bytes = 0;

    const std::string id_tag = R"("id":")";
    std::string       line;
    std::streamoff    streampos;

    while (read_async.get_line_utf8(line, streampos))
    {
        compressed_pos = streampos;
        decompressed_bytes += line.size() + 1; // +1 for newline

        // Fast string search
        size_t id_pos = line.find(id_tag);
        if (id_pos != std::string::npos)
        {
            size_t s = id_pos + id_tag.size();
            size_t e = line.find('\"', s);
            if (e != std::string::npos)
            {
                std::string id_str = line.substr(s, e - s);
                auto        it     = remaining.find(id_str);
                if (it != remaining.end())
                {
                    std::string   filename = id_str + ".json";
                    std::ofstream out(filename, std::ios::binary);
                    if (out)
                    {
                        out.write(line.data(), static_cast<std::streamsize>(line.size()));
                        out << '\n';
                        found++;
                        remaining.erase(it);
                        _pImpl->_n->out_stream() << "→ " << filename << std::endl;
                    }
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_update).count() >= 1.0)
        {
            // Percentage based on file position (Compressed)
            double percent = total_compressed_size ? 100.0 * static_cast<double>(compressed_pos) / static_cast<double>(total_compressed_size) : 0.0;

            // Speed based on throughput (Decompressed)
            auto   elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            double speed_mib   = elapsed_sec ? (static_cast<double>(decompressed_bytes) / (1024.0 * 1024.0 * static_cast<double>(elapsed_sec))) : 0.0;

            // ETA based on PERCENTAGE (reliable) vs Time
            // If we did X% in Y seconds, we need (100-X)% more time.
            int eta = 0;
            if (percent > 0 && elapsed_sec > 0)
            {
                double total_time_est = static_cast<double>(elapsed_sec) / (percent / 100.0);
                eta                   = static_cast<int>(total_time_est - static_cast<double>(elapsed_sec));
            }

            _pImpl->_n->diagnostic_stream() << "Progress: " << std::fixed << std::setprecision(2) << percent << "% "
                                            << (compressed_pos / (1024 * 1024)) << " MiB (cmp)"
                                            << " | Speed: " << std::fixed << std::setprecision(1) << speed_mib << " MiB/s (dec)"
                                            << " | ETA: " << (eta / 3600) << "h " << ((eta % 3600) / 60) << "m " << (eta % 60) << "s"
                                            << " | Found: " << found << "/" << total_requested << std::endl;

            last_update = now;
        }

        if (remaining.empty()) break;
    }

    _pImpl->_n->diagnostic_stream() << "Export completed." << std::endl;
}
