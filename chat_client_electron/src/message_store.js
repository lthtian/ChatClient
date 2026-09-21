// ABOUTME: Stores account-scoped messages, synchronization coverage and media cache indexes.
// ABOUTME: Serializes SQLite transactions so messages and completed page boundaries commit together.
const sqlite = require('sqlite3');
const fs = require('node:fs/promises');
const path = require('node:path');

function conversationKey(value) {
  if (!value || typeof value.is_group !== 'boolean' || !Number.isSafeInteger(value.target) || value.target < 1) {
    throw new Error('invalid_conversation');
  }
  return `${value.is_group ? 'g' : 'p'}:${value.target}`;
}
function sequence(value) {
  const number = Number(value);
  if (!Number.isSafeInteger(number) || number < 0) throw new Error('invalid_sequence');
  return number;
}
class MessageStore {
  constructor(db, account) { this.db = db; this.account = account; this.tail = Promise.resolve(); }
  static async open(file, account) {
    if (!Number.isSafeInteger(account) || account < 1) throw new Error('invalid_account');
    await fs.mkdir(path.dirname(file), { recursive: true });
    const db = await new Promise((resolve, reject) => {
      const connection = new sqlite.Database(file, error => error ? reject(error) : resolve(connection));
    });
    const store = new MessageStore(db, account);
    try {
      const version = await store.get('PRAGMA user_version');
      if (![0, 1].includes(version.user_version)) throw new Error('unsupported_store_version');
      await store.exec(`PRAGMA journal_mode=WAL; PRAGMA busy_timeout=5000; PRAGMA synchronous=FULL;
        CREATE TABLE IF NOT EXISTS messages (
          local_id TEXT PRIMARY KEY, message_id TEXT UNIQUE, conversation TEXT NOT NULL,
          sequence INTEGER NOT NULL, sender_id INTEGER NOT NULL, client_msg_id TEXT,
          status TEXT NOT NULL, body TEXT NOT NULL,
          UNIQUE(sender_id, client_msg_id));
        CREATE INDEX IF NOT EXISTS messages_page ON messages(conversation, sequence);
        CREATE TABLE IF NOT EXISTS coverage (conversation TEXT PRIMARY KEY, lower INTEGER NOT NULL, upper INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS resources (media_id TEXT NOT NULL, variant TEXT NOT NULL,
          conversation TEXT NOT NULL, body TEXT NOT NULL, PRIMARY KEY(media_id, variant, conversation));
        PRAGMA user_version=1;`);
      return store;
    } catch (error) { await store.close(); throw error; }
  }
  exec(sql) { return new Promise((resolve, reject) => this.db.exec(sql, error => error ? reject(error) : resolve())); }
  run(sql, ...args) { return new Promise((resolve, reject) => this.db.run(sql, args, error => error ? reject(error) : resolve())); }
  get(sql, ...args) { return new Promise((resolve, reject) => this.db.get(sql, args, (error, row) => error ? reject(error) : resolve(row))); }
  all(sql, ...args) { return new Promise((resolve, reject) => this.db.all(sql, args, (error, rows) => error ? reject(error) : resolve(rows))); }
  serial(action) {
    const result = this.tail.then(() => { if (this.closed) throw new Error('store_closed'); return action(); });
    this.tail = result.catch(() => {}); return result;
  }
  async transaction(action) {
    await this.exec('BEGIN IMMEDIATE');
    try { const value = await action(); await this.exec('COMMIT'); return value; }
    catch (error) { await this.exec('ROLLBACK'); throw error; }
  }
  async write(target, message) {
    const key = conversationKey(target);
    if (!['text', 'image', 'file'].includes(message.kind) || !Number.isSafeInteger(message.sender_id) || message.sender_id < 1 ||
        !Number.isFinite(message.time) || (message.kind === 'text' && typeof message.text !== 'string')) {
      throw new Error('invalid_message');
    }
    if (message.message_id != null && !/^[1-9][0-9]*$/.test(String(message.message_id))) throw new Error('invalid_message');
    if (message.client_msg_id != null && !/^[a-f0-9-]{36}$/.test(message.client_msg_id)) throw new Error('invalid_message');
    const id = message.client_msg_id ? `c:${message.sender_id}:${message.client_msg_id}` : `m:${message.message_id}`;
    const seq = sequence(message.sequence || 0);
    if (message.message_id && !seq) throw new Error('invalid_sequence');
    if (!message.message_id && (!message.client_msg_id || message.sender_id !== this.account)) throw new Error('invalid_message');
    const existing = await this.get('SELECT message_id,conversation FROM messages WHERE local_id=?', id);
    if (existing && existing.conversation !== key) throw new Error('message_conflict');
    if (existing?.message_id && !message.message_id) return;
    const value = { ...message, status: message.message_id ? 'sent' : message.status || 'pending' };
    await this.run(`INSERT INTO messages VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(local_id) DO UPDATE SET
      message_id=excluded.message_id, sequence=excluded.sequence, status=excluded.status, body=excluded.body`,
    id, message.message_id ? String(message.message_id) : null, key, seq, message.sender_id,
    message.client_msg_id || null, value.status, JSON.stringify(value));
  }
  put(target, message) { return this.serial(() => this.write(target, message)); }
  pending(target, value) {
    return this.put(target, { ...value, sender_id: this.account, kind: 'text', time: Date.now(), status: 'pending' });
  }
  status(id, status, error = '') {
    return this.serial(async () => {
      const row = await this.get('SELECT body,message_id FROM messages WHERE sender_id=? AND client_msg_id=?', this.account, id);
      if (!row || row.message_id) return;
      const value = { ...JSON.parse(row.body), status, error };
      await this.run('UPDATE messages SET status=?,body=? WHERE sender_id=? AND client_msg_id=?', status, JSON.stringify(value), this.account, id);
    });
  }
  recover() {
    return this.serial(async () => {
      const rows = await this.all("SELECT local_id,body FROM messages WHERE message_id IS NULL AND status='pending'");
      for (const row of rows) await this.run("UPDATE messages SET status='paused',body=? WHERE local_id=?",
        JSON.stringify({ ...JSON.parse(row.body), status: 'paused' }), row.local_id);
    });
  }
  pendingMessage(id) {
    return this.serial(async () => {
      const row = await this.get('SELECT conversation,body FROM messages WHERE sender_id=? AND client_msg_id=? AND message_id IS NULL', this.account, id);
      if (!row) throw new Error('not_found');
      const [kind, target] = row.conversation.split(':');
      return { conversation: { is_group: kind === 'g', target: Number(target) }, message: JSON.parse(row.body) };
    });
  }
  coverage(target) {
    return this.serial(async () => await this.get('SELECT lower,upper FROM coverage WHERE conversation=?', conversationKey(target)) || null);
  }
  applyPage(target, page, direction) {
    return this.serial(() => this.transaction(async () => {
      const key = conversationKey(target), lower = sequence(page.lower), upper = sequence(page.upper);
      if (lower > upper + 1 || !['initial', 'before', 'after'].includes(direction)) throw new Error('invalid_page');
      const previous = await this.get('SELECT lower,upper FROM coverage WHERE conversation=?', key);
      if (direction !== 'initial' && !previous) throw new Error('missing_coverage');
      if (direction === 'after' && lower > previous.upper + 1) throw new Error('history_gap');
      if (direction === 'before' && upper < previous.lower - 1) throw new Error('history_gap');
      for (const message of page.messages) {
        if (sequence(message.sequence) < lower || sequence(message.sequence) > upper) throw new Error('invalid_page');
        await this.write(target, message);
      }
      await this.run(`INSERT INTO coverage VALUES(?,?,?) ON CONFLICT(conversation) DO UPDATE SET lower=excluded.lower,upper=excluded.upper`,
        key, previous ? Math.min(previous.lower, lower) : lower, previous ? Math.max(previous.upper, upper) : upper);
    }));
  }
  page(target, before = 0, limit = 50) {
    return this.serial(async () => {
      const key = conversationKey(target), cursor = sequence(before);
      if (!Number.isInteger(limit) || limit < 1 || limit > 100) throw new Error('invalid_limit');
      const rows = await this.all(`SELECT body FROM messages WHERE conversation=? AND sequence>0
        AND (?=0 OR sequence<?) ORDER BY sequence DESC LIMIT ?`, key, cursor, cursor, limit + 1);
      const messages = rows.slice(0, limit).reverse().map(row => JSON.parse(row.body));
      const covered = await this.get('SELECT lower,upper FROM coverage WHERE conversation=?', key);
      const earliest = messages.length ? Number(messages[0].sequence) : cursor;
      const hasMore = rows.length > limit || !covered || covered.lower > 1;
      if (!cursor) {
        const pending = await this.all('SELECT body FROM messages WHERE conversation=? AND message_id IS NULL ORDER BY rowid', key);
        messages.push(...pending.map(row => JSON.parse(row.body)));
      }
      return { messages, next_cursor: hasMore ? String(earliest || 0) : '', cached: true };
    });
  }
  resource(mediaId, variant, target, value) {
    return this.serial(() => {
      if (!/^[a-f0-9]{32}$/.test(mediaId) || !['original', 'thumbnail', 'preview'].includes(variant) ||
          !/^[a-f0-9]{32}-(original|thumbnail|preview)$/.test(value.name) || !Number.isSafeInteger(value.bytes) || value.bytes < 0 ||
          !/^[a-f0-9]{64}$/.test(value.sha256) || !['image/png', 'image/jpeg', 'application/octet-stream'].includes(value.mime)) throw new Error('invalid_resource');
      return this.run('INSERT OR REPLACE INTO resources VALUES(?,?,?,?)', mediaId, variant, conversationKey(target), JSON.stringify(value));
    });
  }
  getResource(mediaId, variant, target) {
    return this.serial(async () => {
      const row = await this.get('SELECT body FROM resources WHERE media_id=? AND variant=? AND conversation=?', mediaId, variant, conversationKey(target));
      return row ? JSON.parse(row.body) : null;
    });
  }
  media(mediaId, target) {
    return this.serial(async () => {
      const row = await this.get("SELECT body FROM messages WHERE conversation=? AND json_extract(body,'$.media.media_id')=? AND message_id IS NOT NULL",
        conversationKey(target), mediaId);
      if (!row) throw new Error('not_found');
      return JSON.parse(row.body);
    });
  }
  async close() {
    await this.tail;
    if (this.closed) return;
    this.closed = true;
    await new Promise((resolve, reject) => this.db.close(error => error ? reject(error) : resolve()));
  }
}
module.exports = { MessageStore, conversationKey, sequence };
