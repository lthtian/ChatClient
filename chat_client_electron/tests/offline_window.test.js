// ABOUTME: Opens the actual client with a persisted account fixture and a real authentication server.
// ABOUTME: Verifies local text and image browsing after that isolated server disconnects.
const { app, BrowserWindow } = require('electron');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const { MessageStore } = require('../src/message_store');
require('../src/desktop_bridge');
const output = process.argv[3];
const port = process.argv[2];
const bytes = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGPgUbLwA2EABYEBaWcDN6YAAAAASUVORK5CYII=', 'base64');

async function start() {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'chat-offline-window-'));
  app.setPath('userData', root);
  process.env.CHAT_HOST = '127.0.0.1'; process.env.CHAT_PORT = port;
  const key = crypto.createHash('sha256').update('127.0.0.1:' + port).digest('hex').slice(0, 16);
  const directory = path.join(root, 'images', key, '4');
  const store = await MessageStore.open(path.join(directory, 'chat.db'), 4);
  const target = { is_group: false, target: 2 }, mediaId = 'a'.repeat(32);
  await fs.mkdir(path.join(directory, 'cache'), { recursive: true });
  await store.put(target, { message_id: '8001', sequence: '1', sender_id: 4, sender_name: 'client_sender',
    kind: 'text', text: '离线消息持久化验证', time: Date.now() });
  await store.put(target, { message_id: '8002', sequence: '2', sender_id: 4, sender_name: 'client_sender',
    kind: 'image', time: Date.now(), media: { media_id: mediaId, mime: 'image/png', bytes: bytes.length, width: 2, height: 1 } });
  for (const variant of ['original', 'thumbnail']) {
    const name = mediaId + '-' + variant;
    await fs.writeFile(path.join(directory, 'cache', name), bytes);
    await store.resource(mediaId, variant, target,
      { name, mime: 'image/png', bytes: bytes.length, sha256: crypto.createHash('sha256').update(bytes).digest('hex') });
  }
  await store.close();
  require('../main');
  await app.whenReady();
  const win = BrowserWindow.getAllWindows()[0];
  const evaluate = code => win.webContents.executeJavaScript(code);
  const errors = [];
  win.webContents.on('console-message', (_event, level, text) => { if (level >= 3) errors.push(text); });
  const wait = async (code, timeout = 15000) => {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      if (await evaluate(code)) return;
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw new Error('UI timeout: ' + code);
  };
  try {
    await new Promise(resolve => win.webContents.once('did-finish-load', resolve));
    await wait('!document.getElementById("login-btn").disabled');
    await evaluate(`document.getElementById('username').value='client_sender';
      document.getElementById('password').value='test'; document.getElementById('login-btn').click();`);
    await wait('document.querySelectorAll("#contact-list li").length > 0');
    await fs.writeFile(path.join(output, 'offline-window-ready.json'), JSON.stringify({ ready: true, root }));
    await wait('!document.getElementById("connection-login").hidden', 120000);
    await evaluate('document.querySelector("#contact-list li").click()');
    await wait('document.getElementById("messages").textContent.includes("离线消息持久化验证")');
    await wait('!!document.querySelector(".picture-button img[src]")');
    assert.equal(await evaluate('document.getElementById("send-btn").disabled'), true);
    await fs.writeFile(path.join(output, 'offline-chat.png'), (await win.webContents.capturePage()).toPNG());
    await evaluate('document.querySelector(".picture-link").click()');
    await wait('document.getElementById("original-status").textContent.includes("MB")');
    await fs.writeFile(path.join(output, 'offline-original.png'), (await win.webContents.capturePage()).toPNG());
    assert.deepEqual(errors, []);
    await fs.writeFile(path.join(output, 'offline-window-result.json'), JSON.stringify({ ok: true }));
    app.quit();
  } catch (error) {
    const picture = await evaluate(`({ html: document.querySelector('.picture-message')?.outerHTML,
      loads: [...imageView.loading], cache: [...imageView.cache], visibility: document.visibilityState })`);
    const probe = await evaluate(`Promise.race([
      window.chat.load({ requestId: crypto.randomUUID(), mediaId: '${mediaId}', variant: 'thumbnail', target: { is_group: false, target: 2 } })
        .then(value => ({ result: value })).catch(error => ({ error: error.message })),
      new Promise(resolve => setTimeout(() => resolve({ timeout: true }), 3000))])`);
    await fs.writeFile(path.join(output, 'offline-window-result.json'), JSON.stringify({ ok: false, error: error.message, errors, picture, probe, root }));
    await fs.writeFile(path.join(output, 'offline-window-failure.png'), (await win.webContents.capturePage()).toPNG());
    app.once('will-quit', () => app.exit(1));
    app.quit();
  }
}
start().catch(async error => {
  await fs.writeFile(path.join(output, 'offline-window-result.json'), JSON.stringify({ ok: false, error: error.message }));
  app.exit(1);
});
