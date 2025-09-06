import { Redis } from 'ioredis';
import Queue from 'bull';

const DOMAIN_NAME = process.env.DOMAIN_NAME || 'your.domain.tld';
const TAILSCALE_IP = process.env.TAILSCALE_IP || '100.64.0.1';
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

const metricsQueue = new Queue('metrics', {
  redis: {
    host: '127.0.0.1',
    port: 6379
  }
});

metricsQueue.process('generateMetrics', async () => {
  try {
    const metrics = await generateMetrics();
    await redis.set('metrics:latest', JSON.stringify(metrics));
    await redis.lpush('req-lock', 1);
    console.log('Metrics generated and cached successfully');
  } catch (error) {
    console.error('Error generating metrics:', error);
  }
});

async function generateMetrics() {
  console.time('Total metrics generation');
  console.log(`[${new Date().toISOString()}] Starting metrics generation`);

  console.time('1. IP history retrieval');
  const ipHistoryEntries = await redis.xrange(REDIS_SETTINGS.STREAM_KEYS.RESOLVED_IPS, '-', '+');
  console.timeEnd('1. IP history retrieval');

  console.time('2. Port data retrieval');
  const [domainPortsEntry, ipPortsEntry] = await Promise.all([
    redis.xrevrange(REDIS_SETTINGS.STREAM_KEYS.INTERNET_OPEN_PORTS, '+', '-', 'COUNT', 1),
    redis.xrevrange(REDIS_SETTINGS.STREAM_KEYS.TAILSCALE_OPEN_PORTS, '+', '-', 'COUNT', 1)
  ]);
  console.timeEnd('2. Port data retrieval');

  console.time('3. Ping data processing');
  const latestPingEntry = await redis.xrevrange(REDIS_SETTINGS.STREAM_KEYS.PING_STATS, '+', '-', 'COUNT', 1);
  const now = Date.now();
  const pingEntries = await redis.xrange(
    REDIS_SETTINGS.STREAM_KEYS.PING_STATS,
    (now - 86400000).toString(),
    '+'
  );
  console.log('entries length: ', pingEntries.length);
  console.timeEnd('3. Ping data processing');

  console.time('4. Data formatting');
  const formattedIpHistory = ipHistoryEntries.map(entry => ({
    ip: entry[1][1],
    timestamp: new Date(parseInt(entry[0].split('-')[0])).toISOString()
  })).slice(-10).reverse();

  const currentDomainPorts = domainPortsEntry.length > 0 ? domainPortsEntry[0][1][1].split(';').filter(s => !!s) : [];
  const currentIpPorts = ipPortsEntry.length > 0 ? ipPortsEntry[0][1][1].split(';').filter(s => !!s) : [];

  const latestLatency = latestPingEntry.length > 0
    ? parseFloat(latestPingEntry[0][1][1])
    : null;

  const latestLossRaw = latestPingEntry.length > 0
    ? parseInt(latestPingEntry[0][1][3], 10)
    : 1;
  const latestLossPercent = latestLossRaw * 100;
  const roundedMinute = (now - (now % 60000)) + 60000;

  const outages: { start: string; end: string; duration_seconds: number }[] = [];
  let outageStart: number | null = null;
  let consecutiveMisses = 0;
  let seenTimestamps = [];
  const OUTAGE_THRESHOLD = 3;
  for (const entry of pingEntries) {
    const timestamp = parseInt(entry[0].split('-')[0], 10);
    seenTimestamps.push(timestamp);
    if (seenTimestamps.length > OUTAGE_THRESHOLD) {
      seenTimestamps.unshift();
    }
    const loss = parseInt(entry[1][3], 10);

    if (loss === 1) {
      consecutiveMisses++;
      if (consecutiveMisses >= OUTAGE_THRESHOLD && outageStart === null) {
        outageStart = seenTimestamps[0];
      }
    } else {
      if (outageStart !== null) {
        outages.push({
          start: new Date(outageStart).toISOString(),
          end: new Date(timestamp).toISOString(),
          duration_seconds: Math.round((timestamp - outageStart) / 1000)
        });
        outageStart = null;
      }
      consecutiveMisses = 0;
      seenTimestamps = [];
    }
  }

  if (outageStart !== null) {
    outages.push({
      start: new Date(outageStart).toISOString(),
      end: new Date().toISOString(),
      duration_seconds: Math.round((Date.now() - outageStart) / 1000)
    });
  }

  console.time('5. Historical calculations');

  const pingEntriesReversed = pingEntries.reverse();

  console.time('5.1 generateMinuteHistory');
  const minuteHistoryData = generateMinuteHistory(roundedMinute, pingEntriesReversed);
  console.timeEnd('5.1 generateMinuteHistory');
  console.time('5.2 calculateAllHistoricalAverages');
  const historicalAverages = calculateAllHistoricalAverages(now, pingEntriesReversed);
  console.timeEnd('5.2 calculateAllHistoricalAverages');

  console.timeEnd('5. Historical calculations');

  const pingStats = {
    latency: {
      latest: latestLatency !== null ? parseFloat(latestLatency.toFixed(2)) : null,
      last1m: historicalAverages.latency.last1m,
      last5m: historicalAverages.latency.last5m,
      last1h: historicalAverages.latency.last1h,
      last24h: historicalAverages.latency.last24h
    },
    packet_loss: {
      latest_percent: parseFloat(latestLossPercent.toFixed(2)),
      last1m_percent: historicalAverages.loss.last1m,
      last5m_percent: historicalAverages.loss.last5m,
      last1h_percent: historicalAverages.loss.last1h,
      last24h_percent: historicalAverages.loss.last24h
    },
    outages: outages,
    minute_history: minuteHistoryData
  };

  console.timeEnd('4. Data formatting');
  console.timeEnd('Total metrics generation');

  return {
    ip_history: formattedIpHistory,
    internet_ports: currentDomainPorts,
    tailscale_ports: currentIpPorts,
    ping_statistics: pingStats,
    last_updated: Date.now()
  };
}

