#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <napi.h>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp" // From nlohmann
#include <sw/redis++/redis++.h>

using json = nlohmann::json;

// --- Utility Functions ---

// The type of result from redis-plus-plus for stream commands
using StreamResult = std::vector<
    std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>;

long long get_timestamp_ms(const std::string &redis_id) {
  return std::stoll(redis_id.substr(0, redis_id.find('-')));
}

std::string to_iso_string(long long ms_since_epoch) {
  auto time_point = std::chrono::time_point<std::chrono::system_clock,
                                            std::chrono::milliseconds>(
      std::chrono::milliseconds(ms_since_epoch));
  auto in_time_t = std::chrono::system_clock::to_time_t(time_point);
  std::stringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%S");
  long long ms = ms_since_epoch % 1000;
  ss << '.' << std::setfill('0') << std::setw(3) << ms << 'Z';
  return ss.str();
}

// NOTE: This function is no longer needed as we're directly using StreamResult
// Helper to convert redis-plus-plus stream data to nlohmann::json
// This mimics the structure from ioredis to minimize changes to existing code
/*
template <typename T> json redis_stream_to_json(T &redis_result) {
  json arr = json::array();
  for (const auto &msg : redis_result) {
    json msg_json = json::array();
    msg_json.push_back(msg.first); // id
    json fields_json = json::array();
    for (const auto &field : msg.second) {
      fields_json.push_back(field.first);
      fields_json.push_back(field.second);
    }
    msg_json.push_back(fields_json);
    arr.push_back(msg_json);
  }
  return arr;
}
*/

// Forward declarations for processing functions
json process_ip_history(const StreamResult &ip_history_entries);
json detect_outages(const StreamResult &ping_entries);
json generate_ping_minute_history(long long now, int history_minutes,
                                  const StreamResult &ping_entries);
json calculate_all_historical_ping_averages(long long now,
                                            const StreamResult &ping_entries);
json process_traffic_data(long long now, int history_minutes,
                          const StreamResult &traffic_entries);

// --- Main Addon Function ---

