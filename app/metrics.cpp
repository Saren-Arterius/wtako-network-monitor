#include <napi.h>
#include <string>
#include <vector>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

#include "json.hpp" // From nlohmann

using json = nlohmann::json;

// --- Utility Functions ---

long long get_timestamp_ms(const std::string& redis_id) {
    return std::stoll(redis_id.substr(0, redis_id.find('-')));
}

std::string to_iso_string(long long ms_since_epoch) {
    auto time_point = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>(std::chrono::milliseconds(ms_since_epoch));
    auto in_time_t = std::chrono::system_clock::to_time_t(time_point);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%S");
    long long ms = ms_since_epoch % 1000;
    ss << '.' << std::setfill('0') << std::setw(3) << ms << 'Z';
    return ss.str();
}

// Forward declarations for processing functions
json process_ip_history(const json& ip_history_entries);
json detect_outages(const json& ping_entries);
json generate_ping_minute_history(long long now, int history_minutes, const json& ping_entries);
json calculate_all_historical_ping_averages(long long now, const json& ping_entries);
json process_traffic_data(long long now, int history_minutes, const json& traffic_entries);


// --- Main Addon Function ---

Napi::String GenerateMetrics(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "String expected").ThrowAsJavaScriptException();
        return Napi::String::New(env, "");
    }

    std::string raw_data_str = info[0].As<Napi::String>().Utf8Value();
    json raw_data = json::parse(raw_data_str);

    long long now = raw_data["now"];
    int history_minutes = raw_data["historyMinutes"];

    // --- IP History ---
    json formatted_ip_history = process_ip_history(raw_data["ipHistoryEntries"]);

    // --- Ports ---
    std::vector<std::string> domain_ports;
    if (!raw_data["domainPortsEntry"].empty()) {
        std::string port_str = raw_data["domainPortsEntry"][0][1][1];
        std::stringstream ss(port_str);
        std::string segment;
        while(std::getline(ss, segment, ';')) {
            if(!segment.empty()) domain_ports.push_back(segment);
        }
    }
    std::vector<std::string> ip_ports;
    if (!raw_data["ipPortsEntry"].empty()) {
        std::string port_str = raw_data["ipPortsEntry"][0][1][1];
        std::stringstream ss(port_str);
        std::string segment;
        while(std::getline(ss, segment, ';')) {
            if(!segment.empty()) ip_ports.push_back(segment);
        }
    }

    // --- Ping Stats ---
    json ping_stats;
    double latest_latency = -1.0;
    int latest_loss_raw = 1;
    if (!raw_data["latestPingEntry"].empty()) {
        latest_latency = std::stod(raw_data["latestPingEntry"][0][1][1].get<std::string>());
        latest_loss_raw = std::stoi(raw_data["latestPingEntry"][0][1][3].get<std::string>());
    }

    ping_stats["outages"] = detect_outages(raw_data["pingEntries"]);
    ping_stats["minute_history"] = generate_ping_minute_history(now, history_minutes, raw_data["pingEntries"]);

    json ping_historical_averages = calculate_all_historical_ping_averages(now, raw_data["pingEntries"]);
    ping_stats["latency"] = ping_historical_averages["latency"];
    ping_stats["packet_loss"] = ping_historical_averages["loss"];
    ping_stats["latency"]["latest"] = (latest_latency != -1.0) ? round(latest_latency * 100.0) / 100.0 : -1;
    ping_stats["packet_loss"]["latest_percent"] = round(static_cast<double>(latest_loss_raw) * 100.0 * 100.0) / 100.0;


    // --- Network Traffic ---
    json network_traffic_stats = process_traffic_data(now, history_minutes, raw_data["trafficEntries"]);

    // --- Final Assembly ---
    json result;
    result["ip_history"] = formatted_ip_history;
    result["internet_ports"] = domain_ports;
    result["tailscale_ports"] = ip_ports;
    result["ping_statistics"] = ping_stats;
    result["network_traffic"] = network_traffic_stats;

    return Napi::String::New(env, result.dump());
}

