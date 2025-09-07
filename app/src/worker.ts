import { Redis } from 'ioredis';
import Queue from 'bull';
const metricsProcessor = require('../build/Release/metrics.node');
// Constants for configuration and Redis keys - moved outside to prevent re-declaration on every import/call
const DOMAIN_NAME = process.env.DOMAIN_NAME || 'your.domain.tld';
const TAILSCALE_IP = process.env.TAILSCALE_IP || '100.64.0.1';
const HTTP_PORT = 8080; // Not used in the provided snippet, but good to keep if it's a global constant
const REDIS_SETTINGS = Object.freeze({ // Use Object.freeze for immutable constants
  DATA_RETENTION_DAYS: 31,
  HISTORY_MINUTES: 60,
  STREAM_KEYS: Object.freeze({ // Freeze nested objects too
    RESOLVED_IPS: 'domain:resolved_ips',
    INTERNET_OPEN_PORTS: `network:open_ports:${DOMAIN_NAME}`,
    TAILSCALE_OPEN_PORTS: `network:open_ports:${TAILSCALE_IP}`,
    PING_STATS: 'network:ping_stats',
    NETWORK_TRAFFIC_STATS: 'network:traffic_stats'
  })
});

const redis = new Redis({
  host: '127.0.0.1',
});
// Initialize Bull Queue once
const metricsQueue = new Queue('metrics', {
  redis: {
    host: '127.0.0.1',
    port: 6379,
  },
});
// Process function for the 'generateMetrics' job
// Use async/await for better readability and error handling
metricsQueue.process('generateMetrics', 1, async (job) => { // '1' indicates concurrency of 1, good for Pi2
  try {
    // Connect Redis if lazyConnect is true
    // await redis.connect(); // Only if lazyConnect is true and not handled by ioredis itself
    const metrics = await generateMetrics();
    // Use pipeline for multiple Redis commands in quick succession to reduce round trips
    const pipeline = redis.pipeline();
    pipeline.set('metrics:latest', JSON.stringify(metrics));
    pipeline.lpush('req-lock', 1);
    await pipeline.exec(); // Execute all commands in the pipeline
    // console.log(`[${new Date().toISOString()}] Metrics generated and cached successfully for job ${job.id}`);
    job.progress(100); // Report job completion
  } catch (error) {
    console.error(`[${new Date().toISOString()}] Error generating metrics for job ${job.id}:`, error);
    // Throw error to Bull so it can handle retries or mark as failed
    throw error;
  } finally {
    // No need to close redis connection if it's a global client managed by ioredis's pooling
  }
});
// // Add an event listener for queue errors
metricsQueue.on('error', (err) => {
  console.error(`[${new Date().toISOString()}] Bull Queue Error:`, err);
});
// // Add listeners for job events for better logging and debugging
// metricsQueue.on('active', (job) => {
//   console.log(`[${new Date().toISOString()}] Job ${job.id} is active, processing...`);
// });
// metricsQueue.on('completed', (job, result) => {
//   console.log(`[${new Date().toISOString()}] Job ${job.id} completed! Result:`, result);
// });
metricsQueue.on('failed', (job, err) => {
  console.error(`[${new Date().toISOString()}] Job ${job.id} failed with error:`, err);
});
async function generateMetrics() {
  console.time('Total metrics generation');
  console.log(`[${new Date().toISOString()}] Starting metrics generation`);
  const now = Date.now();
  console.time('Redis data retrieval');
  const thirtyDaysAgoMs = now - 30 * 86400 * 1000;
  const sixtyMinutesAgoMs = now - REDIS_SETTINGS.HISTORY_MINUTES * 60 * 1000;
  const [
    ipHistoryEntries,
    domainPortsEntry,
    ipPortsEntry,
    latestPingEntry,
    pingEntries,
    trafficEntries
  ] = await Promise.all([
    redis.xrange(REDIS_SETTINGS.STREAM_KEYS.RESOLVED_IPS, '-', '+'),
    redis.xrevrange(REDIS_SETTINGS.STREAM_KEYS.INTERNET_OPEN_PORTS, '+', '-', 'COUNT', 1),
    redis.xrevrange(REDIS_SETTINGS.STREAM_KEYS.TAILSCALE_OPEN_PORTS, '+', '-', 'COUNT', 1),
    redis.xrevrange(REDIS_SETTINGS.STREAM_KEYS.PING_STATS, '+', '-', 'COUNT', 1),
    redis.xrange(REDIS_SETTINGS.STREAM_KEYS.PING_STATS, sixtyMinutesAgoMs.toString(), '+'),
    redis.xrange(REDIS_SETTINGS.STREAM_KEYS.NETWORK_TRAFFIC_STATS, thirtyDaysAgoMs.toString(), '+'),
  ]);
  console.timeEnd('Redis data retrieval');
  console.time('C++ processing');
  const rawData = {
    now: now,
    ipHistoryEntries,
    domainPortsEntry,
    ipPortsEntry,
    latestPingEntry,
    pingEntries,
    trafficEntries,
    historyMinutes: REDIS_SETTINGS.HISTORY_MINUTES
  };
  // The C++ addon will return a JSON string
  const metrics = JSON.parse(metricsProcessor.generate(JSON.stringify(rawData)));
  console.timeEnd('C++ processing');
  console.timeEnd('Total metrics generation');
  return {
    ...metrics,
    last_updated: Date.now()
  };
}
