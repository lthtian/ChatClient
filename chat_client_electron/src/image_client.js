// ABOUTME: Persists account-scoped image sends and resumes them using stable message identifiers.
// ABOUTME: Owns local image copies, bounded downloads and private disk cache entries.
const fs = require('node:fs/promises');
const { createReadStream } = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { EventEmitter } = require('node:events');
const { processImage } = require('./image_processor');
const { transferFile } = require('./image_transfer');

const limit = 20 * 1024 * 1024;
const identifier = /^[a-f0-9-]{36}$/;
function conversation(value) {
  if (!value || typeof value.is_group !== 'boolean' || !Number.isSafeInteger(value.target) || value.target < 1) {
    throw new Error('invalid_conversation');
  }
  return { is_group: value.is_group, target: value.target };
}
async function digest(file) {
  const hash = crypto.createHash('sha256');
  for await (const chunk of createReadStream(file)) hash.update(chunk);
  return hash.digest('hex');
}
class ImageClient extends EventEmitter {
  constructor({ root, toolPath, rpc }) {
    super();
    this.root = root; this.toolPath = toolPath; this.rpc = rpc;
    this.jobs = new Map(); this.running = new Map(); this.downloads = new Map();
    this.operations = new Set();
    this.writes = Promise.resolve(); this.account = null; this.online = false;
    this.downloadCount = 0; this.downloadWaiters = [];
  }
  async login(account) {
    if (!Number.isSafeInteger(account) || account < 1) throw new Error('invalid_account');
    this.suspend();
    await Promise.allSettled([...this.operations]);
    if (this.account !== account) this.jobs.clear();
    this.account = account; this.directory = path.join(this.root, String(account));
    await fs.mkdir(path.join(this.directory, 'cache'), { recursive: true });
    if (!this.jobs.size) {
      let records = [];
      try { records = JSON.parse(await fs.readFile(path.join(this.directory, 'tasks.json'), 'utf8')); }
      catch (error) { if (error.code !== 'ENOENT') throw new Error('task_store_unreadable'); }
      if (!Array.isArray(records) || records.length > 20) throw new Error('task_store_unreadable');
      for (const job of records) {
        if (!identifier.test(job.id)) throw new Error('task_store_unreadable');
        conversation(job.conversation);
        job.state = job.cancelRequested ? 'canceling' : 'paused'; job.progress = 0;
        this.jobs.set(job.id, job);
      }
    }
    for (const entry of await fs.readdir(this.directory, { withFileTypes: true })) {
      if (entry.isDirectory() && identifier.test(entry.name) && !this.jobs.has(entry.name)) {
        await fs.rm(path.join(this.directory, entry.name), { recursive: true, force: true });
      }
    }
    const cacheDirectory = path.join(this.directory, 'cache');
    for (const name of await fs.readdir(cacheDirectory)) {
      if (name.endsWith('.part')) await fs.rm(path.join(cacheDirectory, name), { force: true });
    }
    this.online = true;
    return this.list();
  }
  suspend() {
    this.online = false;
    for (const controller of this.running.values()) controller.abort();
    for (const controller of this.downloads.values()) controller.abort();
  }
  list() { return [...this.jobs.values()].map(job => this.view(job)); }
  view(job) {
    return { id: job.id, conversation: job.conversation, name: job.name, createdAt: job.createdAt,
      state: job.state, progress: job.progress || 0, error: job.error || '',
      source: job.source, thumbnail: job.thumbnail, message: job.message };
  }
  folder(id) {
    if (!identifier.test(id)) throw new Error('invalid_task');
    return path.join(this.directory, id);
  }
  preview(id) { return path.join(this.folder(id), 'preview', 'objects', 'thumbnail'); }
  async save() {
    const contents = JSON.stringify([...this.jobs.values()].filter(job => job.state !== 'sent'));
    const file = path.join(this.directory, 'tasks.json');
    const write = this.writes.catch(() => {}).then(async () => {
      const handle = await fs.open(file + '.tmp', 'w');
      try { await handle.writeFile(contents); await handle.sync(); } finally { await handle.close(); }
      await fs.rename(file + '.tmp', file);
    });
    this.writes = write;
    await write;
  }
  async change(job, state, error = '') {
    job.state = state; job.error = error;
    await this.save(); this.emit('task', this.view(job));
  }
  async enqueue(file, target) {
    if (!this.online) throw new Error('disconnected');
    if (this.jobs.size >= 20) throw new Error('queue_full');
    const stat = await fs.stat(file);
    if (!stat.isFile() || stat.size < 1 || stat.size > limit) throw new Error('byte_limit');
    const job = { id: crypto.randomUUID(), conversation: conversation(target), name: path.basename(file),
      bytes: stat.size, createdAt: Date.now(), state: 'preparing', progress: 0 };
    const directory = this.folder(job.id);
    await fs.mkdir(directory);
    this.jobs.set(job.id, job);
    try {
      await fs.copyFile(file, path.join(directory, 'original'));
      if ((await fs.stat(path.join(directory, 'original'))).size !== stat.size) throw new Error('source_changed');
      await this.change(job, 'queued');
      this.pump();
      return this.view(job);
    } catch (error) {
      this.jobs.delete(job.id); await fs.rm(directory, { recursive: true, force: true }); throw error;
    }
  }
  pump() {
    if (!this.online) return;
    for (const job of this.jobs.values()) {
      if (this.running.size >= 2) break;
      if (!['queued', 'canceling'].includes(job.state) || this.running.has(job.id)) continue;
      const controller = new AbortController(); this.running.set(job.id, controller);
      const operation = this.run(job, controller.signal).catch(error => {
        this.emit('storage-error', error.message);
      }).finally(() => { this.running.delete(job.id); this.operations.delete(operation); this.pump(); });
      this.operations.add(operation);
    }
  }
  async run(job, signal) {
    try {
      const sourcePath = path.join(this.folder(job.id), 'original');
      if (job.cancelRequested) { await this.finishCancel(job); return; }
      if (!job.source) {
        await this.change(job, 'preparing');
        await fs.rm(path.join(this.folder(job.id), 'preview'), { recursive: true, force: true });
        const result = await processImage({ toolPath: this.toolPath, sourcePath,
          outputDirectory: path.join(this.folder(job.id), 'preview'), signal });
        job.source = result.source; job.thumbnail = result.thumbnail;
        job.sha256 = await digest(sourcePath);
        await this.change(job, 'requesting');
      }
      signal.throwIfAborted();
      let media = await this.rpc('begin', { conversation: job.conversation, client_msg_id: job.id,
        bytes: job.bytes, sha256: job.sha256 });
      job.mediaId = media.media_id;
      await this.save();
      signal.throwIfAborted();
      if (['failed', 'processing'].includes(media.state) || (media.expired && media.state !== 'ready')) {
        media = await this.rpc('retry', { media_id: job.mediaId });
      }
      if (media.state === 'uploading') {
        if (!media.upload) throw new Error('expired');
        await this.change(job, 'uploading');
        let last = 0;
        await transferFile({ descriptor: media.upload, file: sourcePath, bytes: job.bytes, signal,
          uploaded: () => { job.state = 'processing'; this.emit('task', this.view(job)); },
          progress: ({ loaded, total }) => {
            job.progress = Math.floor(loaded / total * 100);
            if (Date.now() - last > 100 || loaded === total) {
              last = Date.now(); this.emit('task', this.view(job));
            }
          } });
      } else if (media.state !== 'ready') throw new Error(media.state);
      signal.throwIfAborted();
      await this.change(job, 'confirming');
      const payload = { conversation: job.conversation, client_msg_id: job.id, media_id: job.mediaId };
      let sent;
      try { sent = await this.rpc('publish', payload); }
      catch (error) {
        if (error.message !== 'expired') throw error;
        await this.rpc('retry', { media_id: job.mediaId });
        sent = await this.rpc('publish', payload);
      }
      job.message = sent.message;
      await this.change(job, 'sent');
      this.jobs.delete(job.id);
      await fs.rm(this.folder(job.id), { recursive: true, force: true });
    } catch (error) {
      if (job.cancelRequested && this.online) {
        try { await this.finishCancel(job); return; }
        catch (cancelError) { error = cancelError; }
      }
      await this.change(job, this.online ? 'failed' : 'paused', error.code || error.message);
    }
  }
  async retry(id) {
    const job = this.jobs.get(id);
    if (!job || this.running.has(id)) throw new Error('busy');
    if (!this.online) throw new Error('disconnected');
    await this.change(job, job.cancelRequested ? 'canceling' : 'queued'); this.pump();
  }
  async cancel(id) {
    const job = this.jobs.get(id);
    if (!job) return;
    job.cancelRequested = true;
    await this.change(job, 'canceling');
    const controller = this.running.get(id);
    if (controller) controller.abort();
    else this.pump();
  }
  async finishCancel(job) {
    if (job.mediaId) {
      try { await this.rpc('cancel', { media_id: job.mediaId }); }
      catch (error) {
        if (error.message !== 'already_sent') throw error;
        const sent = await this.rpc('publish', { conversation: job.conversation,
          client_msg_id: job.id, media_id: job.mediaId });
        job.message = sent.message; await this.change(job, 'sent');
      }
    }
    this.jobs.delete(job.id); await this.save();
    if (!job.message) this.emit('task', { ...this.view(job), state: 'canceled' });
    await fs.rm(this.folder(job.id), { recursive: true, force: true });
  }
  async slot(signal) {
    signal.throwIfAborted();
    if (this.downloadCount < 3) { this.downloadCount++; return; }
    await new Promise((resolve, reject) => {
      const waiter = () => { signal.removeEventListener('abort', aborted); resolve(); };
      const aborted = () => {
        this.downloadWaiters = this.downloadWaiters.filter(item => item !== waiter);
        reject(signal.reason);
      };
      signal.addEventListener('abort', aborted, { once: true });
      this.downloadWaiters.push(waiter);
    });
    if (signal.aborted) { this.releaseSlot(); signal.throwIfAborted(); }
  }
  releaseSlot() {
    const waiter = this.downloadWaiters.shift();
    if (waiter) waiter(); else this.downloadCount--;
  }
  load(options) {
    const operation = this.loadFile(options);
    this.operations.add(operation);
    operation.finally(() => this.operations.delete(operation)).catch(() => {});
    return operation;
  }
  async loadFile({ requestId, mediaId, variant, target, force = false }) {
    if (!this.online) throw new Error('disconnected');
    if (!identifier.test(requestId) || !/^[a-f0-9]{32}$/.test(mediaId) ||
        !['original', 'thumbnail'].includes(variant) || this.downloads.has(requestId)) throw new Error('invalid_argument');
    if (this.downloads.size >= 100) throw new Error('busy');
    const controller = new AbortController(); const signal = controller.signal;
    this.downloads.set(requestId, controller);
    let acquired = false;
    const file = path.join(this.directory, 'cache', `${mediaId}-${variant}`);
    const temporary = file + '.' + requestId + '.part';
    try {
      await this.slot(signal); acquired = true;
      const result = await this.rpc('read', { media_id: mediaId, variant, conversation: conversation(target) });
      signal.throwIfAborted();
      const stat = await fs.stat(file).catch(error => { if (error.code !== 'ENOENT') throw error; return null; });
      if (!force && stat?.size === result.bytes && (!result.sha256 || await digest(file) === result.sha256)) {
        await fs.utimes(file, new Date(), new Date()); return { file, mime: result.mime };
      }
      let last = 0;
      await transferFile({ descriptor: result.download, file: temporary, bytes: result.bytes, signal,
        progress: value => {
          if (Date.now() - last > 100 || value.loaded === value.total) {
            last = Date.now(); this.emit('progress', { requestId, ...value });
          }
        } });
      if (result.sha256 && await digest(temporary) !== result.sha256) throw new Error('hash_mismatch');
      await fs.rename(temporary, file);
      await this.trimCache(file);
      return { file, mime: result.mime };
    } finally {
      await fs.rm(temporary, { force: true });
      this.downloads.delete(requestId);
      if (acquired) this.releaseSlot();
    }
  }
  cancelLoad(id) { this.downloads.get(id)?.abort(); }
  async trimCache(keep) {
    const directory = path.join(this.directory, 'cache');
    const entries = [];
    for (const name of await fs.readdir(directory)) {
      if (!/^[a-f0-9]{32}-(original|thumbnail)$/.test(name)) continue;
      const file = path.join(directory, name); const stat = await fs.stat(file).catch(() => null);
      if (stat) entries.push({ file, size: stat.size, time: stat.mtimeMs });
    }
    let total = entries.reduce((sum, entry) => sum + entry.size, 0);
    for (const entry of entries.sort((a, b) => a.time - b.time)) {
      if (total <= 256 * 1024 * 1024) break;
      if (entry.file === keep) continue;
      await fs.rm(entry.file, { force: true }); total -= entry.size;
    }
  }
}
module.exports = { ImageClient, conversation, digest };