// --- Implementations of Processing Functions ---

json process_ip_history(const json& ip_history_entries) {
    json formatted_history = json::array();
    for(const auto& entry : ip_history_entries) {
        formatted_history.push_back({
            {"ip", entry[1][1]},
            {"timestamp", to_iso_string(get_timestamp_ms(entry[0]))}
        });
    }

    json final_history = json::array();
    int start = std::max(0, (int)formatted_history.size() - 10);
    for(int i = formatted_history.size() - 1; i >= start; --i) {
        final_history.push_back(formatted_history[i]);
    }
    return final_history;
}

json detect_outages(const json& ping_entries) {
    json outages = json::array();
    long long outage_start = 0;
    int consecutive_misses = 0;
    std::vector<long long> seen_timestamps;
    const int OUTAGE_THRESHOLD = 2;

    for (const auto& entry : ping_entries) {
        long long timestamp = get_timestamp_ms(entry[0]);
        seen_timestamps.push_back(timestamp);
        if (seen_timestamps.size() > OUTAGE_THRESHOLD) {
            seen_timestamps.erase(seen_timestamps.begin());
        }
        int loss = std::stoi(entry[1][3].get<std::string>());

        if (loss == 1) {
            consecutive_misses++;
            if (consecutive_misses >= OUTAGE_THRESHOLD && outage_start == 0) {
                outage_start = seen_timestamps[0];
            }
        } else {
            if (outage_start != 0) {
                outages.push_back({
                    {"start", to_iso_string(outage_start)},
                    {"end", to_iso_string(timestamp)},
                    {"duration_seconds", (long)round((timestamp - outage_start) / 1000.0)}
                });
                outage_start = 0;
            }
            consecutive_misses = 0;
            seen_timestamps.clear();
        }
    }

    if (outage_start != 0) {
        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        outages.push_back({
            {"start", to_iso_string(outage_start)},
            {"end", to_iso_string(now_ms)},
            {"duration_seconds", (long)round((now_ms - outage_start) / 1000.0)}
        });
    }

    return outages;
}

struct PingMinuteBucket {
    double latency_sum = 0;
    int valid_latency_count = 0;
    int loss_count = 0;
    int total_entries = 0;
};

json generate_ping_minute_history(long long now, int history_minutes, const json& ping_entries) {
    long long rounded_minute = (now - (now % 60000)) + 60000;
    long long min_timestamp = rounded_minute - (long long)history_minutes * 60000;

    std::map<long long, PingMinuteBucket> buckets;

    for (const auto& entry : ping_entries) {
        long long timestamp = get_timestamp_ms(entry[0]);
        if (timestamp < min_timestamp) continue;
        long long minute_key = floor(timestamp / 60000.0) * 60000;

        buckets[minute_key].total_entries++;
        try {
            double latency = std::stod(entry[1][1].get<std::string>());
            buckets[minute_key].latency_sum += latency;
            buckets[minute_key].valid_latency_count++;
        } catch (...) { /* ignore if not a double */ }
        buckets[minute_key].loss_count += std::stoi(entry[1][3].get<std::string>());
    }

    json minute_data = json::array();
    for (int i = 0; i < history_minutes; ++i) {
        long long window_start = rounded_minute - ((long long)history_minutes - i) * 60000;
        long long minute_key = floor(window_start / 60000.0) * 60000;

        json minute_point;
        minute_point["timestamp"] = to_iso_string(window_start);

        if (buckets.count(minute_key)) {
            auto& bucket = buckets[minute_key];
            minute_point["latency_ms"] = bucket.valid_latency_count > 0 ? round((bucket.latency_sum / bucket.valid_latency_count) * 100.0) / 100.0 : -1;
            minute_point["packet_loss_percent"] = bucket.total_entries > 0 ? round(((double)bucket.loss_count / bucket.total_entries * 100.0) * 100.0) / 100.0 : -1;
        } else {
            minute_point["latency_ms"] = -1;
            minute_point["packet_loss_percent"] = -1;
        }
        minute_data.push_back(minute_point);
    }
    return minute_data;
}

