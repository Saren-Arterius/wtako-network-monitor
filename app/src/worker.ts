import { Redis } from 'ioredis';
import Queue from 'bull';
const metricsProcessor = require('../build/Release/metrics.node');

const DOMAIN_NAME = 'chibisuke.wtako.net';
const TAILSCALE_IP = '100.64.0.1';
const HTTP_PORT = 8080;
const REDIS_SETTINGS = Object.freeze({
  DATA_RETENTION_DAYS: 31,
  HISTORY_MINUTES: 60,
  STREAM_KEYS: Object.freeze({
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

const metricsQueue = new Queue('metrics1', {
  redis: {
    host: '127.0.0.1',
    port: 6379,
  },
});

metricsQueue.process('generateMetrics', 1, async (job) => {
  try {
    const metrics = await generateMetrics();
    const pipeline = redis.pipeline();
    pipeline.set('metrics:latest', JSON.stringify(metrics));
    pipeline.lpush('req-lock', 1);
    await pipeline.exec();

    job.progress(100);
  } catch (error) {
    console.error(`[${new Date().toISOString()}] Error generating metrics for job ${job.id}:`, error);

    throw error;
  } finally {

  }
});

metricsQueue.on('error', (err) => {
  console.error(`[${new Date().toISOString()}] Bull Queue Error:`, err);
});

metricsQueue.on('failed', (job, err) => {
  console.error(`[${new Date().toISOString()}] Job ${job.id} failed with error:`, err);
});

async function generateMetrics() {
  console.time('Total metrics generation');
  console.log(`[${new Date().toISOString()}] Starting metrics generation`);
  console.time('C++ processing');
  const workerInput = {
    redisUri: process.env.REDIS_URL || "redis://127.0.0.1:6379",
    streamKeys: REDIS_SETTINGS.STREAM_KEYS,
    historyMinutes: REDIS_SETTINGS.HISTORY_MINUTES
  };

  const metrics = JSON.parse(metricsProcessor.generate(JSON.stringify(workerInput)));

  console.timeEnd('C++ processing');
  console.timeEnd('Total metrics generation');
  return {
    ...metrics,
    last_updated: Date.now()
  };
}

