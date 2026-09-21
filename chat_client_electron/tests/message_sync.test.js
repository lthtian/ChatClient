// ABOUTME: Verifies durable text sending and history reconciliation against a real chat server.
// ABOUTME: Uses isolated accounts, real TCP connections and SQLite without replacing network behavior.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const TcpClient = require('../src/tcp');
const { MessageStore } = require('../src/message_store');
const { MessageSync } = require('../src/message_sync');

test('persists acknowledgments, fills history gaps and retries durable text', async t => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'chat-message-sync-'));
  const peers = [], errors = [];
  t.after(async () => {
    for (const peer of peers) { peer.tcp.close(); await peer.sync.close(); await peer.store.close(); }
    await fs.rm(root, { recursive: true, force: true });
  });
  async function wait(check) {
    const deadline = Date.now() + 15000;
    while (Date.now() < deadline) {
      if (await check()) return;
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    throw new Error('Message reconciliation timed out');
  }
  async function connect(peer) {
    await peer.tcp.connect('127.0.0.1', Number(process.argv[2]));
    await wait(async () => {
      const login = await peer.tcp.sendAndWait({ msgid: 1, username: peer.name, password: 'test' }, 2);
      if (login.errno === 0) return true;
      assert.equal(login.errno, 1, JSON.stringify(login));
      assert.equal(login.errmsg, '该用户已经登录, 不能重复登录');
      return false;
    });
  }
  async function create(id, name) {
    const tcp = new TcpClient(); tcp.on('error', error => errors.push(error.message));
    const store = await MessageStore.open(path.join(root, String(id), 'chat.db'), id);
    const rpc = async (op, fields) => {
      const response = await tcp.sendAndWait({ msgid: 26, request_id: crypto.randomUUID(), op, ...fields }, 27);
      if (!response.ok) throw new Error(response.error);
      return response.data;
    };
    const sync = new MessageSync({ store, rpc, changed() {}, failed: error => errors.push(error.message) });
    const peer = { tcp, store, sync, name }; peers.push(peer);
    tcp.on('message', value => {
      if (value.msgid === 28) sync.receive(value.conversation, value.message).catch(error => errors.push(error.message));
    });
    await connect(peer); return peer;
  }
  const sender = await create(4, 'client_sender'), receiver = await create(5, 'ack_sender');
  const destination = { is_group: false, target: 5 }, source = { is_group: false, target: 4 };
  await receiver.sync.sync({ conversation: source });
  assert.deepEqual(await receiver.store.coverage(source), { lower: 1, upper: 0 });
  async function send(text) {
    const queued = await sender.sync.send(destination, text, sender.name);
    await sender.sync.retry(queued.client_msg_id);
    const page = await sender.store.page(destination, 0, 100);
    const message = page.messages.find(value => value.client_msg_id === queued.client_msg_id);
    assert.equal(message.status, 'sent'); assert.ok(message.message_id);
    return message;
  }
  const first = await send('实时入库验证');
  await wait(async () => (await receiver.store.page(source)).messages.some(value => value.message_id === first.message_id));
  assert.equal((await receiver.store.coverage(source)).upper, 0);
  await receiver.sync.sync({ conversation: source });
  receiver.tcp.close(); await wait(() => !receiver.tcp.socket);
  for (let number = 0; number < 55; number++) await send('离线补齐 ' + number);
  await connect(receiver);
  const last = await send('重连后的实时消息');
  await wait(async () => (await receiver.store.page(source)).messages.some(value => value.message_id === last.message_id));
  assert.equal((await receiver.store.coverage(source)).upper, 1);
  const latest = await receiver.sync.sync({ conversation: source });
  assert.equal(latest.messages.length, 50);
  const earlier = await receiver.sync.sync({ conversation: source, before_sequence: latest.next_cursor });
  assert.equal(earlier.messages.length, 7);
  const complete = await receiver.store.page(source, 0, 100);
  assert.equal(complete.messages.length, 57);
  assert.deepEqual(complete.messages.map(value => Number(value.sequence)), Array.from({ length: 57 }, (_, index) => index + 1));
  await receiver.sync.sync({ conversation: source });
  assert.equal((await receiver.store.page(source, 0, 100)).messages.length, 57);

  sender.tcp.close(); await wait(() => !sender.tcp.socket);
  const pending = await sender.sync.send(destination, '断线重试验证', sender.name);
  await sender.sync.retry(pending.client_msg_id);
  assert.equal((await sender.store.pendingMessage(pending.client_msg_id)).message.status, 'failed');
  await connect(sender);
  await sender.sync.retry(pending.client_msg_id);
  const committed = (await sender.store.page(destination, 0, 100)).messages.filter(value => value.client_msg_id === pending.client_msg_id);
  assert.equal(committed.length, 1); assert.equal(committed[0].status, 'sent');
  await receiver.sync.sync({ conversation: source });
  assert.equal((await receiver.store.page(source, 0, 100)).messages.length, 58);
  await receiver.sync.close(); await receiver.store.close();
  receiver.store = await MessageStore.open(path.join(root, '5', 'chat.db'), 5);
  assert.equal((await receiver.store.page(source, 0, 100)).messages.length, 58);
  assert.deepEqual(errors, []);
});