Napi::String GenerateMetrics(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "String expected").ThrowAsJavaScriptException();
    return Napi::String::New(env, "");
  }

  try {
    std::string worker_input_str = info[0].As<Napi::String>().Utf8Value();
    json worker_input = json::parse(worker_input_str);

    // --- Redis setup ---
    sw::redis::Redis redis(worker_input["redisUri"].get<std::string>());
    json stream_keys = worker_input["streamKeys"];
    int history_minutes = worker_input["historyMinutes"];

    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    long long thirty_days_ago_ms = now - 30LL * 86400 * 1000;
    long long one_day_ago_ms = now - 24LL * 60 * 60 * 1000;

    // --- Fetch data from Redis using a pipeline ---
    auto pipe = redis.pipeline(false);
    pipe.xrange(stream_keys["RESOLVED_IPS"].get<std::string>(), "-", "+");
    pipe.xrevrange(stream_keys["INTERNET_OPEN_PORTS"].get<std::string>(), "+",
                   "-", 1);
    pipe.xrevrange(stream_keys["TAILSCALE_OPEN_PORTS"].get<std::string>(), "+",
                   "-", 1);
    pipe.xrevrange(stream_keys["PING_STATS"].get<std::string>(), "+", "-", 1);
    pipe.xrange(stream_keys["PING_STATS"].get<std::string>(),
                std::to_string(one_day_ago_ms), "+");
    pipe.xrange(stream_keys["NETWORK_TRAFFIC_STATS"].get<std::string>(),
                std::to_string(thirty_days_ago_ms), "+");
    auto replies = pipe.exec();
    // --- Get results ---
    StreamResult ip_history_val;
    replies.get(0, std::back_inserter(ip_history_val));
    StreamResult domain_ports_val;
    replies.get(1, std::back_inserter(domain_ports_val));
    StreamResult ip_ports_val;
    replies.get(2, std::back_inserter(ip_ports_val));
    StreamResult latest_ping_val;
    replies.get(3, std::back_inserter(latest_ping_val));
    StreamResult ping_entries_val;
    replies.get(4, std::back_inserter(ping_entries_val));
    StreamResult traffic_entries_val;
    replies.get(5, std::back_inserter(traffic_entries_val));
    // --- IP History ---
    json formatted_ip_history = process_ip_history(ip_history_val);

    // --- Ports ---
    std::vector<std::string> domain_ports;
    if (!domain_ports_val.empty() && !domain_ports_val[0].second.empty()) {
      std::string port_str = domain_ports_val[0].second[0].second;
      std::stringstream ss(port_str);
      std::string segment;
      while (std::getline(ss, segment, ';')) {
        if (!segment.empty())
          domain_ports.push_back(segment);
      }
    }
    std::vector<std::string> ip_ports;
    if (!ip_ports_val.empty() && !ip_ports_val[0].second.empty()) {
      std::string port_str = ip_ports_val[0].second[0].second;
      std::stringstream ss(port_str);
      std::string segment;
      while (std::getline(ss, segment, ';')) {
        if (!segment.empty())
          ip_ports.push_back(segment);
      }
    }

    // --- Ping Stats ---
    json ping_stats;
    double latest_latency = -1.0;
    int latest_loss_raw = 1;
    if (!latest_ping_val.empty() && latest_ping_val[0].second.size() >= 2) {
      latest_latency = std::stod(latest_ping_val[0].second[0].second);
      latest_loss_raw = std::stoi(latest_ping_val[0].second[1].second);
    }

    ping_stats["outages"] = detect_outages(ping_entries_val);
    ping_stats["minute_history"] =
        generate_ping_minute_history(now, history_minutes, ping_entries_val);

    json ping_historical_averages =
        calculate_all_historical_ping_averages(now, ping_entries_val);
    ping_stats["latency"] = ping_historical_averages["latency"];
    ping_stats["packet_loss"] = ping_historical_averages["loss"];
    ping_stats["latency"]["latest"] =
        (latest_latency != -1.0) ? round(latest_latency * 100.0) / 100.0 : -1;
    ping_stats["packet_loss"]["latest_percent"] =
        round(static_cast<double>(latest_loss_raw) * 100.0 * 100.0) / 100.0;

    // --- Network Traffic ---
    json network_traffic_stats =
        process_traffic_data(now, history_minutes, traffic_entries_val);

    // --- Final Assembly ---
    json result;
    result["ip_history"] = formatted_ip_history;
    result["internet_ports"] = domain_ports;
    result["tailscale_ports"] = ip_ports;
    result["ping_statistics"] = ping_stats;
    result["network_traffic"] = network_traffic_stats;

    return Napi::String::New(env, result.dump());
  } catch (const sw::redis::Error &e) {
    Napi::Error::New(env, "Redis error: " + std::string(e.what()))
        .ThrowAsJavaScriptException();
  } catch (const std::exception &e) {
    Napi::Error::New(env, "Error: " + std::string(e.what()))
        .ThrowAsJavaScriptException();
  }
  return Napi::String::New(env, "");
}

// --- Implementations of Processing Functions ---

json process_ip_history(const StreamResult &ip_history_entries) {
  json formatted_history = json::array();
  for (const auto &entry : ip_history_entries) {
    if (entry.second.empty())
      continue;
    formatted_history.push_back(
        {{"ip", entry.second[0].second},
         {"timestamp", to_iso_string(get_timestamp_ms(entry.first))}});
  }

  json final_history = json::array();
  int start = std::max(0, (int)formatted_history.size() - 10);
  for (int i = formatted_history.size() - 1; i >= start; --i) {
    final_history.push_back(formatted_history[i]);
  }
  return final_history;
}

