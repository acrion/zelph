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

#include "platform_utils.hpp"
#include "read_async.hpp"
#include "stopwatch.hpp"
#include "string_utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/map.hpp>
#include <boost/tokenizer.hpp>

#include <atomic>
#include <fstream>
#include <iomanip> // for std::setprecision
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace zelph::console;
using namespace zelph::network;
using boost::escaped_list_separator;
using boost::tokenizer;

class Wikidata::Impl
{
public:
    Impl(network::Zelph* n, const std::filesystem::path& file_name)
        : _n(n)
        , _file_name(file_name)
    {
    }

    bool                  read_index_file();
    void                  write_index_file() const;
    std::filesystem::path index_file_name() const;

    network::Zelph*                       _n{nullptr};
    std::filesystem::path                 _file_name;
    std::map<std::string, std::streamoff> _index;
    std::recursive_mutex                  _mtx;
    bool                                  _logging{true};
    std::string                           _last_entry;
    std::streamoff                        _last_index{0};
};

Wikidata::Wikidata(Zelph* n, const std::filesystem::path& file_name)
    : _pImpl(new Impl(n, file_name))
{
    n->set_process_node([this](const Node node, const std::string& lang)
                        { return this->process_node(node, lang); });
}

Wikidata::~Wikidata()
{
    delete _pImpl;
}

void Wikidata::import_all(bool filter_existing_only)
{
    std::clog << "Number of nodes prior import: " << _pImpl->_n->count() << std::endl;

    std::filesystem::path cache_file = _pImpl->_file_name;
    cache_file.replace_extension(".bin");

    bool cache_loaded = false;
    if (std::filesystem::exists(cache_file))
    {
        try
        {
            _pImpl->_n->print(L"Loading network from cache " + cache_file.wstring() + L"...", true);
            std::ifstream                   ifs(cache_file, std::ios::binary);
            boost::archive::binary_iarchive ia(ifs);

            _pImpl->_n->load_from_file(cache_file.string());

            _pImpl->_n->print(L"Cache loaded successfully (" + std::to_wstring(_pImpl->_n->count()) + L" nodes).", true);
            cache_loaded = true;
        }
        catch (std::exception& ex)
        {
            _pImpl->_n->print(L"Failed to load cache: " + utils::wstr(ex.what()), true);
        }
    }

    if (!cache_loaded)
    {
        _pImpl->_n->print(L"Importing file " + _pImpl->_file_name.wstring(), true);

        ReadAsync read_async(_pImpl->_file_name, 100000);
        if (!read_async.error_text().empty())
        {
            throw std::runtime_error(read_async.error_text());
        }

        const std::streamsize total_size = read_async.get_total_size();

        size_t baseline_memory = zelph::platform::get_process_memory_usage(); // Baseline before import starts

        // Atomic counters for thread coordination and progress tracking
        std::atomic<std::streamoff> bytes_read{0};
        const unsigned int          num_threads = std::thread::hardware_concurrency();
        std::atomic<unsigned int>   active_threads{num_threads};
        std::vector<std::thread>    workers;

        // Worker function: each thread processes lines from ReadAsync
        std::mutex read_mtx;

        auto worker_func = [&]()
        {
            for (;;)
            {
                std::wstring   line;
                std::streamoff streampos;

                {
                    std::lock_guard<std::mutex> lk(read_mtx);

                    if (!read_async.get_line(line, streampos))
                        break;
                }

                bytes_read.store(streampos, std::memory_order_relaxed);
                process_entry(line, true, false, filter_existing_only, filter_existing_only);
            }

            active_threads.fetch_sub(1, std::memory_order_relaxed);
        };

        // Start worker threads
        for (unsigned int i = 0; i < num_threads; ++i)
        {
            workers.emplace_back(worker_func);
        }

        // Progress reporting in main thread
        std::chrono::steady_clock::time_point start_time       = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_update_time = start_time;

        while (active_threads.load(std::memory_order_relaxed) > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            auto current_time           = std::chrono::steady_clock::now();
            auto time_since_last_update = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              current_time - last_update_time)
                                              .count()
                                        / 1000.0;

            if (time_since_last_update >= 1.0)
            {
                std::streamoff current_bytes      = bytes_read.load(std::memory_order_relaxed);
                double         current_percentage = (static_cast<double>(current_bytes) / total_size) * 100.0;
                double         progress_fraction  = static_cast<double>(current_bytes) / total_size;
                auto           elapsed_seconds    = std::chrono::duration_cast<std::chrono::seconds>(
                                           current_time - start_time)
                                           .count();
                double speed       = 0;
                int    eta_seconds = 0;

                if (elapsed_seconds > 0 && current_bytes > 0)
                {
                    speed       = static_cast<double>(current_bytes) / elapsed_seconds;
                    eta_seconds = static_cast<int>((total_size - current_bytes) / speed);
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

                std::clog << "Progress: " << std::fixed << std::setprecision(decimal_places)
                          << current_percentage << "% " << current_bytes << "/" << total_size << " bytes";

                std::clog << " | Nodes: " << _pImpl->_n->count();

                std::clog << " | ETA: ";
                if (eta_hours > 0) std::clog << eta_hours << "h ";
                if (eta_minutes > 0) std::clog << eta_minutes << "m ";
                std::clog << eta_seconds << "s";

                std::clog << " | Memory Used: " << std::fixed << std::setprecision(1) << (static_cast<double>(memory_used) / (1024 * 1024 * 1024)) << " GiB"
                          << " | Estimated Total Memory: " << std::fixed << std::setprecision(1) << (static_cast<double>(estimated_memory) / (1024 * 1024 * 1024)) << " GiB"
                          << std::endl;

                last_update_time = current_time;
            }
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
            std::clog << "Import completed successfully (" << _pImpl->_n->count() << " nodes)." << std::endl;

            try
            {
                _pImpl->_n->print(L"Saving network to cache " + cache_file.wstring() + L"...", true);
                _pImpl->_n->save_to_file(cache_file.string());
                _pImpl->_n->print(L"Cache saved.", true);
            }
            catch (std::exception& ex)
            {
                _pImpl->_n->print(L"Failed to save cache: " + utils::wstr(ex.what()), true);
            }
        }
        else
        {
            throw std::runtime_error(read_async.error_text());
        }
    }
}

