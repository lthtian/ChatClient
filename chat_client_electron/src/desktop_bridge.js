// ABOUTME: Exposes narrow chat and image actions from the trusted Electron main process.
// ABOUTME: Restricts file selection, image URLs and IPC callers to the application window.
const { app, ipcMain, dialog, protocol, net } = require('electron');
const { pathToFileURL } = require('node:url');
const path = require('node:path');
const crypto = require('node:crypto');
const TcpClient = require('./tcp');
const { MediaClient } = require('./media_client');
const { MessageSync } = require('./message_sync');

protocol.registerSchemesAsPrivileged([{ scheme: 'chat-image', privileges: {
  standard: true, secure: true, supportFetchAPI: true, stream: true,
} }]);

function installBridge(win) {
  const tcp = new TcpClient();
  const host = process.env.CHAT_HOST || '127.0.0.1';
  const port = Number(process.env.CHAT_PORT || 16000);
  const assets = new Map();
  let account = 0;
  let authenticated = false, accountName = '', messages = null;
  const send = (event, data) => { if (!win.isDestroyed()) win.webContents.send('chat:event', event, data); };
  const rpc = async (op, fields) => {
    if (!authenticated) throw new Error('disconnected');
    const response = await tcp.sendAndWait({ ...fields, msgid: 26, request_id: crypto.randomUUID(), op }, 27, 15000);
    if (!response.ok) throw new Error(response.error);
    return response.data;
  };
  const serverKey = crypto.createHash('sha256').update(`${host}:${port}`).digest('hex').slice(0, 16);
  const media = new MediaClient({ root: path.join(app.getPath('userData'), 'images', serverKey), rpc,
    toolPath: process.env.CHAT_IMAGE_TOOL || path.join(__dirname, '..', 'resources', 'native', 'chat_image.exe') });
  function asset(file, mime) {
    for (const [token, value] of assets) {
      if (value.file === file) return 'chat-image://asset/' + token;
    }
    const token = crypto.randomUUID(); assets.set(token, { file, mime });
    if (assets.size > 512) assets.delete(assets.keys().next().value);
    return 'chat-image://asset/' + token;
  }
  function task(job) {
    return { ...job, preview: job.thumbnail && job.state !== 'sent'
      ? asset(media.preview(job.id), job.thumbnail.mime) : null };
  }
  protocol.handle('chat-image', async request => {
    const url = new URL(request.url);
    const found = account && url.hostname === 'asset' ? assets.get(url.pathname.slice(1)) : null;
    if (!found) return new Response('', { status: 404 });
    try { return await net.fetch(pathToFileURL(found.file).toString()); }
    catch (_) { return new Response('', { status: 404 }); }
  });
  media.on('task', job => { if (media.account === account) send('task', task(job)); });
  media.on('committed', value => { if (media.account === account) send('stored', value); });
  media.on('progress', value => send('progress', value));
  media.on('storage-error', message => send('image-error', message));
  tcp.on('message', message => {
    if (message.msgid !== 28) { send('message', message); return; }
    if (!messages || !authenticated) return;
    messages.receive(message.conversation, message.message).catch(error => send('image-error', error.message));
  });
  tcp.on('error', () => {});
  tcp.on('disconnected', () => { authenticated = false; media.suspend(); send('disconnected'); });
  const acknowledgments = new Map([[1, 2], [3, 4], [6, 13], [7, 15], [8, 14], [11, 12], [20, 21]]);
  const sends = new Set([18, 19, 22, 23, 24]);
  function payload(value) {
    if (!value || typeof value !== 'object' || JSON.stringify(value).length > 1024 * 1024) throw new Error('invalid_argument');
    if (![1, 3].includes(value.msgid) && !authenticated) throw new Error('unauthorized');
    const result = { ...value };
    if (result.msgid !== 24) {
      if ('id' in result) result.id = account;
      if ('userid' in result) result.userid = account;
    }
    return result;
  }
  const handlers = {
    connect: async () => {
      if (tcp.socket && !tcp.socket.destroyed) return;
      await tcp.connect(host, port);
    },
    request: async value => {
      if (!acknowledgments.has(value?.msgid)) throw new Error('invalid_argument');
      const response = await tcp.sendAndWait(payload(value), acknowledgments.get(value.msgid));
      if (value.msgid === 1 && response.errno === 0) {
        authenticated = false;
        if (messages) await messages.close();
        messages = null;
        assets.clear();
        account = Number(response.id);
        accountName = response.name;
        try {
          await media.login(account);
          const owner = account;
          const service = new MessageSync({ store: media.store, rpc,
            failed: error => { if (owner === account && messages === service) send('image-error', error.message); },
            changed: (conversation, message) => {
              if (owner === account && messages === service) send('stored', { conversation, message });
            } });
          messages = service;
          authenticated = true;
        } catch (error) {
          account = 0; tcp.close(); throw error;
        }
      }
      return response;
    },
    send: value => { if (!sends.has(value?.msgid)) throw new Error('invalid_argument'); tcp.sendJson(payload(value)); },
    history: value => { if (!messages) throw new Error('unauthorized'); return messages.page(value); },
    syncHistory: value => { if (!authenticated || !messages) throw new Error('disconnected'); return messages.sync(value); },
    sendText: value => {
      if (!authenticated || !messages) throw new Error('disconnected');
      return messages.send(value.conversation, value.text, accountName);
    },
    retryText: id => {
      if (!authenticated || !messages) throw new Error('disconnected');
      return messages.retry(id);
    },
    jobs: () => media.list().map(task),
    pick: async target => {
      if (!authenticated) throw new Error('unauthorized');
      const owner = account;
      const selected = await dialog.showOpenDialog(win, { title: '发送图片（JPEG / PNG，最大 20 MB）',
        properties: ['openFile'], filters: [{ name: '图片', extensions: ['jpg', 'jpeg', 'png'] }] });
      if (selected.canceled) return null;
      if (owner !== account) throw new Error('unauthorized');
      return task(await media.enqueue(selected.filePaths[0], target));
    },
    pickFile: async target => {
      if (!authenticated) throw new Error('unauthorized');
      const owner = account;
      const selected = await dialog.showOpenDialog(win, { title: '发送文件（最大 100 MB）', properties: ['openFile'] });
      if (selected.canceled) return null;
      if (owner !== account) throw new Error('unauthorized');
      const file = selected.filePaths[0];
      return task(await media.enqueue(file, target, /\.(jpe?g|png)$/i.test(file) ? 'image' : 'file'));
    },
    saveFile: async value => {
      const owner = account;
      if (!owner || !media.store) throw new Error('unauthorized');
      const message = await media.store.media(value.mediaId, value.target);
      if (message.kind !== 'file') throw new Error('invalid_argument');
      let name = message.media.name.replace(/[<>:"/\\|?*\x00-\x1f]/g, '_').replace(/[. ]+$/g, '') || 'file';
      if (/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(name)) name = 'download-' + name;
      const selected = await dialog.showSaveDialog(win, { title: '保存文件', defaultPath: name });
      if (selected.canceled) return { canceled: true };
      if (owner !== account) throw new Error('unauthorized');
      return media.saveAs({ requestId: value.requestId, mediaId: value.mediaId,
        target: value.target, destination: selected.filePath });
    },
    retry: id => media.retry(id),
    cancel: id => media.cancel(id),
    load: async value => {
      const owner = account;
      const result = await media.load(value);
      if (!owner || owner !== account) throw new Error('unauthorized');
      if (!['image/png', 'image/jpeg'].includes(result.mime)) throw new Error('invalid_argument');
      return { url: asset(result.file, result.mime) };
    },
    cancelLoad: id => media.cancelLoad(id),
  };
  ipcMain.handle('chat:invoke', async (event, action, value) => {
    if (event.sender !== win.webContents || event.senderFrame !== win.webContents.mainFrame ||
        !Object.hasOwn(handlers, action)) throw new Error('unauthorized');
    try { return { ok: true, data: await handlers[action](value) }; }
    catch (error) { return { ok: false, error: error.name === 'AbortError' ? 'aborted' : error.code || error.message }; }
  });
  win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  win.webContents.on('will-navigate', event => event.preventDefault());
  let shutdown;
  const close = () => {
    if (shutdown) return shutdown;
    authenticated = false; media.suspend(); tcp.close();
    const stopped = messages ? messages.close() : Promise.resolve();
    shutdown = stopped.then(() => media.close());
    ipcMain.removeHandler('chat:invoke'); protocol.unhandle('chat-image');
    return shutdown;
  };
  win.on('closed', () => { close().catch(error => console.error('Storage shutdown failed:', error.message)); });
  return close;
}
module.exports = { installBridge };
