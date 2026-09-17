// ABOUTME: Exercises the actual image client, C++ helper and running MySQL-backed chat service.
// ABOUTME: Verifies publication, cancellation, account persistence and cached original downloads.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const TcpClient = require('../src/tcp');
const { ImageClient } = require('../src/image_client');
const port = Number(process.argv[2]);
const fixture = process.argv[3] ? require('node:fs').readFileSync(process.argv[3])
  : Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGPgUbLwA2EABYEBaWcDN6YAAAAASUVORK5CYII=', 'base64');
const toolPath = path.join(__dirname, '..', 'resources', 'native', 'chat_image.exe');

function until(client, id, terminal) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { client.off('task', listener); reject(new Error('task timeout')); }, 20000);
    const listener = value => {
      if (value.id !== id) return;
      if (value.state === 'failed') { clearTimeout(timer); client.off('task', listener); reject(new Error(value.error)); }
      else if (value.state === terminal) { clearTimeout(timer); client.off('task', listener); resolve(value); }
    };
    client.on('task', listener);
  });
}
test('sends and restores real image tasks against the running service', async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'chat-image-client-'));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  const tcp = new TcpClient(); tcp.on('error', () => {});
  await tcp.connect('127.0.0.1', port);
  t.after(() => tcp.close());
  const login = await tcp.sendAndWait({ msgid: 1, username: 'client_sender', password: 'test' }, 2);
  assert.equal(login.errno, 0);
  const rpc = async (op, fields) => {
    const response = await tcp.sendAndWait({ msgid: 26, request_id: crypto.randomUUID(), op, ...fields }, 27);
    if (!response.ok) throw new Error(response.error);
    return response.data;
  };
  const client = new ImageClient({ root, toolPath, rpc });
  const failures = []; client.on('storage-error', value => failures.push(value));
  await client.login(4); t.after(() => client.suspend());
  const source = path.join(root, '中文 图片.png'); await fs.writeFile(source, fixture);
  const target = { is_group: false, target: 2 };
  const states = []; client.on('task', job => states.push(job.state));
  const queued = await client.enqueue(source, target);
  const sent = await until(client, queued.id, 'sent');
  assert.ok(states.includes('preparing')); assert.ok(states.includes('uploading'));
  assert.ok(states.includes('processing')); assert.ok(states.includes('confirming'));
  const loaded = await client.load({ requestId: crypto.randomUUID(), mediaId: sent.message.media.media_id,
    variant: 'original', target });
  assert.deepEqual(await fs.readFile(loaded.file), fixture);
  const cached = await client.load({ requestId: crypto.randomUUID(), mediaId: sent.message.media.media_id,
    variant: 'original', target });
  assert.equal(cached.file, loaded.file);
  await fs.writeFile(loaded.file, Buffer.alloc(fixture.length));
  await client.load({ requestId: crypto.randomUUID(), mediaId: sent.message.media.media_id,
    variant: 'original', target });
  assert.deepEqual(await fs.readFile(loaded.file), fixture);
  const canceledId = crypto.randomUUID();
  const download = client.load({ requestId: canceledId, mediaId: sent.message.media.media_id,
    variant: 'original', target, force: true });
  client.cancelLoad(canceledId);
  await assert.rejects(download, { name: 'AbortError' });
  assert.equal((await fs.readdir(path.dirname(loaded.file))).filter(name => name.endsWith('.part')).length, 0);
  const records = JSON.parse(await fs.readFile(path.join(root, '4', 'tasks.json'), 'utf8'));
  assert.equal(records.length, 0);
  // A queued file remains available after the selected source is removed and the client is recreated.
  const paused = await client.enqueue(source, target);
  const stopped = until(client, paused.id, 'paused');
  client.suspend(); await stopped;
  await fs.rm(source);
  const restored = new ImageClient({ root, toolPath, rpc });
  restored.on('storage-error', value => failures.push(value));
  const jobs = await restored.login(4); t.after(() => restored.suspend());
  assert.equal(jobs[0].state, 'paused');
  const complete = until(restored, paused.id, 'sent');
  await restored.retry(paused.id); await complete;
  const extra = path.join(root, 'cancel.png'); await fs.writeFile(extra, fixture);
  const canceledJob = await restored.enqueue(extra, target);
  const canceled = until(restored, canceledJob.id, 'canceled');
  await restored.cancel(canceledJob.id); await canceled;
  assert.equal(restored.jobs.has(canceledJob.id), false);
  assert.deepEqual(failures, []);
});
