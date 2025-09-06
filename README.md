# wtako-network-monitor

A high-frequency network monitoring extension for the `wtako-monitor` project, providing deep insights into network health, performance, and security.

This service runs independently and exposes a detailed metrics API designed to be consumed by the `wtako-monitor` dashboard. It uses Node.js, TypeScript, Redis, and BullMQ to perform continuous, asynchronous network analysis.

## Features

-   **High-Frequency Ping Monitoring**: Tracks latency and packet loss every 5 seconds to assess real-time network stability.
-   **Historical Performance Analysis**: Calculates and provides moving averages for latency and packet loss over 1-minute, 5-minute, 1-hour, and 24-hour windows.
-   **Outage Detection**: Automatically identifies network outages, logging their start and end times.
-   **Minute-by-Minute Timeline**: Generates a 60-minute historical timeline of network health, perfect for visualizing recent stability on the `wtako-monitor` frontend.
-   **Port Scanning**: Periodically uses `nmap` to scan for open ports on both a public domain and a private Tailscale IP.
-   **Public IP Tracking**: Monitors and logs changes to your public IP address.
-   **Resilient Architecture**: Utilizes a producer/worker pattern with Redis Streams and BullMQ to ensure data collection and processing are decoupled and fault-tolerant.
-   **Simple Integration**: Exposes a single `GET /metrics` JSON endpoint for easy consumption by `wtako-monitor`.

## Architecture

The system is composed of key components orchestrated by Docker Compose:

1.  **Redis**: Serves as the data backbone, storing historical data in streams and managing the BullMQ job queue.
2.  **Producer (`index.ts`)**: A lightweight process that runs checks at set intervals (ping, DNS, port scan). It writes raw data into Redis Streams and adds a `generateMetrics` job to a BullMQ queue.
3.  **Worker (`worker.ts`)**: A BullMQ consumer that processes `generateMetrics` jobs. It reads raw data from Redis, performs all calculations (e.g., historical averages, outage detection), and caches the complete JSON report in a Redis key.
4.  **HTTP Server (`index.ts`)**: A simple, non-blocking server that serves the pre-calculated JSON report from the Redis cache via the `/metrics` endpoint. This ensures API requests are always fast and do not trigger new scans.

## Prerequisites

-   Docker and Docker Compose
-   A running instance of [wtako-monitor](https://github.com/your-username/wtako-monitor) to consume and display the metrics.

Note: The included `Dockerfile` for the `app` service handles the installation of `nmap`.
## Installation and Configuration

1.  Clone the repository.

2.  **Configure Environment Variables** in `docker-compose.yml`:
    -   `DOMAIN_NAME`: Your public domain name (e.g., `your.domain.tld`) for public IP resolution and port scanning.
    -   `TAILSCALE_IP`: A private IP address (e.g., a Tailscale IP) for internal port scanning.

3.  **Build and Start the Service**:
    ```bash
    docker-compose up -d --build
    ```
    The service will be available on port `8080`.

## Usage and Integration

Once running, `wtako-network-monitor` exposes its data at `http://localhost:8080/metrics`.

To integrate with `wtako-monitor`, configure its backend (`wtako-monitor/src/systemMonitor.ts`) to fetch data from this endpoint. The `wtako-monitor` server should then emit these metrics through its WebSocket connection to the frontend.

**Example `systemMonitor.ts` modification in `wtako-monitor`:**

```typescript
// In your wtako-monitor/src/systemMonitor.ts or a similar file

// Periodically fetch network metrics
async updateNetworkMetrics() {
    try {
        const response = await fetch('http://localhost:8080/metrics');
        if (response.ok) {
            this.networkMetrics = await response.json();
        }
    } catch (error) {
        console.error('Failed to fetch network metrics:', error);
    }
}

// Ensure updateNetworkMetrics() is called on an interval,
// and the result is stored and sent to the frontend.
```

The data structure from the `/metrics` endpoint is designed to be directly used by the `networkMetrics` state object in the `wtako-monitor` frontend.

### Simple Ping Check

This repository includes `check_ping.sh`, a script for basic internet connectivity checks. To use it, the host being monitored needs to report its internet connection status.

The script pings `1.1.1.1` and then `8.8.8.8` (if the first fails), writing the output of the first successful ping to `/tmp/ping`. If both pings fail, an empty file is created.

To provide continuous monitoring, the script should be run in a loop. For example, to check every 5 seconds:
```bash
while true; do /path/to/wtako-network-monitor/check_ping.sh; sleep 5; done
```

This process can be run in the background (e.g., using `nohup` or a `systemd` service).

To make the ping status available to `wtako-monitor` or another web service, you can create a symbolic link from `/tmp/ping` into a publicly accessible directory.
```bash
# Example for wtako-monitor, assuming it serves a 'public' directory
ln -s /tmp/ping /path/to/wtako-monitor/public/latest-ping
```

The ping result can then be fetched from `http://<monitor-host>/latest-ping`.

### API Endpoint

-   **`GET /metrics`**

    Returns a JSON object containing the latest comprehensive network analysis. The data is cached and updates after each processing cycle.

    **Sample Response:**
    ```json
    {
      "ip_history": [
        { "ip": "203.0.113.10", "timestamp": "2023-10-27T10:00:00.000Z" }
      ],
      "internet_ports": ["80/tcp open", "443/tcp open"],
      "tailscale_ports": ["22/tcp open", "3000/tcp open"],
      "ping_statistics": {
        "latency": {
          "latest": 12.5,
          "last1m": 13.1,
          "last5m": 14.2,
          "last1h": 15.0,
          "last24h": 15.5
        },
        "packet_loss": {
          "latest_percent": 0.0,
          "last1m_percent": 0.1,
          "last5m_percent": 0.2,
          "last1h_percent": 0.5,
          "last24h_percent": 1.0
        },
        "outages": [
          {
            "start": "2023-10-27T08:30:00.000Z",
            "end": "2023-10-27T08:32:15.000Z",
            "duration_seconds": 135
          }
        ],
        "minute_history": [
          { "timestamp": "2023-10-27T11:00:00.000Z", "latency_ms": 12.0, "packet_loss_percent": 0 },
          { "timestamp": "2023-10-27T11:01:00.000Z", "latency_ms": 12.5, "packet_loss_percent": 0 },
          // ... 58 more entries
        ]
      },
      "last_updated": 1698397261000
    }
    ```

## License
This project is licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).

