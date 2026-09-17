// ABOUTME: Exposes narrow chat and image actions from the trusted Electron main process.
// ABOUTME: Restricts file selection, image URLs and IPC callers to the application window.
const { app, ipcMain, dialog, protocol, net } = require('electron');
const { pathToFileURL } = require('node:url');
const path = require('node:path');
const crypto = require('node:crypto');
const TcpClient = require('./tcp');
const { ImageClient } = require('./image_client');

protocol.registerSchemesAsPrivileged([{ scheme: 'chat-image', privileges: {
  standard: true, secure: true, supportFetchAPI: true, stream: true,
} }]);

function installBridge(win) {
  const tcp = new TcpClient();
  const host = process.env.CHAT_HOST || '127.0.0.1';
  const port = Number(process.env.CHAT_PORT || 16000);
  const assets = new Map();
  let account = 0;
  const send = (event, data) => { if (!win.isDestroyed()) win.webContents.send('chat:event', event, data); };
  const rpc = async (op, fields) => {
    if (!account) throw new Error('unauthorized');
    const response = await tcp.sendAndWait({ ...fields, msgid: 26, request_id: crypto.randomUUID(), op }, 27, 15000);
    if (!response.ok) throw new Error(response.error);
    return response.data;
  };
  const serverKey = crypto.createHash('sha256').update(`${host}:${port}`).digest('hex').slice(0, 16);
  const images = new ImageClient({ root: path.join(app.getPath('userData'), 'images', serverKey), rpc,
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
      ? asset(images.preview(job.id), job.thumbnail.mime) : null };
  }
  protocol.handle('chat-image', async request => {
    const url = new URL(request.url);
    const found = account && url.hostname === 'asset' ? assets.get(url.pathname.slice(1)) : null;
    if (!found) return new Response('', { status: 404 });
    try { return await net.fetch(pathToFileURL(found.file).toString()); }
    catch (_) { return new Response('', { status: 404 }); }
  });
  images.on('task', job => { if (images.account === account) send('task', task(job)); });
  images.on('progress', value => send('progress', value));
  images.on('storage-error', message => send('image-error', message));
  tcp.on('message', message => send('message', message));
  tcp.on('error', () => {});
  tcp.on('disconnected', () => { account = 0; assets.clear(); images.suspend(); send('disconnected'); });
  const acknowledgments = new Map([[1, 2], [3, 4], [6, 13], [7, 15], [8, 14], [11, 12], [20, 21]]);
  const sends = new Set([5, 9, 18, 19, 22, 23, 24]);
  function payload(value) {
    if (!value || typeof value !== 'object' || JSON.stringify(value).length > 1024 * 1024) throw new Error('invalid_argument');
    if (![1, 3].includes(value.msgid) && !account) throw new Error('unauthorized');
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
        account = Number(response.id);
        try { await images.login(account); }
        catch (error) { send('image-error', error.message); }
      }
      return response;
    },
    send: value => { if (!sends.has(value?.msgid)) throw new Error('invalid_argument'); tcp.sendJson(payload(value)); },
    history: value => rpc('history', value),
    jobs: () => images.list().map(task),
    pick: async target => {
      if (!account) throw new Error('unauthorized');
      const owner = account;
      const selected = await dialog.showOpenDialog(win, { title: '发送图片（JPEG / PNG，最大 20 MB）',
        properties: ['openFile'], filters: [{ name: '图片', extensions: ['jpg', 'jpeg', 'png'] }] });
      if (selected.canceled) return null;
      if (owner !== account) throw new Error('unauthorized');
      return task(await images.enqueue(selected.filePaths[0], target));
    },
    retry: id => images.retry(id),
    cancel: id => images.cancel(id),
    load: async value => {
      const owner = account;
      const result = await images.load(value);
      if (!owner || owner !== account) throw new Error('unauthorized');
      return { url: asset(result.file, result.mime) };
    },
    cancelLoad: id => images.cancelLoad(id),
  };
  ipcMain.handle('chat:invoke', async (event, action, value) => {
    if (event.sender !== win.webContents || event.senderFrame !== win.webContents.mainFrame ||
        !Object.hasOwn(handlers, action)) throw new Error('unauthorized');
    try { return { ok: true, data: await handlers[action](value) }; }
    catch (error) { return { ok: false, error: error.code || error.message }; }
  });
  win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  win.webContents.on('will-navigate', event => event.preventDefault());
  win.on('closed', () => { images.suspend(); tcp.close(); ipcMain.removeHandler('chat:invoke'); protocol.unhandle('chat-image'); });
}
module.exports = { installBridge };