void Wikidata::process_entry(const std::wstring& line, const bool import_english, const bool log, const bool filter_existing_nodes, const bool restrictive_property_filter)
{
    static const std::wstring id_tag(L"\"id\":\"");
    size_t                    id0 = line.find(id_tag);

    if (id0 != std::wstring::npos)
    {
        size_t       id1 = line.find(L"\"", id0 + id_tag.size() + 1);
        std::wstring id_str(line.substr(id0 + id_tag.size(), id1 - id0 - id_tag.size()));

        Node subject        = _pImpl->_n->get_node(id_str, "wikidata");
        bool subject_exists = (subject != 0);

        if (!filter_existing_nodes && subject == 0)
        {
            subject        = _pImpl->_n->node(id_str, "wikidata");
            subject_exists = true;
        }

        if (subject_exists)
        {
            size_t                    language0;
            static const std::wstring language_tag(L"{\"language\":\"");
            size_t                    labels       = line.find(L"\"labels\":{");
            size_t                    aliases      = line.find(L"\"aliases\":{", id1 + 7);
            size_t                    descriptions = line.find(L"\"descriptions\":{", id1 + 7);

            while ((language0 = line.find(language_tag, id1 + 7)) != std::wstring::npos
                   && language0 > labels
                   && (aliases == std::wstring::npos || language0 < aliases)
                   && (descriptions == std::wstring::npos || language0 < descriptions))
            {
                size_t      language1 = line.find(L"\"", language0 + language_tag.size());
                std::string language  = utils::str(line.substr(language0 + language_tag.size(), language1 - language0 - language_tag.size()));

                static const std::wstring value_tag(L"\"value\":\"");
                id0 = line.find(value_tag, language1 + 1);
                id1 = line.find(L"\"", id0 + value_tag.size() + 1);
                std::wstring value(line.substr(id0 + value_tag.size(), id1 - id0 - value_tag.size()));

                if (import_english && language == "en")
                {
                    _pImpl->_n->set_name(subject, value, language);
                }
            }
        }

        size_t                    property0;
        static const std::wstring property_tag(LR"(":[{"mainsnak":{"snaktype":"value","property":")");

        while ((property0 = line.find(property_tag, id1 + 1)) != std::wstring::npos)
        {
            size_t       property1    = line.find(L"\"", property0 + property_tag.size());
            std::wstring property_str = line.substr(property0 + property_tag.size(), property1 - property0 - property_tag.size());

            if (property_str.empty() || property_str[0] != L'P')
            {
                throw std::runtime_error("Invalid property '" + utils::str(property_str) + "' in " + utils::str(line));
            }

            bool process_this_property = true;
            if (restrictive_property_filter)
            {
                if (_pImpl->_n->get_node(property_str, "wikidata") == 0)
                {
                    process_this_property = false;
                }
            }

            static const std::wstring numeric_id_tag(LR"(","datavalue":{"value":{"entity-type":"item","numeric-id":)");

            if (process_this_property && line.substr(property1, numeric_id_tag.size()) == numeric_id_tag)
            {
                id0 = property1 + numeric_id_tag.size();

                bool success = true;
                while (++id0 < line.size() && line[id0] != L',')
                {
                    if (line[id0] < L'0' || line[id0] > L'9')
                    {
                        success = false;
                        break;
                    }
                }

                if (success)
                {
                    static const std::wstring object_tag(LR"("id":")");
                    if (line.substr(id0 + 1, object_tag.size()) == object_tag)
                    {
                        id0 += object_tag.size() + 1;
                        id1                     = line.find(L"\"", id0);
                        std::wstring object_str = line.substr(id0, id1 - id0);

                        bool should_import      = !filter_existing_nodes;
                        Node object_node_handle = 0;

                        if (filter_existing_nodes)
                        {
                            if (restrictive_property_filter)
                            {
                                should_import = true;
                            }
                            else
                            {
                                object_node_handle = _pImpl->_n->get_node(object_str, "wikidata");
                                if (subject_exists || object_node_handle != 0)
                                {
                                    should_import = true;
                                }
                            }
                        }

                        if (should_import)
                        {
                            try
                            {
                                if (subject == 0)
                                {
                                    subject        = _pImpl->_n->node(id_str, "wikidata");
                                    subject_exists = true;
                                }

                                if (object_node_handle == 0)
                                {
                                    object_node_handle = _pImpl->_n->node(object_str, "wikidata");
                                }

                                auto fact = _pImpl->_n->fact(
                                    subject,
                                    _pImpl->_n->node(property_str, "wikidata"),
                                    {object_node_handle});

                                if (log)
                                {
                                    std::wstring output;
                                    _pImpl->_n->format_fact(output, "en", fact);
                                    _pImpl->_n->print(id_str + L":       en> " + output, false);
                                    _pImpl->_n->format_fact(output, "wikidata", fact);
                                    _pImpl->_n->print(id_str + L": wikidata> " + output, false);
                                }
                            }
                            catch (std::exception& ex)
                            {
                                _pImpl->_n->print(utils::wstr(ex.what()), true);
                            }
                        }
                    }
                    else
                    {
                        id1 = id0 + object_tag.size() + 1;
                    }
                }
                else
                {
                    id1 = property1 + numeric_id_tag.size();
                }
            }
            else
            {
                id1 += property_tag.size();
            }
        }
    }
}

void Wikidata::generate_index() const
{
    if (!_pImpl->read_index_file())
    {
        _pImpl->_n->print(L"Indexing file " + _pImpl->_file_name.wstring(), true);

        ReadAsync read_async(_pImpl->_file_name);
        // std::wifstream stream(_pImpl->_file_name.string());

        std::vector<std::thread> threads;

        StopWatch watch;
        watch.start();

        // for (std::wstring line; std::getline(stream, line); )
        std::wstring   line;
        std::streamoff streampos;
        while (read_async.get_line(line, streampos))
        {
            index_entry(line, streampos);

            if (watch.duration() >= 1000)
            {
                {
                    std::lock_guard lock(_pImpl->_mtx);
                    _pImpl->_n->print(L"Indexed " + std::to_wstring(_pImpl->_index.size()) + L" wikidata entries, latest is '" + utils::wstr(_pImpl->_last_entry) + L"' at position " + std::to_wstring(_pImpl->_last_index) + L" (" + std::to_wstring(_pImpl->_last_index / 1024.0 / 1024 / 1024) + L" GB)", true);
                }
                watch.start();
            }
        }

        if (read_async.error_text().empty())
        {
            _pImpl->write_index_file();
        }
        else
        {
            throw std::runtime_error(read_async.error_text());
        }
    }

    _pImpl->_n->print(L"Total number of wikidata items in " + _pImpl->_file_name.wstring() + L": " + std::to_wstring(_pImpl->_index.size()), true);
}

void Wikidata::index_entry(const std::wstring& line, const std::streamoff streampos) const
{
    static const std::wstring id_tag(L"\"id\":\"");
    size_t                    id0 = line.find(id_tag);

    if (id0 != std::wstring::npos)
    {
        size_t       id1 = line.find(L"\"", id0 + id_tag.size() + 1);
        std::wstring idw(line.substr(id0 + id_tag.size(), id1 - id0 - id_tag.size()));
        std::string  id = utils::str(idw);

        std::lock_guard lock(_pImpl->_mtx);
        _pImpl->_index[id]  = streampos;
        _pImpl->_last_entry = id;
        _pImpl->_last_index = streampos;
    }
}

void Wikidata::process_name(const std::wstring& wikidata_name)
{
    if (_pImpl->_index.count(utils::str(wikidata_name)) != 0)
    {
        std::wifstream stream(_pImpl->_file_name);
        stream.seekg(_pImpl->_index[utils::str(wikidata_name)]);

        std::wstring line;
        std::getline(stream, line);
        process_entry(line, true, false, false, false);
    }
}

void Wikidata::process_node(const Node node, const std::string& lang)
{
    std::lock_guard lock(_pImpl->_mtx);

    if (!_pImpl->_n->has_name(node, "en"))
    {
        const std::wstring name = _pImpl->_n->get_name(node, "wikidata", false, false);
        if (!name.empty())
        {
            process_name(name);
            const std::wstring english_name = _pImpl->_n->get_name(node, "en", false, false);
            if (english_name.empty())
            {
                if (_pImpl->_logging)
                {
                    _pImpl->_n->print(L"Fetched node '" + name + L"' (" + std::to_wstring(node) + L")", true);
                }
                _pImpl->_n->set_name(node, name, "en"); // In case a node has no name in Wikidata (like e.g. Q3071250) we want to avoid trying multiple times to find one.
            }
            else if (_pImpl->_logging)
            {
                _pImpl->_n->print(L"Fetched node '" + name + L"' (" + std::to_wstring(node) + L") --> '" + english_name + L"'", true);
            }
        }
    }
}
void Wikidata::set_logging(bool do_log)
{
    _pImpl->_logging = do_log;
}

// ------------------------------------------------- Wikidata::Impl

bool Wikidata::Impl::read_index_file()
{
    if (!std::filesystem::exists(index_file_name()))
    {
        return false;
    }

    _n->print(L"Reading index file " + index_file_name().wstring() + L"...", true);
    std::ifstream                 stream(index_file_name());
    boost::archive::text_iarchive iarch(stream);
    iarch >> _index;
    _n->print(L"Finished reading", true);

    return true;
}

void Wikidata::Impl::write_index_file() const
{
    _n->print(L"Writing index file " + index_file_name().wstring() + L"...", true);
    std::ofstream                 stream(index_file_name());
    boost::archive::text_oarchive oarch(stream);
    oarch << _index;
    _n->print(L"Finished writing", true);
}

std::filesystem::path Wikidata::Impl::index_file_name() const
{
    return _file_name.parent_path() / (_file_name.stem().string() + ".index");
}
