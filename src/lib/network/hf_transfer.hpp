/*
Copyright (c) 2026 acrion innovations GmbH

Bounded, observable curl transport for Hugging Face cache fetches.
*/
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace zelph::network::detail::hf_transfer
{
    inline constexpr const char* host_stats_version = "v1";

    enum class Operation
    {
        probe,
        fetch,
    };

    struct Limits
    {
        uint64_t connect_timeout_seconds             = 15;
        uint64_t probe_timeout_seconds               = 45;
        uint64_t stall_window_seconds                = 90;
        uint64_t min_progress_bytes_per_second       = 1024;
        uint64_t initial_throughput_bytes_per_second = 262144;
    };

    struct HostStats
    {
        std::string host;
        uint64_t    throughput_bytes_per_second = 0;
        uint64_t    samples                     = 0;
    };

    struct Request
    {
        Operation   operation = Operation::fetch;
        std::string source_uri;
        std::string url;
        std::string output_path;
        std::string header_path;
        std::string metrics_path;
        std::string revision;
        std::string etag;
        uint64_t    offset        = 0;
        uint64_t    planned_bytes = 0;
    };

    struct Metrics
    {
        uint64_t dns_milliseconds         = 0;
        uint64_t connect_milliseconds     = 0;
        uint64_t tls_milliseconds         = 0;
        uint64_t first_byte_milliseconds  = 0;
        uint64_t total_milliseconds       = 0;
        uint64_t received_bytes           = 0;
        uint64_t average_bytes_per_second = 0;
        uint64_t http_status              = 0;
        uint64_t curl_exit                = 0;
    };

    enum class Outcome
    {
        completed,
        slow_completed,
        connection_timeout,
        response_timeout,
        stalled,
        transfer_deadline,
        failed_http,
        failed_curl,
    };

    inline const char* operation_name(const Operation operation)
    {
        return operation == Operation::probe ? "probe" : "fetch";
    }

    inline const char* outcome_name(const Outcome outcome)
    {
        switch (outcome)
        {
        case Outcome::completed:
            return "completed";
        case Outcome::slow_completed:
            return "slow_completed";
        case Outcome::connection_timeout:
            return "connection_timeout";
        case Outcome::response_timeout:
            return "response_timeout";
        case Outcome::stalled:
            return "stalled";
        case Outcome::transfer_deadline:
            return "transfer_deadline";
        case Outcome::failed_http:
            return "failed_http";
        case Outcome::failed_curl:
            return "failed_curl";
        }
        return "failed_curl";
    }

    inline uint64_t environment_uint64(const char* name, const uint64_t fallback)
    {
        const char* value = std::getenv(name);
        if (!value || !*value)
        {
            return fallback;
        }
        try
        {
            const auto parsed = std::stoull(value);
            return parsed > 0 ? parsed : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    }

    inline Limits limits_from_environment()
    {
        Limits limits;
        limits.connect_timeout_seconds       = environment_uint64("ZELPH_HF_CONNECT_TIMEOUT_SECONDS", limits.connect_timeout_seconds);
        limits.probe_timeout_seconds         = environment_uint64("ZELPH_HF_PROBE_TIMEOUT_SECONDS", limits.probe_timeout_seconds);
        limits.stall_window_seconds          = environment_uint64("ZELPH_HF_STALL_WINDOW_SECONDS", limits.stall_window_seconds);
        limits.min_progress_bytes_per_second = environment_uint64(
            "ZELPH_HF_MIN_PROGRESS_BYTES_PER_SECOND", limits.min_progress_bytes_per_second);
        limits.initial_throughput_bytes_per_second = environment_uint64(
            "ZELPH_HF_INITIAL_THROUGHPUT_BYTES_PER_SECOND", limits.initial_throughput_bytes_per_second);
        return limits;
    }

    inline std::string shell_quote(const std::string& value)
    {
        std::string out = "'";
        for (const char c : value)
        {
            if (c == '\'')
                out += "'\"'\"'";
            else
                out.push_back(c);
        }
        out += "'";
        return out;
    }

    inline std::string curl_binary_from_environment()
    {
        const char* configured = std::getenv("ZELPH_HF_CURL_BIN");
        return configured && *configured ? configured : "curl";
    }

    inline std::string host_from_url(const std::string& url)
    {
        const auto scheme = url.find("://");
        const auto start  = scheme == std::string::npos ? 0 : scheme + 3;
        const auto end    = url.find('/', start);
        return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    inline std::string stable_token(const std::string& value)
    {
        uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char byte : value)
        {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return std::to_string(hash);
    }

    inline std::filesystem::path host_stats_path(const std::filesystem::path& cache_root, const std::string& host)
    {
        return cache_root / ("transfer-host_" + stable_token(host) + ".meta");
    }

    inline std::optional<HostStats> read_host_stats(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        if (!input)
        {
            return std::nullopt;
        }
        std::string version;
        HostStats   stats;
        std::string line;
        while (std::getline(input, line))
        {
            const auto equals = line.find('=');
            if (equals == std::string::npos) continue;
            const auto key   = line.substr(0, equals);
            const auto value = line.substr(equals + 1);
            try
            {
                if (key == "version")
                    version = value;
                else if (key == "host")
                    stats.host = value;
                else if (key == "throughput_bytes_per_second")
                    stats.throughput_bytes_per_second = std::stoull(value);
                else if (key == "samples")
                    stats.samples = std::stoull(value);
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
        if (version != host_stats_version || stats.host.empty() || stats.throughput_bytes_per_second == 0)
        {
            return std::nullopt;
        }
        return stats;
    }

    inline bool write_host_stats(const std::filesystem::path& path, const HostStats& stats)
    {
        std::ofstream output(path, std::ios::trunc);
        if (!output) return false;
        output << "version=" << host_stats_version << '\n'
               << "host=" << stats.host << '\n'
               << "throughput_bytes_per_second=" << stats.throughput_bytes_per_second << '\n'
               << "samples=" << stats.samples << '\n';
        return static_cast<bool>(output);
    }

    inline uint64_t conservative_rate(const std::optional<HostStats>& stats, const Limits& limits)
    {
        constexpr uint64_t minimum  = 64 * 1024;
        constexpr uint64_t maximum  = 32 * 1024 * 1024;
        const uint64_t     baseline = stats ? stats->throughput_bytes_per_second / 4
                                            : limits.initial_throughput_bytes_per_second;
        return std::clamp(baseline, minimum, maximum);
    }

    inline uint64_t payload_timeout_seconds(const uint64_t planned_bytes,
                                            const uint64_t rate,
                                            const Limits&  limits)
    {
        const uint64_t transfer_seconds = planned_bytes == 0 ? 0 : (planned_bytes + rate - 1) / rate;
        return std::max<uint64_t>(limits.stall_window_seconds + limits.connect_timeout_seconds,
                                  uint64_t{45} + (uint64_t{4} * transfer_seconds));
    }

    inline std::string build_curl_command(const Request& request,
                                          const Limits&  limits,
                                          const uint64_t rate)
    {
        const uint64_t max_time = request.operation == Operation::probe
                                    ? limits.probe_timeout_seconds
                                    : payload_timeout_seconds(request.planned_bytes, rate, limits);
        std::string    command  = shell_quote(curl_binary_from_environment()) + " -fsSL --retry 2 --connect-timeout "
                                + std::to_string(limits.connect_timeout_seconds)
                                + " --max-time " + std::to_string(max_time);
        if (request.operation == Operation::probe)
        {
            command += " -I --retry-max-time " + std::to_string(limits.probe_timeout_seconds)
                     + " -o /dev/null -D " + shell_quote(request.header_path);
        }
        else
        {
            command += " --speed-limit " + std::to_string(limits.min_progress_bytes_per_second)
                     + " --speed-time " + std::to_string(limits.stall_window_seconds);
            if (request.planned_bytes > 0)
            {
                command += " --range " + std::to_string(request.offset) + "-"
                         + std::to_string(request.offset + request.planned_bytes - 1);
            }
            command += " -o " + shell_quote(request.output_path);
        }
        command += " --write-out " + shell_quote("%{json}\\n") + " " + shell_quote(request.url)
                 + " > " + shell_quote(request.metrics_path);
        return command;
    }

    inline std::optional<double> json_number(const std::string& body, const std::string& key)
    {
        const auto marker = '"' + key + '"';
        auto       pos    = body.find(marker);
        if (pos == std::string::npos) return std::nullopt;
        pos = body.find(':', pos + marker.size());
        if (pos == std::string::npos) return std::nullopt;
        ++pos;
        while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])))
            ++pos;
        const auto end  = body.find_first_of(",}", pos);
        const auto text = body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (text == "null" || text.empty()) return std::nullopt;
        try
        {
            return std::stod(text);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    inline uint64_t milliseconds(const std::optional<double>& seconds)
    {
        return seconds ? static_cast<uint64_t>(std::llround(*seconds * 1000.0)) : 0;
    }

    inline std::optional<Metrics> read_metrics(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        if (!input) return std::nullopt;
        std::stringstream buffer;
        buffer << input.rdbuf();
        const auto body = buffer.str();
        if (body.empty()) return std::nullopt;
        Metrics metrics;
        metrics.dns_milliseconds         = milliseconds(json_number(body, "time_namelookup"));
        metrics.connect_milliseconds     = milliseconds(json_number(body, "time_connect"));
        metrics.tls_milliseconds         = milliseconds(json_number(body, "time_appconnect"));
        metrics.first_byte_milliseconds  = milliseconds(json_number(body, "time_starttransfer"));
        metrics.total_milliseconds       = milliseconds(json_number(body, "time_total"));
        metrics.received_bytes           = static_cast<uint64_t>(json_number(body, "size_download").value_or(0.0));
        metrics.average_bytes_per_second = static_cast<uint64_t>(json_number(body, "speed_download").value_or(0.0));
        metrics.http_status              = static_cast<uint64_t>(json_number(body, "http_code").value_or(0.0));
        metrics.curl_exit                = static_cast<uint64_t>(json_number(body, "exitcode").value_or(0.0));
        return metrics;
    }

    inline Outcome classify(const Request& request, const Metrics& metrics, const uint64_t conservative_bytes_per_second)
    {
        if (metrics.curl_exit == 0)
        {
            if (request.operation == Operation::fetch && request.planned_bytes >= 64 * 1024
                && metrics.average_bytes_per_second > 0
                && metrics.average_bytes_per_second < conservative_bytes_per_second / 2)
            {
                return Outcome::slow_completed;
            }
            return Outcome::completed;
        }
        if (metrics.http_status >= 400) return Outcome::failed_http;
        if (metrics.curl_exit == 28)
        {
            if (metrics.connect_milliseconds == 0) return Outcome::connection_timeout;
            if (metrics.received_bytes == 0) return Outcome::response_timeout;
            if (request.operation == Operation::fetch
                && metrics.average_bytes_per_second < conservative_bytes_per_second / 2)
            {
                return Outcome::stalled;
            }
            return Outcome::transfer_deadline;
        }
        return Outcome::failed_curl;
    }

    inline void update_host_stats(const std::filesystem::path& cache_root,
                                  const std::string&           host,
                                  const uint64_t               planned_bytes,
                                  const Metrics&               metrics)
    {
        // Header reads and tiny objects are dominated by latency. Retain their
        // per-call diagnostics, but do not let them shrink a shard budget.
        if (planned_bytes < 64 * 1024 || metrics.curl_exit != 0 || metrics.average_bytes_per_second == 0) return;
        const auto     path  = host_stats_path(cache_root, host);
        const auto     prior = read_host_stats(path);
        const uint64_t rate  = prior
                                 ? (prior->throughput_bytes_per_second * 4 + metrics.average_bytes_per_second) / 5
                                 : metrics.average_bytes_per_second;
        write_host_stats(path, {host, rate, prior ? prior->samples + 1 : 1});
    }

    inline std::string json_escape(const std::string& value)
    {
        std::string escaped;
        for (const char c : value)
        {
            if (c == '\n')
            {
                escaped += "\\n";
            }
            else
            {
                if (c == '"' || c == '\\') escaped.push_back('\\');
                escaped.push_back(c);
            }
        }
        return escaped;
    }

    inline void append_diagnostic(const Request&     request,
                                  const Metrics&     metrics,
                                  const Outcome      outcome,
                                  const std::string& cache_state)
    {
        const char* configured = std::getenv("ZELPH_HF_TRANSFER_LOG");
        if (!configured || !*configured) return;
        std::ofstream output(configured, std::ios::app);
        if (!output) return;
        const auto recorded_at = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        output << "{\"recorded_at_epoch_seconds\":" << recorded_at
               << ",\"operation\":\"" << operation_name(request.operation)
               << "\",\"outcome\":\"" << outcome_name(outcome)
               << "\",\"source_uri\":\"" << json_escape(request.source_uri)
               << "\",\"offset\":" << request.offset
               << ",\"planned_bytes\":" << request.planned_bytes
               << ",\"received_bytes\":" << metrics.received_bytes
               << ",\"average_bytes_per_second\":" << metrics.average_bytes_per_second
               << ",\"dns_milliseconds\":" << metrics.dns_milliseconds
               << ",\"connect_milliseconds\":" << metrics.connect_milliseconds
               << ",\"tls_milliseconds\":" << metrics.tls_milliseconds
               << ",\"first_byte_milliseconds\":" << metrics.first_byte_milliseconds
               << ",\"total_milliseconds\":" << metrics.total_milliseconds
               << ",\"http_status\":" << metrics.http_status
               << ",\"curl_exit\":" << metrics.curl_exit
               << ",\"cache_state\":\"" << json_escape(cache_state)
               << "\",\"revision\":\"" << json_escape(request.revision)
               << "\",\"etag\":\"" << json_escape(request.etag) << "\"}\n";
    }
}