json detect_outages(const StreamResult &ping_entries) {
  json outages = json::array();
  long long outage_start_ts = 0;
  int consecutive_misses = 0;
  // This vector mimics the JS `seenTimestamps` array, holding a sliding window
  // of recent timestamps.
  std::vector<long long> seen_timestamps;
  const int OUTAGE_THRESHOLD = 3; // As specified in the JS code
  for (const auto &entry : ping_entries) {
    long long timestamp = get_timestamp_ms(entry.first);

    // Add current timestamp to our window
    seen_timestamps.push_back(timestamp);
    // Keep the window size at a maximum of OUTAGE_THRESHOLD
    if (seen_timestamps.size() > OUTAGE_THRESHOLD) {
      seen_timestamps.erase(seen_timestamps.begin());
    }

    if (entry.second.size() < 2)
      continue; // Expecting at least latency and loss fields
    int loss = std::stoi(entry.second[1].second);

    if (loss == 1) { // Current ping is a miss
      consecutive_misses++;

      // If we have enough misses to trigger an outage and one isn't already
      // active
      if (consecutive_misses >= OUTAGE_THRESHOLD && outage_start_ts == 0) {
        // The outage started at the time of the first miss in this sequence,
        // which is the oldest timestamp in our current window.
        if (!seen_timestamps.empty()) {
          outage_start_ts = seen_timestamps.front();
        }
      }
    } else { // Current ping is a success
      // If an outage was in progress, it has now ended.
      if (outage_start_ts != 0) {
        outages.push_back(
            {{"start", to_iso_string(outage_start_ts)},
             {"end", to_iso_string(timestamp)},
             {"duration_seconds",
              (long)round((timestamp - outage_start_ts) / 1000.0)}});
        outage_start_ts = 0;
      }
      // A successful ping breaks any sequence of misses and resets the window.
      consecutive_misses = 0;
      seen_timestamps.clear();
    }
  }

  // If the loop finishes and an outage is still in progress, record it as
  // ongoing.
  if (outage_start_ts != 0) {
    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    outages.push_back({{"start", to_iso_string(outage_start_ts)},
                       {"end", to_iso_string(now_ms)},
                       {"duration_seconds",
                        (long)round((now_ms - outage_start_ts) / 1000.0)}});
  }

  // The provided JS code does not limit the number of outages, so the limit is
  // removed.
  return outages;
}

struct PingMinuteBucket {
  double latency_sum = 0;
  int valid_latency_count = 0;
  int loss_count = 0;
  int total_entries = 0;
};

json generate_ping_minute_history(long long now, int history_minutes,
                                  const StreamResult &ping_entries) {
  long long rounded_minute = (now - (now % 60000)) + 60000;
  long long min_timestamp = rounded_minute - (long long)history_minutes * 60000;
  std::map<long long, PingMinuteBucket> buckets;

  for (const auto &entry : ping_entries) {
    long long timestamp = get_timestamp_ms(entry.first);
    if (timestamp < min_timestamp)
      continue;
    long long minute_key = floor(timestamp / 60000.0) * 60000;
    if (entry.second.size() < 2)
      continue; // Expecting at least latency and loss fields
    buckets[minute_key].total_entries++;
    try {
      double latency = std::stod(entry.second[0].second);
      buckets[minute_key].latency_sum += latency;
      buckets[minute_key].valid_latency_count++;
    } catch (...) { /* ignore if not a double */
    }
    buckets[minute_key].loss_count += std::stoi(entry.second[1].second);
  }

  json minute_data = json::array();
  for (int i = 0; i < history_minutes; ++i) {
    long long window_start =
        rounded_minute - ((long long)history_minutes - i) * 60000;
    long long minute_key = floor(window_start / 60000.0) * 60000;

    json minute_point;
    minute_point["timestamp"] = to_iso_string(window_start);

    if (buckets.count(minute_key)) {
      auto &bucket = buckets[minute_key];
      minute_point["latency_ms"] =
          bucket.valid_latency_count > 0
              ? round((bucket.latency_sum / bucket.valid_latency_count) *
                      100.0) /
                    100.0
              : -1;
      minute_point["packet_loss_percent"] =
          bucket.total_entries > 0 ? round(((double)bucket.loss_count /
                                            bucket.total_entries * 100.0) *
                                           100.0) /
                                         100.0
                                   : -1;
    } else {
      minute_point["latency_ms"] = -1;
      minute_point["packet_loss_percent"] = -1;
    }
    minute_data.push_back(minute_point);
  }
  return minute_data;
}

