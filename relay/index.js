'use strict';

const crypto = require('node:crypto');

const MAX_IMAGE_BYTES = 350 * 1024;
const MODEL_URL = 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions';
const MODEL_ID = 'qwen3.8-flash';

function respond(statusCode, value) {
  return { statusCode, headers: { 'content-type': 'application/json; charset=utf-8',
    'cache-control': 'no-store' }, body: JSON.stringify(value), isBase64Encoded: false };
}

function equalSecret(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || !a || !b) return false;
  const left = Buffer.from(a);
  const right = Buffer.from(b);
  return left.length === right.length && crypto.timingSafeEqual(left, right);
}

function headersLower(headers = {}) {
  return Object.fromEntries(Object.entries(headers).map(([key, value]) => [key.toLowerCase(), value]));
}

function parseResult(content) {
  if (typeof content !== 'string') throw new Error('invalid model response');
  const cleaned = content.trim().replace(/^```(?:json)?\s*/i, '').replace(/\s*```$/, '');
  const value = JSON.parse(cleaned);
  if (!value || typeof value.type !== 'string' || !value.type.trim() ||
      !Array.isArray(value.evidence) || !['high', 'medium', 'low'].includes(value.confidence_description)) {
    throw new Error('invalid model response');
  }
  return {
    type: value.type.trim().slice(0, 40),
    confidence_description: value.confidence_description,
    evidence: value.evidence.filter(item => typeof item === 'string' && item.trim())
      .slice(0, 3).map(item => item.trim().slice(0, 80))
  };
}

async function handle(event, env = process.env, fetchImpl = globalThis.fetch) {
  try {
    if (Buffer.isBuffer(event)) event = event.toString('utf8');
    if (typeof event === 'string') event = JSON.parse(event);
    const method = event?.requestContext?.http?.method;
    const path = event?.requestContext?.http?.path || event?.rawPath;
    if (method === 'GET' && path === '/health') return respond(200, { ok: true });
    if (method !== 'POST' || path !== '/recognize') return respond(404, { error: 'not_found' });
    if (!env.DeviceToken || !env.BailianApiKey) return respond(503, { error: 'not_configured' });
    const headers = headersLower(event.headers);
    if (!equalSecret(headers['x-device-token'], env.DeviceToken)) return respond(401, { error: 'unauthorized' });
    if (String(headers['content-type'] || '').split(';')[0].trim().toLowerCase() !== 'image/jpeg')
      return respond(415, { error: 'jpeg_required' });
    if (!event.isBase64Encoded || typeof event.body !== 'string' ||
        event.body.length > Math.ceil(MAX_IMAGE_BYTES / 3) * 4 + 4)
      return respond(413, { error: 'invalid_image' });
    const image = Buffer.from(event.body, 'base64');
    if (image.length < 4 || image.length > MAX_IMAGE_BYTES || image[0] !== 0xff || image[1] !== 0xd8)
      return respond(400, { error: 'invalid_image' });

    const payload = {
      model: MODEL_ID, enable_thinking: false, max_tokens: 512,
      messages: [{ role: 'user', content: [
        { type: 'image_url', image_url: { url: `data:image/jpeg;base64,${image.toString('base64')}` } },
        { type: 'text', text: 'Identify the dominant cloud genus in this image. Return JSON only: type (Simplified Chinese name or unknown), confidence_description (high, medium, or low; subjective and not calibrated), evidence (up to three brief visual reasons in Simplified Chinese). If sky or cloud is unclear use unknown. Do not invent location, weather, or time.' }
      ] }]
    };
    const upstream = await fetchImpl(MODEL_URL, {
      method: 'POST', headers: { authorization: `Bearer ${env.BailianApiKey}`, 'content-type': 'application/json' },
      body: JSON.stringify(payload), signal: AbortSignal.timeout(45000)
    });
    if (!upstream.ok) return respond(502, { error: 'model_unavailable', upstream_status: upstream.status });
    const answer = await upstream.json();
    const result = parseResult(answer?.choices?.[0]?.message?.content);
    return respond(200, { ...result, location: null, weather: null, model: MODEL_ID });
  } catch (_) {
    return respond(502, { error: 'recognition_failed' });
  }
}

exports.handler = async function(event) { return handle(event); };
exports.handle = handle;
exports.parseResult = parseResult;
