// ABOUTME: Verifies generic file copies, confirmed cache ownership and SQLite persistence.
// ABOUTME: Uses actual files and database rows without invoking image codecs or network substitutes.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const { MediaClient } = require('../src/media_client');

test('confirmed opaque files and empty files remain available offline after source removal', async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'chat-file-cache-'));
  const client = new MediaClient({ root, toolPath: path.join(root, 'no-image-codec.exe') });
  t.after(async () => { await client.close(); await fs.rm(root, { recursive: true, force: true }); });
  await client.login(4);
  client.online = false;
  const target = { is_group: false, target: 2 };
  for (const [index, bytes] of [Buffer.from([0, 1, 255, 20]), Buffer.alloc(0)].entries()) {
    const id = crypto.randomUUID(), mediaId = (index + 1).toString(16).repeat(32);
    const directory = client.folder(id); await fs.mkdir(directory);
    await fs.writeFile(path.join(directory, 'original'), bytes);
    const hash = crypto.createHash('sha256').update(bytes).digest('hex');
    const message = { message_id: String(index + 1), sequence: String(index + 1), client_msg_id: id,
      sender_id: 4, sender_name: 'sender', kind: 'file', time: Date.now(),
      media: { media_id: mediaId, name: '资料.bin', bytes: bytes.length, mime: 'application/octet-stream', sha256: hash } };
    const job = { id, kind: 'file', conversation: target, name: message.media.name,
      bytes: bytes.length, sha256: hash, message, createdAt: Date.now() };
    client.jobs.set(id, job); await client.complete(job);
    await assert.rejects(fs.stat(directory), { code: 'ENOENT' });
    const loaded = await client.load({ requestId: crypto.randomUUID(), mediaId, variant: 'original', target });
    assert.deepEqual(await fs.readFile(loaded.file), bytes);
    assert.equal(loaded.mime, 'application/octet-stream');
    const destination = path.join(root, `saved-${index}.bin`);
    await fs.writeFile(destination, 'existing destination');
    const saving = client.saveAs({ requestId: crypto.randomUUID(), mediaId, target, destination });
    assert.ok(client.operations.has(saving));
    await saving;
    assert.equal(client.operations.has(saving), false);
    await fs.rm(loaded.file);
    assert.deepEqual(await fs.readFile(destination), bytes);
  }
  assert.equal((await client.store.page(target)).messages.length, 2);
});
