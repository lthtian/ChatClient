// ABOUTME: Exercises request correlation and connection failure using real TCP sockets.
// ABOUTME: Verifies that split UTF-8 bytes and late replies preserve message boundaries.
const test = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const { once } = require('node:events');
const TcpClient = require('../src/tcp');

async function pair(t) {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const accepted = once(server, 'connection');
  const client = new TcpClient();
  client.on('error', () => {});
  await client.connect('127.0.0.1', server.address().port);
  const [socket] = await accepted;
  t.after(() => { client.close(); socket.destroy(); server.close(); });
  return { client, socket };
}

test('correlates concurrent media replies by request_id', async t => {
  const { client, socket } = await pair(t);
  const first = client.sendAndWait({ msgid: 26, request_id: 'first' }, 27);
  const second = client.sendAndWait({ msgid: 26, request_id: 'second' }, 27);
  socket.write(JSON.stringify({ msgid: 27, request_id: 'second', value: 2 }));
  socket.write(JSON.stringify({ msgid: 27, request_id: 'first', value: 1 }));
  assert.equal((await first).value, 1);
  assert.equal((await second).value, 2);
});

test('decodes a Chinese character split between network reads', async t => {
  const { client, socket } = await pair(t);
  const received = once(client, 'message');
  const bytes = Buffer.from(JSON.stringify({ text: '图片' }));
  const split = bytes.indexOf(Buffer.from('图')) + 1;
  socket.write(bytes.subarray(0, split));
  await new Promise(resolve => setTimeout(resolve, 30));
  socket.write(bytes.subarray(split));
  assert.deepEqual((await received)[0], { text: '图片' });
});

test('disconnect rejects pending requests without waiting for their deadline', async t => {
  const { client, socket } = await pair(t);
  const start = Date.now();
  const pending = client.sendAndWait({ msgid: 26, request_id: 'lost' }, 27, 1000);
  const rejected = assert.rejects(pending, /disconnect|连接/);
  socket.end();
  await rejected;
  assert.ok(Date.now() - start < 900);
});