json calculate_all_historical_ping_averages(long long now, const json& ping_entries) {
    json latency_results, loss_results;
    std::vector<std::pair<std::string, long long>> window_configs = {
        {"last1m", 60000}, {"last5m", 300000},
        {"last1h", 3600000}, {"last24h", 86400000}
    };

    // Assuming ping_entries are sorted by time, which they are from xrange
    for (const auto& config : window_configs) {
        long long min_timestamp = now - config.second;
        double latency_sum = 0;
        int valid_count = 0, loss_count = 0, total_entries = 0;

        for (const auto& entry : ping_entries) {
             long long timestamp = get_timestamp_ms(entry[0]);
             if (timestamp < min_timestamp) continue;
             
             total_entries++;
             try {
                double latency = std::stod(entry[1][1].get<std::string>());
                latency_sum += latency;
                valid_count++;
             } catch (...) {}
             loss_count += std::stoi(entry[1][3].get<std::string>());
        }
        latency_results[config.first] = valid_count > 0 ? round((latency_sum / valid_count) * 100.0) / 100.0 : -1;
        loss_results[config.first] = total_entries > 0 ? round(((double)loss_count / total_entries * 100.0) * 100.0) / 100.0 : -1;
    }
    return {{"latency", latency_results}, {"loss", loss_results}};
}

// ... Traffic Processing Functions to be added here ... (in next comment block)
// (Continued from above, append to src/metrics.cpp)

struct TrafficEntry { long long timestamp; long long rx; long long tx; int uptime; };
struct TrafficDelta { long long timestamp; long long rx_delta; long long tx_delta; double time_delta_s; };

struct TrafficResult {
    long long cum_rx = 0;
    long long cum_tx = 0;
    double avg_rx_Bps = 0;
    double avg_tx_Bps = 0;
};

struct TrafficMinuteBucket {
    long long total_rx_in_minute = 0;
    long long total_tx_in_minute = 0;
    double total_time_s = 0;
};

