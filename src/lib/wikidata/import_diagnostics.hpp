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

#pragma once

#include "io/read_async.hpp"
#include "network/zelph.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

#define ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS 1

#ifndef ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
    #ifdef NDEBUG
        #define ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS 0
    #else
        #define ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS 1
    #endif
#endif

#ifndef ZELPH_WIKIDATA_IMPORT_DIAGNOSTIC_INTERVAL_SEC
    #define ZELPH_WIKIDATA_IMPORT_DIAGNOSTIC_INTERVAL_SEC 60
#endif

namespace zelph::wikidata
{
    using SteadyClock = std::chrono::steady_clock;

    inline uint64_t to_ns(const SteadyClock::duration d)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    }

    inline double ns_to_s(uint64_t ns)
    {
        return static_cast<double>(ns) / 1e9;
    }

    inline double bytes_to_mib(double bytes)
    {
        return bytes / (1024.0 * 1024.0);
    }

    struct ImportThreadStats
    {
        uint64_t batches     = 0;
        uint64_t lines       = 0;
        uint64_t claims      = 0;
        uint64_t facts       = 0;
        uint64_t named_nodes = 0;

        uint64_t wait_batch_ns    = 0;
        uint64_t process_entry_ns = 0;
        uint64_t parse_id_ns      = 0;
        uint64_t parse_label_ns   = 0;
        uint64_t claim_scan_ns    = 0;
        uint64_t subject_ns       = 0;
        uint64_t property_node_ns = 0;
        uint64_t object_node_ns   = 0;
        uint64_t fact_ns          = 0;
        uint64_t set_name_ns      = 0;

        void reset()
        {
            *this = {};
        }
    };

    struct ImportDiagSnapshot
    {
        uint64_t batches        = 0;
        uint64_t lines          = 0;
        uint64_t claims         = 0;
        uint64_t facts          = 0;
        uint64_t named_nodes    = 0;
        uint64_t max_batch_size = 0;

        uint64_t wait_batch_ns    = 0;
        uint64_t process_entry_ns = 0;
        uint64_t parse_id_ns      = 0;
        uint64_t parse_label_ns   = 0;
        uint64_t claim_scan_ns    = 0;
        uint64_t subject_ns       = 0;
        uint64_t property_node_ns = 0;
        uint64_t object_node_ns   = 0;
        uint64_t fact_ns          = 0;
        uint64_t set_name_ns      = 0;
    };

    class ImportDiagnostics
    {
    public:
        void merge(const ImportThreadStats& s, size_t batch_size)
        {
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
            _batches.fetch_add(s.batches, std::memory_order_relaxed);
            _lines.fetch_add(s.lines, std::memory_order_relaxed);
            _claims.fetch_add(s.claims, std::memory_order_relaxed);
            _facts.fetch_add(s.facts, std::memory_order_relaxed);
            _named_nodes.fetch_add(s.named_nodes, std::memory_order_relaxed);

            _wait_batch_ns.fetch_add(s.wait_batch_ns, std::memory_order_relaxed);
            _process_entry_ns.fetch_add(s.process_entry_ns, std::memory_order_relaxed);
            _parse_id_ns.fetch_add(s.parse_id_ns, std::memory_order_relaxed);
            _parse_label_ns.fetch_add(s.parse_label_ns, std::memory_order_relaxed);
            _claim_scan_ns.fetch_add(s.claim_scan_ns, std::memory_order_relaxed);
            _subject_ns.fetch_add(s.subject_ns, std::memory_order_relaxed);
            _property_node_ns.fetch_add(s.property_node_ns, std::memory_order_relaxed);
            _object_node_ns.fetch_add(s.object_node_ns, std::memory_order_relaxed);
            _fact_ns.fetch_add(s.fact_ns, std::memory_order_relaxed);
            _set_name_ns.fetch_add(s.set_name_ns, std::memory_order_relaxed);

            uint64_t current = _max_batch_size.load(std::memory_order_relaxed);
            while (batch_size > current
                   && !_max_batch_size.compare_exchange_weak(current, static_cast<uint64_t>(batch_size), std::memory_order_relaxed))
            {
            }
#else
            (void)s;
            (void)batch_size;
#endif
        }

        ImportDiagSnapshot snapshot() const
        {
            ImportDiagSnapshot s;
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
            s.batches        = _batches.load(std::memory_order_relaxed);
            s.lines          = _lines.load(std::memory_order_relaxed);
            s.claims         = _claims.load(std::memory_order_relaxed);
            s.facts          = _facts.load(std::memory_order_relaxed);
            s.named_nodes    = _named_nodes.load(std::memory_order_relaxed);
            s.max_batch_size = _max_batch_size.load(std::memory_order_relaxed);

            s.wait_batch_ns    = _wait_batch_ns.load(std::memory_order_relaxed);
            s.process_entry_ns = _process_entry_ns.load(std::memory_order_relaxed);
            s.parse_id_ns      = _parse_id_ns.load(std::memory_order_relaxed);
            s.parse_label_ns   = _parse_label_ns.load(std::memory_order_relaxed);
            s.claim_scan_ns    = _claim_scan_ns.load(std::memory_order_relaxed);
            s.subject_ns       = _subject_ns.load(std::memory_order_relaxed);
            s.property_node_ns = _property_node_ns.load(std::memory_order_relaxed);
            s.object_node_ns   = _object_node_ns.load(std::memory_order_relaxed);
            s.fact_ns          = _fact_ns.load(std::memory_order_relaxed);
            s.set_name_ns      = _set_name_ns.load(std::memory_order_relaxed);
#endif
            return s;
        }

    private:
        std::atomic<uint64_t> _batches{0};
        std::atomic<uint64_t> _lines{0};
        std::atomic<uint64_t> _claims{0};
        std::atomic<uint64_t> _facts{0};
        std::atomic<uint64_t> _named_nodes{0};
        std::atomic<uint64_t> _max_batch_size{0};

        std::atomic<uint64_t> _wait_batch_ns{0};
        std::atomic<uint64_t> _process_entry_ns{0};
        std::atomic<uint64_t> _parse_id_ns{0};
        std::atomic<uint64_t> _parse_label_ns{0};
        std::atomic<uint64_t> _claim_scan_ns{0};
        std::atomic<uint64_t> _subject_ns{0};
        std::atomic<uint64_t> _property_node_ns{0};
        std::atomic<uint64_t> _object_node_ns{0};
        std::atomic<uint64_t> _fact_ns{0};
        std::atomic<uint64_t> _set_name_ns{0};
    };

    static void log_import_window(
        zelph::network::Zelph*                      n,
        const ImportDiagSnapshot&                   cur,
        const ImportDiagSnapshot&                   prev,
        const zelph::io::ReadAsync::StatsSnapshot&  read_cur,
        const zelph::io::ReadAsync::StatsSnapshot&  read_prev,
        const std::chrono::steady_clock::time_point now,
        const std::chrono::steady_clock::time_point last,
        uint64_t                                    cpu_cur_ns,
        uint64_t                                    cpu_prev_ns,
        std::streamoff                              current_bytes,
        std::streamsize                             total_size,
        bool                                        export_constraints,
        uint64_t                                    node_count,
        unsigned int                                num_threads)
    {
#if ZELPH_WIKIDATA_IMPORT_DIAGNOSTICS
        const uint64_t wall_ns = to_ns(now - last);
        if (wall_ns == 0) return;

        const uint64_t lines_delta   = cur.lines - prev.lines;
        const uint64_t claims_delta  = cur.claims - prev.claims;
        const uint64_t facts_delta   = cur.facts - prev.facts;
        const uint64_t named_delta   = cur.named_nodes - prev.named_nodes;
        const uint64_t batches_delta = cur.batches - prev.batches;

        const uint64_t wait_delta  = cur.wait_batch_ns - prev.wait_batch_ns;
        const uint64_t entry_delta = cur.process_entry_ns - prev.process_entry_ns;
        const uint64_t id_delta    = cur.parse_id_ns - prev.parse_id_ns;
        const uint64_t label_delta = cur.parse_label_ns - prev.parse_label_ns;
        const uint64_t scan_delta  = cur.claim_scan_ns - prev.claim_scan_ns;
        const uint64_t subj_delta  = cur.subject_ns - prev.subject_ns;
        const uint64_t prop_delta  = cur.property_node_ns - prev.property_node_ns;
        const uint64_t obj_delta   = cur.object_node_ns - prev.object_node_ns;
        const uint64_t fact_delta  = cur.fact_ns - prev.fact_ns;
        const uint64_t name_delta  = cur.set_name_ns - prev.set_name_ns;

        const uint64_t read_batches_delta   = read_cur.batches_enqueued - read_prev.batches_enqueued;
        const uint64_t read_entries_delta   = read_cur.entries_enqueued - read_prev.entries_enqueued;
        const uint64_t read_source_delta    = read_cur.source_bytes_read - read_prev.source_bytes_read;
        const uint64_t read_output_delta    = read_cur.output_bytes_emitted - read_prev.output_bytes_emitted;
        const uint64_t read_wait_full_delta = read_cur.queue_wait_not_full_ns - read_prev.queue_wait_not_full_ns;

        const double seconds      = ns_to_s(wall_ns);
        const double progress_pct = total_size > 0
                                      ? 100.0 * static_cast<double>(current_bytes) / static_cast<double>(total_size)
                                      : 0.0;

        const double cpu_cores_avg           = wall_ns > 0 ? static_cast<double>(cpu_cur_ns - cpu_prev_ns) / static_cast<double>(wall_ns) : 0.0;
        const double worker_busy_cores       = wall_ns > 0 ? static_cast<double>(entry_delta) / static_cast<double>(wall_ns) : 0.0;
        const double worker_wait_cores       = wall_ns > 0 ? static_cast<double>(wait_delta) / static_cast<double>(wall_ns) : 0.0;
        const double reader_blocked_fraction = wall_ns > 0 ? static_cast<double>(read_wait_full_delta) / static_cast<double>(wall_ns) : 0.0;

        const uint64_t accounted   = id_delta + label_delta + scan_delta + subj_delta + prop_delta + obj_delta + fact_delta + name_delta;
        const uint64_t other_delta = entry_delta > accounted ? entry_delta - accounted : 0;

        std::vector<std::pair<std::string, uint64_t>> top{
            {"fact()", fact_delta},
            {"node(object)", obj_delta},
            {"node(property)", prop_delta},
            {"get/create subject", subj_delta},
            {"claim scan", scan_delta},
            {"set_name()", name_delta},
            {"parse label", label_delta},
            {"parse id", id_delta},
            {"other", other_delta},
        };

        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b)
                  { return a.second > b.second; });

        {
            auto ds = n->diagnostic_stream();
            ds << "[ImportDiag] last " << std::fixed << std::setprecision(1) << seconds << "s"
               << " | progress=" << std::setprecision(2) << progress_pct << "% "
               << current_bytes << "/" << total_size << " bytes";
            if (!export_constraints)
            {
                ds << " | nodes=" << node_count;
            }
            ds << " | workers=" << num_threads
               << " | cpu(main)=" << std::setprecision(2) << cpu_cores_avg << " cores"
               << " | worker-busy=" << worker_busy_cores << " cores"
               << " | worker-input-wait=" << worker_wait_cores << " cores"
               << std::endl;
        }

        {
            auto ds = n->diagnostic_stream();
            ds << "[ImportDiag] throughput"
               << " | lines=" << lines_delta << " (" << std::fixed << std::setprecision(0) << (lines_delta / seconds) << "/s)"
               << " | claims=" << claims_delta << " (" << (claims_delta / seconds) << "/s)"
               << " | facts=" << facts_delta << " (" << (facts_delta / seconds) << "/s)"
               << " | names=" << named_delta << " (" << (named_delta / seconds) << "/s)"
               << " | worker-batches=" << batches_delta
               << " | max-batch=" << cur.max_batch_size
               << std::endl;
        }

        {
            auto ds = n->diagnostic_stream();
            ds << "[ImportDiag] reader"
               << " | mode=" << read_cur.decompressor_name
               << " | compressed=" << (read_cur.compressed ? "yes" : "no")
               << " | queue-capacity=" << read_cur.queue_capacity
               << " | batches-enqueued=" << read_batches_delta
               << " | entries-enqueued=" << read_entries_delta
               << " | source=" << std::fixed << std::setprecision(2) << bytes_to_mib(static_cast<double>(read_source_delta) / seconds) << " MiB/s"
               << " | output=" << bytes_to_mib(static_cast<double>(read_output_delta) / seconds) << " MiB/s"
               << " | reader-blocked(full)=" << ns_to_s(read_wait_full_delta) << "s"
               << " | max-queue-fill=" << read_cur.max_queue_size << "/" << read_cur.queue_capacity
               << std::endl;
        }

        {
            auto ds = n->diagnostic_stream();
            ds << "[ImportDiag] top steps";
            const size_t limit = std::min<size_t>(6, top.size());
            for (size_t i = 0; i < limit; ++i)
            {
                const auto& [name, ns] = top[i];
                const double pct       = entry_delta > 0 ? 100.0 * static_cast<double>(ns) / static_cast<double>(entry_delta) : 0.0;
                ds << " | " << name << "=" << std::fixed << std::setprecision(2) << ns_to_s(ns) << "s (" << pct << "%)";
            }
            ds << std::endl;
        }

        if (read_cur.using_external_decompressor)
        {
            auto ds = n->diagnostic_stream();
            ds << "[ImportDiag] note: cpu(main) excludes external decompressor process '" << read_cur.decompressor_name << "'."
               << std::endl;
        }
#else
        (void)n;
        (void)cur;
        (void)prev;
        (void)read_cur;
        (void)read_prev;
        (void)now;
        (void)last;
        (void)cpu_cur_ns;
        (void)cpu_prev_ns;
        (void)current_bytes;
        (void)total_size;
        (void)export_constraints;
        (void)node_count;
        (void)num_threads;
#endif
    }
}