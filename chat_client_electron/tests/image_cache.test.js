// ABOUTME: Exercises managed original promotion and offline cache reads with real files and SQLite.
// ABOUTME: Verifies damaged files, account boundaries and recovery after cache promotion failure.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const { MediaClient } = require('../src/media_client');
const { digest } = require('../src/media_client');
const target = { is_group: false, target: 2 };
const bytes = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGPgUbLwA2EABYEBaWcDN6YAAAAASUVORK5CYII=', 'base64');

async function fixture(t) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'chat-cache-'));
  const client = new MediaClient({ root });
  await client.login(1); client.suspend();
  t.after(async () => { await client.close(); await fs.rm(root, { recursive: true, force: true }); });
  const id = crypto.randomUUID(), mediaId = 'a'.repeat(32);
  const folder = client.folder(id);
  await fs.mkdir(path.join(folder, 'preview', 'objects'), { recursive: true });
  await fs.writeFile(path.join(folder, 'original'), bytes);
  await fs.writeFile(client.preview(id), bytes);
  const job = { id, conversation: target, name: 'test.png', state: 'confirming', bytes: bytes.length,
    sha256: await digest(path.join(folder, 'original')), thumbnail: { mime: 'image/png' },
    message: { message_id: '1', sequence: '1', sender_id: 1, sender_name: '用户', client_msg_id: id,
      kind: 'image', time: Date.now(), media: { media_id: mediaId, mime: 'image/png', bytes: bytes.length, width: 2, height: 1 } } };
  client.jobs.set(id, job); await client.save();
  return { root, client, job, options: { requestId: crypto.randomUUID(), mediaId, variant: 'original', target } };
}
test('confirmed original becomes offline cache without contacting server or selected path', async t => {
  const { client, job, options } = await fixture(t);
  await client.complete(job);
  assert.equal(client.jobs.size, 0);
  await assert.rejects(fs.stat(client.folder(job.id)), { code: 'ENOENT' });
  const loaded = await client.load(options);
  assert.deepEqual(await fs.readFile(loaded.file), bytes);
  const preview = await client.load({ ...options, requestId: crypto.randomUUID(), variant: 'thumbnail' });
  assert.deepEqual(await fs.readFile(preview.file), bytes);
  await fs.writeFile(loaded.file, Buffer.alloc(bytes.length));
  await assert.rejects(client.load({ ...options, requestId: crypto.randomUUID() }), /disconnected/);
  await assert.rejects(client.load({ ...options, requestId: crypto.randomUUID(), target: { is_group: false, target: 3 } }), /disconnected/);
});
test('failed promotion preserves confirmed job and original, recovery finishes without resending', async t => {
  const { root, client, job, options } = await fixture(t);
  const destination = path.join(client.directory, 'cache', options.mediaId + '-original');
  await fs.mkdir(destination);
  await assert.rejects(client.complete(job));
  assert.equal(client.jobs.get(job.id).message.message_id, '1');
  assert.deepEqual(await fs.readFile(path.join(client.folder(job.id), 'original')), bytes);
  await fs.rmdir(destination); await client.close();
  const restored = new MediaClient({ root });
  await restored.login(1); restored.suspend();
  assert.equal(restored.jobs.size, 0);
  assert.equal((await restored.store.page(target)).messages[0].message_id, '1');
  assert.deepEqual(await fs.readFile((await restored.load(options)).file), bytes);
  await restored.close();
});

test('confirmed task can finish when cache is committed and the task files are already removed', async t => {
  const { client, job, options } = await fixture(t);
  await client.complete(job);
  job.state = 'confirmed'; client.jobs.set(job.id, job); await client.save();
  await client.complete(job);
  assert.equal(client.jobs.size, 0);
  assert.deepEqual(await fs.readFile((await client.load(options)).file), bytes);
});
