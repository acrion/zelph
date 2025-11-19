/*
Copyright (c) 2025 acrion innovations GmbH
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
#include "read_async.hpp"
#include "stopwatch.hpp"
#include "utils.hpp"

#include <boost/algorithm/string.hpp>
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
    std::mutex                            _mtx;
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

void Wikidata::import_all()
{
    _pImpl->_n->print(L"Importing file " + _pImpl->_file_name.wstring(), true);

    ReadAsync read_async(_pImpl->_file_name, 100000);
    if (!read_async.error_text().empty())
    {
        throw std::runtime_error(read_async.error_text());
    }

    const std::streamsize total_size = read_async.get_total_size();

    // Atomic counters for thread coordination and progress tracking
    std::atomic<std::streamoff> bytes_read{0};
    const unsigned int          num_threads = std::thread::hardware_concurrency() * 500;
    std::atomic<unsigned int>   active_threads{num_threads};
    std::vector<std::thread>    workers;

    // Worker function: each thread processes lines from ReadAsync
    auto worker_func = [&](size_t thread_idx)
    {
        std::wstring   line;
        std::streamoff streampos;
        while (read_async.get_line(line, streampos))
        {
            bytes_read.store(streampos, std::memory_order_relaxed);
            process_entry(line, false, thread_idx);
        }
        active_threads.fetch_sub(1, std::memory_order_relaxed);
    };

    // Start worker threads
    for (unsigned int i = 0; i < num_threads; ++i)
    {
        workers.emplace_back(worker_func, i);
    }

    // Progress reporting in main thread
    std::chrono::steady_clock::time_point start_time       = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_update_time = start_time;
    const int                             decimal_places   = 2;

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

            int eta_minutes = eta_seconds / 60;
            eta_seconds %= 60;
            int eta_hours = eta_minutes / 60;
            eta_minutes %= 60;

            std::clog << "Progress: " << std::fixed << std::setprecision(decimal_places)
                      << current_percentage << "% " << current_bytes << "/" << total_size << " bytes";

            if (eta_seconds > 0 || eta_minutes > 0 || eta_hours > 0)
            {
                std::clog << " | ETA: ";
                if (eta_hours > 0) std::clog << eta_hours << "h ";
                if (eta_minutes > 0) std::clog << eta_minutes << "m ";
                std::clog << eta_seconds << "s";
            }
            std::clog << " | Threads: " << num_threads << std::endl;

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
        std::clog << "Import completed successfully!" << std::endl;
    }
    else
    {
        throw std::runtime_error(read_async.error_text());
    }
}

void Wikidata::process_entry(const std::wstring& line, const bool log, const size_t thread_index)
{
    static const std::wstring id_tag(L"\"id\":\"");
    size_t                    id0 = line.find(id_tag);

    if (id0 != std::wstring::npos)
    {
        size_t       id1 = line.find(L"\"", id0 + id_tag.size() + 1);
        std::wstring id(line.substr(id0 + id_tag.size(), id1 - id0 - id_tag.size()));

        Node current = _pImpl->_n->node(id, "wikidata"); // we treat the id as an additional language named "wikidata"

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

            if (language == "en") // currently we only include the languages "en" and "wikidata", which is the id (see above comment).
            {
                _pImpl->_n->set_name(current, value, language);
                //_pImpl->_n->print(id + L": " + utils::wstr(language) + L": " + value, false);
            }
        }

        size_t                    property0;
        static const std::wstring property_tag(LR"(":[{"mainsnak":{"snaktype":"value","property":")");
        while ((property0 = line.find(property_tag, id1 + 1)) != std::wstring::npos)
        {
            size_t       property1 = line.find(L"\"", property0 + property_tag.size());
            std::wstring property  = line.substr(property0 + property_tag.size(), property1 - property0 - property_tag.size());

            static const std::wstring numeric_id_tag(LR"(","datavalue":{"value":{"entity-type":"item","numeric-id":)");

            if (line.substr(property1, numeric_id_tag.size()) == numeric_id_tag)
            {
                id0 = property1 + numeric_id_tag.size();

                bool success = true;
                while (line[++id0] != L',')
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
                        id1                 = line.find(L"\"", id0);
                        std::wstring object = line.substr(id0, id1 - id0);
                        // std::wcout << L" ---> " << property << L" " << object << std::endl;

                        try
                        {
                            auto fact = _pImpl->_n->fact(current, _pImpl->_n->node(property, "wikidata"), {_pImpl->_n->node(object, "wikidata")});
                            if (log)
                            {
                                std::wstring output;
                                _pImpl->_n->format_fact(output, "en", fact);
                                _pImpl->_n->print(id + L":       en> " + output, false);
                                _pImpl->_n->format_fact(output, "wikidata", fact);
                                _pImpl->_n->print(id + L": wikidata> " + output, false);
                            }
                        }
                        catch (std::exception& ex)
                        {
                            _pImpl->_n->print(utils::wstr(ex.what()), true);
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

    _running = _running & ~(1ull << thread_index);
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
            //    size_t i = 0;
            //    for (i = 0; i < threads.size(); ++i)
            //      if (!(_running & 1ull<<i))
            //        break;

            //    _running = _running | 1ull<<i;
            //    std::thread t(&Wikidata::process_command, this, line, i);
            //    if (i < threads.size())
            //    {
            //      if (threads[i].joinable()) threads[i].join();
            //      threads[i] = std::move(t);
            //    }
            //    else
            //    {
            //      threads.emplace_back(std::move(t));
            //      _pImpl->_n->print(L"Running " + std::to_wstring(threads.size()) + L" threads for reading wikidata file...", true);
            //    }

            if (watch.duration() >= 1000)
            {
                {
                    std::lock_guard<std::mutex> lock(_pImpl->_mtx);
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

        std::lock_guard<std::mutex> lock(_pImpl->_mtx);
        _pImpl->_index[id]  = streampos;
        _pImpl->_last_entry = id;
        _pImpl->_last_index = streampos;
    }
}

void Wikidata::traverse(std::wstring start_entry)
{
    if (start_entry.empty())
    {
        std::wifstream stream(_pImpl->_file_name);
        for (int i = 0; i < 4; ++i) // TODO: workaround!
        {
            for (const auto& it : _pImpl->_n->get_nodes_in_language("wikidata"))
            {
                const Node         node  = it.first;
                const std::wstring entry = it.second;
                std::wstring       name  = _pImpl->_n->get_name(node, "en");
                if (name.empty())
                {
                    if (_pImpl->_index.count(utils::str(entry)) == 0)
                    {
                        _pImpl->_n->print(L"Cannot find wikidata entry '" + entry + L"' in " + _pImpl->_file_name.wstring(), true);
                    }
                    else
                    {
                        stream.seekg(_pImpl->_index[utils::str(entry)]);

                        std::wstring line;
                        std::getline(stream, line);
                        process_entry(line);
                    }
                }
                else
                {
                    //_pImpl->_n->print(name + L" (" + entry + L") is already known.", true);
                }
            }
        }
    }
    else
    {
        process_name(start_entry); // TODO this causes a segmentation fault!
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
        process_entry(line, false);
    }
}

void Wikidata::process_node(const Node node, const std::string& lang)
{
    std::lock_guard<std::mutex> lock(_pImpl->_mtx);

    if (!_pImpl->_n->has_name(node, "en"))
    {
        const std::wstring name = _pImpl->_n->get_name(node, "wikidata", false, false);
        if (!name.empty())
        {
            process_name(name);
            const std::wstring english_name = _pImpl->_n->get_name(node, "en", false, false);
            if (english_name.empty())
            {
                _pImpl->_n->print(L"Fetched node '" + name + L"' (" + std::to_wstring(node) + L")", true);
                _pImpl->_n->set_name(node, name, "en"); // In case a node has no name in Wikidata (like e.g. Q3071250) we want to avoid trying multiple times to find one.
            }
            else
            {
                _pImpl->_n->print(L"Fetched node '" + name + L"' (" + std::to_wstring(node) + L") --> '" + english_name + L"'", true);
            }
        }
    }
}

// ------------------------------------------------- Wikidata::Impl

bool Wikidata::Impl::read_index_file()
{
    if (!std::filesystem::exists(index_file_name()) || std::filesystem::last_write_time(_file_name) > std::filesystem::last_write_time(index_file_name()))
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
