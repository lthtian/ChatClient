// ABOUTME: Exercises durable messages, coverage and account isolation using real SQLite files.
// ABOUTME: Verifies that acknowledgments merge pending sends and incomplete batches roll back.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { MessageStore } = require('../src/message_store');
const { MessageSync } = require('../src/message_sync');
const target = { is_group: false, target: 2 };
const clientId = '00000000-0000-4000-8000-000000000001';
const message = (id, sender = 1) => ({ message_id: String(id), sequence: String(id),
  client_msg_id: id === 1 ? clientId : null, sender_id: sender, sender_name: '发送者',
  kind: 'text', text: '消息 ' + id, time: 1700000000000 + id });

async function fixture(t) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'chat-message-store-'));
  const store = await MessageStore.open(path.join(root, '1', 'chat.db'), 1);
  t.after(async () => { await store.close(); await fs.rm(root, { recursive: true, force: true }); });
  return { root, store };
}
test('merges pending, confirmation and history and persists after reopen', async t => {
  const { root, store } = await fixture(t);
  await store.pending(target, { client_msg_id: clientId, text: '消息 1', sender_name: '发送者' });
  assert.equal((await store.page(target)).messages[0].status, 'pending');
  await store.put(target, message(1)); await store.put(target, message(1));
  assert.equal((await store.page(target)).messages.length, 1);
  assert.equal((await store.page(target)).messages[0].status, 'sent');
  await store.close();
  const reopened = await MessageStore.open(path.join(root, '1', 'chat.db'), 1);
  assert.equal((await reopened.page(target)).messages[0].message_id, '1');
  await reopened.close();
});
test('live messages do not advance coverage; batches and cursor commit together', async t => {
  const { store } = await fixture(t);
  await store.put(target, message(100, 2));
  assert.equal(await store.coverage(target), null);
  await store.applyPage(target, { messages: [message(49), message(50)], lower: 49, upper: 50 }, 'initial');
  assert.deepEqual(await store.coverage(target), { lower: 49, upper: 50 });
  await assert.rejects(store.applyPage(target,
    { messages: [message(51), { ...message(52), kind: 'invalid' }], lower: 51, upper: 52 }, 'after'));
  assert.equal((await store.coverage(target)).upper, 50);
  assert.ok(!(await store.page(target)).messages.some(item => item.message_id === '51'));
  await store.applyPage(target, { messages: [message(51)], lower: 51, upper: 51 }, 'after');
  assert.equal((await store.coverage(target)).upper, 51);
});
test('accounts, conversations, resource indexes and pending recovery are isolated', async t => {
  const { root, store } = await fixture(t);
  await store.pending(target, { client_msg_id: clientId, text: '待发送', sender_name: '发送者' });
  await store.resource('a'.repeat(32), 'original', target,
    { name: 'a'.repeat(32) + '-original', mime: 'image/png', bytes: 12, sha256: 'b'.repeat(64) });
  assert.equal((await store.getResource('a'.repeat(32), 'original', target)).bytes, 12);
  assert.equal(await store.getResource('a'.repeat(32), 'original', { is_group: true, target: 2 }), null);
  const other = await MessageStore.open(path.join(root, '2', 'chat.db'), 2);
  assert.equal((await other.page(target)).messages.length, 0);
  await other.close();
  await store.recover();
  assert.equal((await store.page(target)).messages[0].status, 'paused');
});

test('notification ingestion is durable, deduplicated and leaves sync frontier unchanged', async t => {
  const { store } = await fixture(t);
  const notifications = [];
  const sync = new MessageSync({ store, changed: (conversation, value) => notifications.push(value.message_id) });
  await sync.receive(target, message(100, 2));
  await sync.receive(target, message(100, 2));
  assert.equal((await store.page(target)).messages.length, 1);
  assert.equal(await store.coverage(target), null);
  assert.deepEqual(notifications, ['100', '100']);
  await sync.close();
  await sync.receive(target, message(101, 2));
  assert.equal((await store.page(target)).messages.length, 1);
});

test('history pagination keeps pending messages on the latest page only', async t => {
  const { store } = await fixture(t);
  await store.applyPage(target, { messages: Array.from({ length: 60 }, (_, index) => message(index + 1)), lower: 1, upper: 60 }, 'initial');
  await store.pending(target, { client_msg_id: '00000000-0000-4000-8000-000000000002', text: '等待确认', sender_name: '发送者' });
  const latest = await store.page(target);
  assert.equal(latest.messages.length, 51);
  assert.equal(latest.next_cursor, '11');
  const earlier = await store.page(target, 11);
  assert.equal(earlier.messages.length, 10);
  assert.equal(earlier.next_cursor, '');
  await assert.rejects(store.applyPage(target, { messages: [message(62)], lower: 62, upper: 62 }, 'after'), /history_gap/);
  assert.equal((await store.coverage(target)).upper, 60);
});

test('a pending update cannot replace a committed message and a key cannot change conversation', async t => {
  const { store } = await fixture(t);
  await store.put(target, message(1));
  await store.pending(target, { client_msg_id: clientId, text: '暂存', sender_name: '发送者' });
  await store.status(clientId, 'failed', 'timeout');
  assert.equal((await store.page(target)).messages[0].status, 'sent');
  await assert.rejects(store.put({ is_group: true, target: 2 }, message(1)), /message_conflict/);
});
