// ABOUTME: Reconciles committed messages and sends with a durable local conversation store.
// ABOUTME: Fetches missing history ranges without treating live notifications as completed synchronization.
const crypto = require('node:crypto');
const { conversationKey, sequence } = require('./message_store');

class MessageSync {
  constructor({ store, rpc, changed, failed }) {
    this.store = store; this.rpc = rpc; this.changed = changed;
    this.failed = failed || (error => console.error('Message storage failed:', error.message));
    this.requests = new Map(); this.sends = new Map(); this.closed = false;
  }
  async receive(target, message) {
    if (this.closed) return;
    await this.store.put(target, message);
    if (!this.closed) this.changed(target, message);
  }
  async send(target, text, name) {
    if (typeof text !== 'string' || !text.trim() || Buffer.byteLength(text) > 16384) throw new Error('invalid_text');
    const id = crypto.randomUUID();
    await this.store.pending(target, { client_msg_id: id, text, sender_name: name });
    this.changed(target);
    // The task is durable before network transmission starts.
    this.retry(id).catch(error => this.failed(error));
    return { client_msg_id: id };
  }
  retry(id) {
    if (this.closed) return Promise.reject(new Error('disconnected'));
    if (this.sends.has(id)) return this.sends.get(id);
    const operation = this.transmit(id).finally(() => this.sends.delete(id));
    this.sends.set(id, operation); return operation;
  }
  async transmit(id) {
    const { conversation: target, message } = await this.store.pendingMessage(id);
    await this.store.status(id, 'pending'); this.changed(target);
    try {
      const result = await this.rpc('send_text', { conversation: target, client_msg_id: id, text: message.text });
      await this.store.put(target, result.message);
    } catch (error) {
      await this.store.status(id, this.closed ? 'paused' : 'failed', error.message);
    }
    if (!this.closed) this.changed(target);
  }
  page(value) { return this.store.page(value.conversation, value.before_sequence || 0, value.limit || 50); }
  sync(value) {
    const key = conversationKey(value.conversation);
    // Requests for one conversation complete in order, including different page directions.
    const previous = this.requests.get(key) || Promise.resolve();
    const operation = previous.catch(() => {}).then(() => this.fetch(value)).finally(() => {
      if (this.requests.get(key) === operation) this.requests.delete(key);
    });
    this.requests.set(key, operation); return operation;
  }
  async fetch(value) {
    if (this.closed) throw new Error('disconnected');
    const target = value.conversation, before = sequence(value.before_sequence || 0);
    let coverage = await this.store.coverage(target);
    if (!coverage) {
      const page = await this.rpc('sync', { conversation: target, direction: 'initial', limit: 50 });
      if (this.closed) throw new Error('disconnected');
      await this.store.applyPage(target, page, 'initial');
      coverage = await this.store.coverage(target);
    } else if (!before) {
      let more = true, through;
      while (more) {
        const page = await this.rpc('sync', { conversation: target, direction: 'after',
          cursor: coverage.upper, limit: 100, ...(through === undefined ? {} : { through }) });
        if (this.closed) throw new Error('disconnected');
        if (page.more && Number(page.upper) <= coverage.upper) throw new Error('invalid_sync_progress');
        await this.store.applyPage(target, page, 'after');
        through = page.through; more = page.more;
        coverage = await this.store.coverage(target);
      }
    }
    if (before && coverage.lower > 1) {
      const cached = await this.store.page(target, before, value.limit || 50);
      const oldest = cached.messages[0];
      if (cached.messages.length < (value.limit || 50) || !oldest || Number(oldest.sequence) < coverage.lower) {
        const page = await this.rpc('sync', { conversation: target, direction: 'before', cursor: coverage.lower, limit: 50 });
        if (this.closed) throw new Error('disconnected');
        await this.store.applyPage(target, page, 'before');
      }
    }
    return this.page(value);
  }
  async close() {
    this.closed = true;
    await Promise.allSettled([...this.requests.values(), ...this.sends.values()]);
  }
}
module.exports = { MessageSync };
