import { Redis } from 'ioredis';
import { exec } from 'child_process';
import { promisify } from 'util';
import Queue from 'bull';
import * as dns from 'dns';
import * as http from 'http';

const DOMAIN_NAME = process.env.DOMAIN_NAME || 'your.domain.tld';
const TAILSCALE_IP = process.env.TAILSCALE_IP || '100.64.0.1';
// const CHECK_IP = '1.1.1.1';
const HTTP_PORT = 8080;
const REDIS_SETTINGS = {
  DATA_RETENTION_DAYS: 7,
  HISTORY_MINUTES: 60,
  STREAM_KEYS: {
    RESOLVED_IPS: 'domain:resolved_ips',
    INTERNET_OPEN_PORTS: `network:open_ports:${DOMAIN_NAME}`,
    TAILSCALE_OPEN_PORTS: `network:open_ports:${TAILSCALE_IP}`,
    PING_STATS: 'network:ping_stats'
  }
};
const redis = new Redis({
  host: '127.0.0.1'
});
const redisBlock = new Redis({
  host: '127.0.0.1'
});

let tsNmapScanning = false;


const execAsync = promisify(exec);
async function addStreamEntry(key: string, data: Record<string, string>) {
  const multi = redis.multi();
  multi.xadd(key, '*', ...Object.entries(data).flat());
  const cutoff = Date.now() - REDIS_SETTINGS.DATA_RETENTION_DAYS * 86400 * 1000;
  multi.xtrim(key, 'MINID', '~', cutoff.toString());
  await multi.exec();
}
async function checkDomainResolution() {
  try {
    const address = await new Promise<string>((resolve, reject) => {
      dns.resolve4(DOMAIN_NAME, (err: any, addresses: any) => {
        if (err) return reject(err);
        resolve(addresses[0]);
      });
    });
    const lastEntry = await redis.xrevrange(REDIS_SETTINGS.STREAM_KEYS.RESOLVED_IPS, '+', '-', 'COUNT', 1);

    if (lastEntry.length > 0 && lastEntry[0][1][1] === address) {
      console.log(`Resolved IP hasn't changed for ${DOMAIN_NAME}`);
      return;
    }

    await addStreamEntry(REDIS_SETTINGS.STREAM_KEYS.RESOLVED_IPS, { ip: address });
  } catch (err) {
    console.error('DNS resolution error:', err);
  }
}

async function scanPorts(target: string, streamKey: string) {
  if (streamKey === REDIS_SETTINGS.STREAM_KEYS.TAILSCALE_OPEN_PORTS) {
    tsNmapScanning = true;
  }
  try {
    const { stdout } = await execAsync(`nmap -Pn -T4 -p 0-65535 ${target}`);
    const openPorts = stdout.match(/\d+\/tcp\s+open/g) || [];
    await addStreamEntry(streamKey, { ports: openPorts.join(';') });
    console.log(`Open TCP ports on ${target}: ${openPorts.length} found: ${openPorts.join(', ')}`);
  } catch (err) {
    console.error(`Port scan error for ${target}:`, err);
  }
  if (streamKey === REDIS_SETTINGS.STREAM_KEYS.TAILSCALE_OPEN_PORTS) {
    tsNmapScanning = false;
  }
}

const metricsQueue = new Queue('metrics', {
  redis: {
    host: '127.0.0.1',
    port: 6379
  },
  defaultJobOptions: {
    removeOnComplete: true,
    removeOnFail: true
  }
});

async function checkPing() {
  // if (tsNmapScanning) {
  //   console.log('Ping stats - nmap scanning for tailscale, later');
  //   return;
  // }
  try {
    const stdout = await new Promise<string>((resolve, reject) => {
      const req = http.get(`http://${TAILSCALE_IP}:3000/latest-ping`, { timeout: 4000 }, (res: any) => {
        if (res.statusCode !== 200) {
          res.resume();
          return reject(new Error(`Failed to get ping data, status code: ${res.statusCode}`));
        }
        let body = '';
        res.on('data', (chunk: any) => (body += chunk));
        res.on('end', () => resolve(body));
      });
      req.on('error', (err: any) => reject(new Error(`Failed to get ping data: ${err.message}`)));
      req.on('timeout', () => {
        req.destroy();
        reject(new Error('Request for ping data timed out'));
      });
    });
    const latencyMatch = stdout.match(/time=(\d+\.\d+)\s+ms/);

    if (!stdout || !latencyMatch) {
      throw new Error('Could not parse latency from ping data or response was empty');
    }

    const latency = latencyMatch[1];
    const packetsLost = '0'; // If we have latency from a single ping, assume 0 loss.

    console.log(`Ping stats - Latency: ${latency}ms, Packet loss: ${packetsLost}`);
    await addStreamEntry(REDIS_SETTINGS.STREAM_KEYS.PING_STATS, {
      latency: latency,
      loss: packetsLost
    });
  } catch (err) {
    console.error('Ping error:', (err as Error).message);
    await addStreamEntry(REDIS_SETTINGS.STREAM_KEYS.PING_STATS, {
      latency: '5000',
      loss: '1'
    });
  }
  await metricsQueue.add('generateMetrics', {});
}

async function handleRequest(req: http.IncomingMessage, res: http.ServerResponse) {
  if (req.url !== '/metrics') {
    res.writeHead(404);
    return res.end();
  }

  res.setHeader('Content-Type', 'application/json');

  try {
    await redisBlock.blmove('req-lock', 'req-lock', 'LEFT', 'RIGHT', 10);
    const metricsData = await redis.get('metrics:latest');
    await redis.expire('req-lock', 1, 'NX');
    if (!metricsData) {
      res.writeHead(404);
      return res.end(JSON.stringify({ error: 'Metrics not found' }));
    }
    res.writeHead(200);
    res.end(metricsData);
  } catch (err) {
    console.error('Metrics error:', err);
    res.writeHead(500);
    res.end(JSON.stringify({ error: 'Internal server error' }));
  }
}

const server = http.createServer(handleRequest);
server.listen(HTTP_PORT, () => {
  console.log(`HTTP server running on port ${HTTP_PORT}`);
});
const INTERVAL_TIMINGS = {
  DNS_CHECK: 60 * 60 * 1000,
  DOMAIN_PORT_SCAN: 60 * 60 * 1000,
  IP_PORT_SCAN: 60 * 60 * 1000,
  PING_CHECK: 5 * 1000
};

function scheduleAtInterval(fn: () => void, intervalMs: number) {
  fn();
  setInterval(fn, intervalMs);
}
scheduleAtInterval(checkPing, INTERVAL_TIMINGS.PING_CHECK);
scheduleAtInterval(checkDomainResolution, INTERVAL_TIMINGS.DNS_CHECK);
scheduleAtInterval(() => scanPorts(DOMAIN_NAME, REDIS_SETTINGS.STREAM_KEYS.INTERNET_OPEN_PORTS), INTERVAL_TIMINGS.DOMAIN_PORT_SCAN);
scheduleAtInterval(() => scanPorts(TAILSCALE_IP, REDIS_SETTINGS.STREAM_KEYS.TAILSCALE_OPEN_PORTS), INTERVAL_TIMINGS.IP_PORT_SCAN);
