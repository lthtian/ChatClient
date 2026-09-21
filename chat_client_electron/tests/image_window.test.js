// ABOUTME: Opens the actual isolated Electron window against the real chat service.
// ABOUTME: Verifies history thumbnails, original preview, dismissal and renderer isolation.
const { app, BrowserWindow } = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'chat-image-window-'));
app.setPath('userData', directory);
process.env.CHAT_HOST = '127.0.0.1';
process.env.CHAT_PORT = process.argv[2];
require('../main');

app.whenReady().then(async () => {
  const window = BrowserWindow.getAllWindows()[0];
  const errors = [];
  window.webContents.on('console-message', (_event, level, message) => { if (level >= 3) errors.push(message); });
  const evaluate = script => window.webContents.executeJavaScript(script);
  const wait = async script => {
    const end = Date.now() + 15000;
    while (Date.now() < end) {
      if (await evaluate(script)) return;
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw new Error('UI condition timed out: ' + script);
  };
  try {
    await new Promise(resolve => window.webContents.once('did-finish-load', resolve));
    await wait('!document.getElementById("login-btn").disabled');
    assert.equal(await evaluate('typeof require'), 'undefined');
    assert.equal(await evaluate('typeof process'), 'undefined');
    await evaluate(`document.getElementById('username').value='client_sender';
      document.getElementById('password').value='test'; document.getElementById('login-btn').click();`);
    await wait('document.querySelectorAll("#contact-list li").length > 0');
    await evaluate('document.querySelector("#contact-list li").click()');
    await wait('!!document.querySelector(".picture-button img[src]")');
    await wait('!!document.querySelector(".file-message .file-save")');
    assert.ok(await evaluate('document.querySelector(".file-message").textContent.includes("资料 21MB.bin")'));
    const chat = await window.webContents.capturePage();
    fs.writeFileSync(path.join(process.argv[3], 'image-chat.png'), chat.toPNG());
    await evaluate('document.querySelector(".picture-link").click()');
    await wait('document.getElementById("original-status").textContent.includes("MB")');
    assert.equal(await evaluate('document.getElementById("image-viewer").open'), true);
    const preview = await window.webContents.capturePage();
    fs.writeFileSync(path.join(process.argv[3], 'image-preview.png'), preview.toPNG());
    await evaluate('document.getElementById("viewer-close").click()');
    assert.equal(await evaluate('document.getElementById("image-viewer").open'), false);
    await evaluate(`document.getElementById('msg-input').value='窗口文本确认验证';
      document.getElementById('send-btn').click();`);
    await wait(`(state.messages.get(state.currentChat) || []).some(message =>
      message.text === '窗口文本确认验证' && message.status === 'sent')`);
    assert.equal(await evaluate(`[...document.querySelectorAll('.msg-bubble')]
      .filter(element => element.textContent === '窗口文本确认验证').length`), 1);
    assert.deepEqual(errors, []);
    fs.writeFileSync(path.join(process.argv[3], 'image-window-result.json'), JSON.stringify({ ok: true }));
    app.quit();
  } catch (error) {
    const failure = await window.webContents.capturePage();
    fs.writeFileSync(path.join(process.argv[3], 'image-window-failure.png'), failure.toPNG());
    fs.writeFileSync(path.join(process.argv[3], 'image-window-result.json'), JSON.stringify({ ok: false, error: error.message, errors }));
    app.once('will-quit', () => app.exit(1));
    app.quit();
  }
});