json process_traffic_data(long long now, int history_minutes, const json& traffic_entries_json) {
    // 1. Parse traffic data
    std::vector<TrafficEntry> parsed_entries;
    for (const auto& entry_json : traffic_entries_json) {
        TrafficEntry entry;
        entry.timestamp = get_timestamp_ms(entry_json[0]);
        const auto& fields = entry_json[1];
        for (size_t i = 0; i < fields.size(); i += 2) {
            if (fields[i] == "rx") entry.rx = std::stoll(fields[i+1].get<std::string>());
            else if (fields[i] == "tx") entry.tx = std::stoll(fields[i+1].get<std::string>());
            else if (fields[i] == "uptime") entry.uptime = std::stoi(fields[i+1].get<std::string>());
        }
        parsed_entries.push_back(entry);
    }
    
    // 2. Calculate deltas
    std::vector<TrafficDelta> deltas;
    for (size_t i = 1; i < parsed_entries.size(); ++i) {
        const auto& current = parsed_entries[i];
        const auto& prev = parsed_entries[i-1];
        double time_delta_s = (current.timestamp - prev.timestamp) / 1000.0;
        if (time_delta_s <= 0) continue;

        TrafficDelta delta;
        delta.timestamp = current.timestamp;
        delta.time_delta_s = time_delta_s;
        if (current.uptime < prev.uptime) { // reboot
            delta.rx_delta = current.rx;
            delta.tx_delta = current.tx;
        } else {
            delta.rx_delta = std::max(0LL, current.rx - prev.rx);
            delta.tx_delta = std::max(0LL, current.tx - prev.tx);
        }
        deltas.push_back(delta);
    }
    std::reverse(deltas.begin(), deltas.end()); // For efficient processing

    // 3. Calculate historical averages
    json historical_averages;
    std::vector<std::pair<std::string, long long>> window_configs = {
        {"last1m", 60000}, {"last5m", 300000}, {"last15m", 900000},
        {"last1h", 3600000}, {"last3h", 10800000}, {"last12h", 43200000},
        {"last1d", 86400000}, {"last3d", 259200000}, {"last7d", 604800000},
        {"last30d", 2592000000}
    };
    long long cumulative_rx = 0, cumulative_tx = 0;
    double total_time_s = 0;
    size_t current_delta_index = 0;

    for (const auto& config : window_configs) {
        long long min_timestamp = now - config.second;
        for (size_t i = current_delta_index; i < deltas.size() ; ++i) {
            const auto& delta = deltas[i];
            if (delta.timestamp < min_timestamp) {
                current_delta_index = i;
                break;
            }
            cumulative_rx += delta.rx_delta;
            cumulative_tx += delta.tx_delta;
            total_time_s += delta.time_delta_s;
            if (i == deltas.size() -1) current_delta_index = deltas.size();
        }
        
        historical_averages[config.first] = {
            {"cum_rx", cumulative_rx},
            {"cum_tx", cumulative_tx},
            {"avg_rx_Bps", total_time_s > 0 ? round((cumulative_rx/total_time_s)*100)/100.0 : 0},
            {"avg_tx_Bps", total_time_s > 0 ? round((cumulative_tx/total_time_s)*100)/100.0 : 0}
        };
    }

    // 4. Generate minute history
    json minute_history = json::array();
    long long rounded_minute = (now - (now % 60000)) + 60000;
    long long oldest_minute_start = rounded_minute - (long long)history_minutes * 60000;
    std::map<long long, TrafficMinuteBucket> buckets;

    for (const auto& delta : deltas) {
        if (delta.timestamp < oldest_minute_start) break;
        if (delta.timestamp >= rounded_minute) continue;

        long long minute_key = floor(delta.timestamp / 60000.0) * 60000;
        buckets[minute_key].total_rx_in_minute += delta.rx_delta;
        buckets[minute_key].total_tx_in_minute += delta.tx_delta;
        buckets[minute_key].total_time_s += delta.time_delta_s;
    }
    
    for(int i = 0; i < history_minutes; ++i) {
        long long window_start = oldest_minute_start + i * 60000;
        
        json point;
        point["timestamp"] = to_iso_string(window_start);

        if(buckets.count(window_start)) {
            auto& bucket = buckets[window_start];
            point["avg_rx_Bps"] = bucket.total_time_s > 0 ? round((bucket.total_rx_in_minute / bucket.total_time_s)*100)/100.0 : 0;
            point["avg_tx_Bps"] = bucket.total_time_s > 0 ? round((bucket.total_tx_in_minute / bucket.total_time_s)*100)/100.0 : 0;
            point["total_rx_in_minute"] = bucket.total_rx_in_minute;
            point["total_tx_in_minute"] = bucket.total_tx_in_minute;
        } else {
            point["avg_rx_Bps"] = 0;
            point["avg_tx_Bps"] = 0;
            point["total_rx_in_minute"] = 0;
            point["total_tx_in_minute"] = 0;
        }

        // Don't append the last minute data if there's no data (total_rx_in_minute + total_tx_in_minute = 0)
        // This is necessary because the last window_start might be "now" or in the future if rounded_minute is future.
        // It avoids showing a 0-value entry for the current minute when no data has been received yet.
        if (!(i == history_minutes - 1 && point["total_rx_in_minute"] == 0 && point["total_tx_in_minute"] == 0)) {
            minute_history.push_back(point);
        }
    }


    return {
        {"historical", historical_averages},
        {"minute_history", minute_history}
    };
}
// --- NAPI Boilerplate ---

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(Napi::String::New(env, "generate"), Napi::Function::New(env, GenerateMetrics));
    return exports;
}

NODE_API_MODULE(metrics, Init)
