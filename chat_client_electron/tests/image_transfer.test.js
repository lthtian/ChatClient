// ABOUTME: Verifies image streaming against a real loopback HTTP file endpoint.
// ABOUTME: Checks exact bytes, truncated downloads, cancellation and transport policy.
const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { once } = require('node:events');
const { transferFile, endpoint } = require('../src/media_transfer');

test('rejects cleartext public endpoints and embedded credentials', () => {
  for (const url of ['http://example.com/media', 'https://user:pass@example.com/media']) {
    assert.throws(() => endpoint({ url, method: 'GET' }), /insecure_endpoint/);
  }
});
test('streams actual file bytes and removes incomplete downloads', async t => {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'chat-transfer-'));
  t.after(() => fs.rm(directory, { recursive: true, force: true }));
  const source = Buffer.alloc(1024 * 256, 47);
  const server = http.createServer(async (request, response) => {
    if (request.method === 'PUT') {
      const chunks = [];
      for await (const chunk of request) chunks.push(chunk);
      await fs.writeFile(path.join(directory, 'uploaded'), Buffer.concat(chunks));
      response.end('{"ok":true}');
    } else {
      response.writeHead(200, { 'Content-Length': source.length });
      if (request.url === '/short') { response.end(source.subarray(0, 100)); }
      else response.end(source);
    }
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  t.after(() => { server.closeAllConnections(); server.close(); });
  const url = `http://127.0.0.1:${server.address().port}`;
  const input = path.join(directory, 'source');
  await fs.writeFile(input, source);
  const progress = [];
  await transferFile({ descriptor: { url, method: 'PUT' }, file: input, bytes: source.length,
    progress: value => progress.push(value.loaded) });
  assert.deepEqual(await fs.readFile(path.join(directory, 'uploaded')), source);
  assert.equal(progress.at(-1), source.length);
  const output = path.join(directory, 'download');
  await transferFile({ descriptor: { url, method: 'GET' }, file: output, bytes: source.length });
  assert.deepEqual(await fs.readFile(output), source);
  const short = path.join(directory, 'short');
  await assert.rejects(transferFile({ descriptor: { url: url + '/short', method: 'GET' },
    file: short, bytes: source.length }));
  await assert.rejects(fs.stat(short), { code: 'ENOENT' });
  const controller = new AbortController(); controller.abort();
  await assert.rejects(transferFile({ descriptor: { url, method: 'GET' }, file: short,
    bytes: source.length, signal: controller.signal }), { name: 'AbortError' });
});
