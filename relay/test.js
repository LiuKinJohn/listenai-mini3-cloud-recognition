'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { handle } = require('./index');

const env = { DeviceToken: 'test-device-token', BailianApiKey: 'test-api-key' };
const jpeg = Buffer.from([0xff, 0xd8, 0xff, 0xd9]);
const request = { requestContext: { http: { method: 'POST', path: '/recognize' } },
  headers: { 'Content-Type': 'image/jpeg', 'X-Device-Token': 'test-device-token' },
  isBase64Encoded: true, body: jpeg.toString('base64') };

test('rejects unauthorized requests without invoking the model', async () => {
  let called = false;
  const result = await handle({ ...request, headers: { ...request.headers, 'X-Device-Token': 'wrong' } }, env,
    async () => { called = true; });
  assert.equal(result.statusCode, 401);
  assert.equal(called, false);
});

test('forwards JPEG and returns validated subjective confidence', async () => {
  const result = await handle(request, env, async (url, options) => {
    assert.equal(url, 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions');
    assert.equal(JSON.parse(options.body).model, 'qwen3.8-flash');
    return { ok: true, json: async () => ({ choices: [{ message: { content:
      '{"type":"积云","confidence_description":"medium","evidence":["云体成团"]}' } }] }) };
  });
  assert.equal(result.statusCode, 200);
  assert.deepEqual(JSON.parse(result.body).confidence_description, 'medium');
  assert.equal(JSON.parse(result.body).location, null);
});

test('rejects invalid images and fails closed when secrets are absent', async () => {
  const bad = await handle({ ...request, body: Buffer.from('bad').toString('base64') }, env);
  assert.equal(bad.statusCode, 400);
  const missing = await handle(request, {});
  assert.equal(missing.statusCode, 503);
});

test('does not return upstream errors or credentials', async () => {
  const result = await handle(request, env, async () => ({ ok: false, status: 403 }));
  assert.equal(result.statusCode, 502);
  assert.equal(result.body.includes('test-api-key'), false);
});