json calculate_all_historical_ping_averages(long long now,
                                            const StreamResult &ping_entries) {
  json latency_results, loss_results;
  std::vector<std::pair<std::string, long long>> window_configs = {
      {"last1m", 60000},
      {"last5m", 300000},
      {"last1h", 3600000},
      {"last24h", 86400000}};

  // Assuming ping_entries are sorted by time, which they are from xrange
  for (const auto &config : window_configs) {
    long long min_timestamp = now - config.second;
    double latency_sum = 0;
    int valid_count = 0, loss_count = 0, total_entries = 0;

    for (const auto &entry : ping_entries) {
      long long timestamp = get_timestamp_ms(entry.first);
      if (timestamp < min_timestamp)
        continue;
      if (entry.second.size() < 2)
        continue; // Expecting at least latency and loss fields

      total_entries++;
      try {
        double latency = std::stod(entry.second[0].second);
        latency_sum += latency;
        valid_count++;
      } catch (...) {
      }
      loss_count += std::stoi(entry.second[1].second);
    }
    latency_results[config.first] =
        valid_count > 0 ? round((latency_sum / valid_count) * 100.0) / 100.0
                        : -1;
    loss_results[config.first] =
        total_entries > 0
            ? round(((double)loss_count / total_entries * 100.0) * 100.0) /
                  100.0
            : -1;
  }
  return {{"latency", latency_results}, {"loss", loss_results}};
}

// ... Traffic Processing Functions to be added here ... (in next comment block)
// (Continued from above, append to src/metrics.cpp)

struct TrafficEntry {
  long long timestamp;
  long long rx;
  long long tx;
  int uptime;
};
struct TrafficDelta {
  long long timestamp;
  long long rx_delta;
  long long tx_delta;
  double time_delta_s;
};

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