function generateMinuteHistory(currentTime: number, pingEntriesReversed: any[]) {
  const minuteData = [];
  const entryMap = new Map<number, any[]>();

  const minTimestamp = currentTime - REDIS_SETTINGS.HISTORY_MINUTES * 60000;

  // Populate entry map with reverse chronological order entries
  for (const entry of pingEntriesReversed) {
    const timestamp = parseInt(entry[0].split('-')[0], 10);
    if (timestamp < minTimestamp) break;
    const minuteKey = Math.floor(timestamp / 60000) * 60000;

    if (!entryMap.has(minuteKey)) {
      entryMap.set(minuteKey, []);
    }
    entryMap.get(minuteKey)!.unshift(entry); // Maintain chronological order within bucket
  }
  // console.log('entryMap', entryMap);

  // Generate data for each historical minute window
  for (let i = 0; i < REDIS_SETTINGS.HISTORY_MINUTES; i++) {
    const windowStart = currentTime - (REDIS_SETTINGS.HISTORY_MINUTES - i) * 60000;
    const minuteKey = Math.floor(windowStart / 60000) * 60000;
    const entries = entryMap.get(minuteKey) || [];
    // console.log({minuteKey}, entries.length);

    let latencySum = 0;
    let validLatencyCount = 0;
    let lossCount = 0;

    entries.forEach(entry => {
      const latency = parseFloat(entry[1][1]);
      const loss = parseInt(entry[1][3], 10);

      if (!isNaN(latency)) {
        latencySum += latency;
        validLatencyCount++;
      }
      lossCount += loss;
    });
    const avgLatency = validLatencyCount > 0
      ? Number((latencySum / validLatencyCount).toFixed(2))
      : null;

    const totalEntries = entries.length;
    const lossPercent = totalEntries > 0
      ? Number(((lossCount / totalEntries) * 100).toFixed(2))
      : 0;

    minuteData.push({
      timestamp: new Date(windowStart).toISOString(),
      latency_ms: avgLatency,
      packet_loss_percent: lossPercent
    });
  }
  // console.log({minuteData})

  return minuteData;
}

function calculateAllHistoricalAverages(currentTime: number, pingEntriesReversed: any[]) {
  const windowConfigs = [
    { key: 'last1m', ms: 60000 },
    { key: 'last5m', ms: 300000 },
    { key: 'last1h', ms: 3600000 },
    { key: 'last24h', ms: 86400000 }
  ];

  const latencyResults: Record<string, number | null> = {};
  const lossResults: Record<string, number> = {};

  let previousIndex = 0;
  let previousLatencySum = 0;
  let previousValidCount = 0;
  let previousLossCount = 0;
  let previousTotalEntries = 0;

  for (const config of windowConfigs) {
    const minTimestamp = currentTime - config.ms;
    let latencySum = previousLatencySum;
    let validCount = previousValidCount;
    let lossCount = previousLossCount;
    let totalEntries = previousTotalEntries;

    for (let i = previousIndex; i < pingEntriesReversed.length; i++) {
      const entry = pingEntriesReversed[i];
      const timestamp = parseInt(entry[0].split('-')[0], 10);
      if (timestamp < minTimestamp) {
        previousIndex = i;
        break;
      }
      const latency = parseFloat(entry[1][1]);
      const loss = parseInt(entry[1][3], 10);
      if (!isNaN(latency)) {
        latencySum += latency;
        validCount++;
      }
      lossCount += loss;
      totalEntries++;
    }

    latencyResults[config.key] = validCount > 0
      ? Number((latencySum / validCount).toFixed(2))
      : null;
    lossResults[config.key] = totalEntries > 0
      ? Number(((lossCount / totalEntries) * 100).toFixed(2))
      : 0;

    previousLatencySum = latencySum;
    previousValidCount = validCount;
    previousLossCount = lossCount;
    previousTotalEntries = totalEntries;
  }

  return {
    latency: latencyResults as { [key: string]: number | null },
    loss: lossResults as { [key: string]: number }
  };
}
