// ABOUTME: Transfers generic files through the actual server and main-process media client.
// ABOUTME: Checks large and empty files, history, cancellation, cache integrity and saved copies.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const TcpClient = require('../src/tcp');
const { MediaClient, digest } = require('../src/media_client');

test('transfers large and empty files, restores canceled downloads and saves outside the cache', async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'chat-file-client-'));
  const tcp = new TcpClient(); tcp.on('error', () => {});
  const failures = [];
  const rpc = async (op, fields) => {
    const response = await tcp.sendAndWait({ msgid: 26, request_id: crypto.randomUUID(), op, ...fields }, 27);
    if (!response.ok) throw new Error(response.error);
    return response.data;
  };
  const client = new MediaClient({ root, rpc, toolPath: path.join(root, 'no-image-tool.exe') });
  client.on('storage-error', value => failures.push(value));
  t.after(async () => { tcp.close(); await client.close(); await fs.rm(root, { recursive: true, force: true }); });
  await tcp.connect('127.0.0.1', Number(process.argv[2]));
  assert.equal((await tcp.sendAndWait({ msgid: 1, username: 'client_sender', password: 'test' }, 2)).errno, 0);
  await client.login(4);
  const target = { is_group: false, target: 2 };
  function terminal(id, state) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { client.off('task', listener); reject(new Error('file task timeout')); }, 60000);
      function listener(job) {
        if (job.id !== id || ![state, 'failed'].includes(job.state)) return;
        clearTimeout(timer); client.off('task', listener);
        if (job.state === 'failed') reject(new Error(job.error)); else resolve(job);
      }
      client.on('task', listener);
    });
  }
  let large;
  for (const [name, bytes] of [['资料 21MB.bin', Buffer.alloc(21 * 1024 * 1024, 0xa5)], ['空文件.txt', Buffer.alloc(0)]]) {
    const selected = path.join(root, name); await fs.writeFile(selected, bytes);
    const hash = await digest(selected);
    const queued = await client.enqueue(selected, target, 'file');
    const sent = await terminal(queued.id, 'sent');
    await fs.rm(selected);
    assert.equal(sent.message.kind, 'file'); assert.equal(sent.message.media.name, name);
    assert.equal(sent.message.media.sha256, hash);
    const mediaId = sent.message.media.media_id;
    const loaded = await client.load({ requestId: crypto.randomUUID(), mediaId, variant: 'original', target });
    assert.equal(await digest(loaded.file), hash);
    await fs.rm(loaded.file);
    const destination = path.join(root, 'saved-' + name);
    await client.saveAs({ requestId: crypto.randomUUID(), mediaId, target, destination });
    assert.equal(await digest(destination), hash);
    if (bytes.length) large = { mediaId, hash, loaded, destination };
  }
  const history = await rpc('sync', { conversation: target, direction: 'initial' });
  assert.equal(history.messages.filter(message => message.kind === 'file').length, 2);
  const cancelSource = path.join(root, 'cancel.bin');
  await fs.writeFile(cancelSource, Buffer.alloc(1024 * 1024));
  const stopUpload = job => {
    if (job.name === 'cancel.bin' && job.state === 'uploading') {
      client.cancel(job.id).catch(error => failures.push(error.message));
    }
  };
  client.on('task', stopUpload);
  const canceledJob = await client.enqueue(cancelSource, target, 'file');
  await terminal(canceledJob.id, 'canceled');
  client.off('task', stopUpload);
  assert.equal(client.jobs.has(canceledJob.id), false);
  const canceledResource = await rpc('begin_file', { conversation: target, client_msg_id: canceledJob.id,
    bytes: 1024 * 1024, sha256: await digest(cancelSource), name: 'cancel.bin' });
  assert.equal(canceledResource.state, 'canceled');
  const id = crypto.randomUUID();
  const canceled = client.load({ requestId: id, mediaId: large.mediaId, variant: 'original', target, force: true });
  client.cancelLoad(id);
  await assert.rejects(canceled, { name: 'AbortError' });
  await fs.writeFile(large.loaded.file, 'corrupted cache');
  await client.load({ requestId: crypto.randomUUID(), mediaId: large.mediaId, variant: 'original', target });
  assert.equal(await digest(large.loaded.file), large.hash);
  client.suspend(); tcp.close();
  const offline = await client.load({ requestId: crypto.randomUUID(), mediaId: large.mediaId, variant: 'original', target });
  assert.equal(await digest(offline.file), large.hash);
  await fs.rm(offline.file);
  assert.equal(await digest(large.destination), large.hash);
  assert.deepEqual(failures, []);
});