json process_traffic_data(long long now, int history_minutes,
                          const StreamResult &traffic_entries) {
  // 1. Parse traffic data
  std::vector<TrafficEntry> parsed_entries;
  for (const auto &entry : traffic_entries) {
    TrafficEntry parsed_entry;
    parsed_entry.timestamp = get_timestamp_ms(entry.first);
    // Initialize with default values
    parsed_entry.rx = 0;
    parsed_entry.tx = 0;
    parsed_entry.uptime = 0;

    for (const auto &field : entry.second) {
      if (field.first == "rx")
        parsed_entry.rx = std::stoll(field.second);
      else if (field.first == "tx")
        parsed_entry.tx = std::stoll(field.second);
      else if (field.first == "uptime")
        parsed_entry.uptime = std::stoi(field.second);
    }
    parsed_entries.push_back(parsed_entry);
  }

  // 2. Calculate deltas
  std::vector<TrafficDelta> deltas;
  for (size_t i = 1; i < parsed_entries.size(); ++i) {
    const auto &current = parsed_entries[i];
    const auto &prev = parsed_entries[i - 1];
    double time_delta_s = (current.timestamp - prev.timestamp) / 1000.0;
    if (time_delta_s <= 0)
      continue;

    TrafficDelta delta;
    delta.timestamp = current.timestamp;
    delta.time_delta_s = time_delta_s;
    if (current.uptime < prev.uptime) { // reboot detected
      // For reboot, assume current reading is the total since reboot.
      // This is a simplification; ideally, we'd only count traffic *since* last
      // boot. But given we just want deltas, this is effectively treating it as
      // a reset.
      delta.rx_delta = current.rx;
      delta.tx_delta = current.tx;
    } else {
      delta.rx_delta = std::max(0LL, current.rx - prev.rx);
      delta.tx_delta = std::max(0LL, current.tx - prev.tx);
    }
    deltas.push_back(delta);
  }
  std::reverse(deltas.begin(),
               deltas.end()); // For efficient processing in next step

  // 3. Calculate historical averages
  json historical_averages;
  std::vector<std::pair<std::string, long long>> window_configs = {
      {"last1m", 60000},      {"last5m", 300000},    {"last15m", 900000},
      {"last1h", 3600000},    {"last3h", 10800000},  {"last12h", 43200000},
      {"last1d", 86400000},   {"last3d", 259200000}, {"last7d", 604800000},
      {"last30d", 2592000000}};
  long long cumulative_rx = 0, cumulative_tx = 0;
  double total_time_s = 0;
  size_t current_delta_read_idx =
      0; // To avoid re-iterating over deltas already accounted for

  for (const auto &config : window_configs) {
    long long min_timestamp = now - config.second;

    // Add deltas to current window, starting from where we left off
    for (size_t i = current_delta_read_idx; i < deltas.size(); ++i) {
      const auto &delta = deltas[i];
      if (delta.timestamp < min_timestamp) {
        // This delta is too old for the current window and all subsequent
        // smaller windows
        current_delta_read_idx =
            i; // Mark this as the new starting point for next window
        break;
      }
      cumulative_rx += delta.rx_delta;
      cumulative_tx += delta.tx_delta;
      total_time_s += delta.time_delta_s;

      if (i == deltas.size() - 1) { // Reached end of deltas
        current_delta_read_idx = deltas.size();
      }
    }

    historical_averages[config.first] = {
        {"cum_rx", cumulative_rx},
        {"cum_tx", cumulative_tx},
        {"avg_rx_Bps", total_time_s > 0
                           ? round((cumulative_rx / total_time_s) * 100) / 100.0
                           : 0},
        {"avg_tx_Bps", total_time_s > 0
                           ? round((cumulative_tx / total_time_s) * 100) / 100.0
                           : 0}};
  }

  // 4. Generate minute history
  json minute_history = json::array();
  long long rounded_minute = (now - (now % 60000)) + 60000;
  long long oldest_minute_start =
      rounded_minute - (long long)history_minutes * 60000;
  std::map<long long, TrafficMinuteBucket> buckets;

  // Iterate oldest_minute_start up to rounded_minute - 1 minute (i.e. exclude
  // current incomplete minute) Traffic data is often lagging, so this attempts
  // to get completed minutes.
  long long history_end_time = rounded_minute;

  for (const auto &delta : deltas) {
    if (delta.timestamp < oldest_minute_start)
      break; // Deltas are reversed, so if this one is too old, all subsequent
             // are too.
    if (delta.timestamp >=
        history_end_time) // only process data up to history_end_time
      continue;

    long long minute_key = floor(delta.timestamp / 60000.0) * 60000;
    buckets[minute_key].total_rx_in_minute += delta.rx_delta;
    buckets[minute_key].total_tx_in_minute += delta.tx_delta;
    buckets[minute_key].total_time_s += delta.time_delta_s;
  }

  // Populate minute_history with results for each minute
  for (int i = 0; i < history_minutes; ++i) {
    long long window_start = oldest_minute_start + i * 60000;

    // Do not generate future minutes
    if (window_start >= history_end_time) {
      break;
    }

    json point;
    point["timestamp"] = to_iso_string(window_start);

    if (buckets.count(window_start)) {
      auto &bucket = buckets[window_start];
      point["avg_rx_Bps"] =
          bucket.total_time_s > 0
              ? round((bucket.total_rx_in_minute / bucket.total_time_s) * 100) /
                    100.0
              : 0;
      point["avg_tx_Bps"] =
          bucket.total_time_s > 0
              ? round((bucket.total_tx_in_minute / bucket.total_time_s) * 100) /
                    100.0
              : 0;
      point["total_rx_in_minute"] = bucket.total_rx_in_minute;
      point["total_tx_in_minute"] = bucket.total_tx_in_minute;
    } else {
      point["avg_rx_Bps"] = 0;
      point["avg_tx_Bps"] = 0;
      point["total_rx_in_minute"] = 0;
      point["total_tx_in_minute"] = 0;
    }
    minute_history.push_back(point);
  }
  return {{"historical", historical_averages},
          {"minute_history", minute_history}};
}
// --- NAPI Boilerplate ---

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set(Napi::String::New(env, "generate"),
              Napi::Function::New(env, GenerateMetrics));
  return exports;
}

NODE_API_MODULE(metrics, Init)
