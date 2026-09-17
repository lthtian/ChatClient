// ABOUTME: Exchanges framed chat messages over a main-process TCP connection.
// ABOUTME: Correlates media requests and rejects pending operations on disconnect.
const net = require('net');
const EventEmitter = require('events');

/**
 * TCP 客户端 - Promise 风格的异步 API
 *
 * 核心优势：sendAndWait() 让你可以用 await 写出线性的异步流程
 *   const resp = await tcp.sendAndWait({ msgid: 1, ... }, 2);
 *   // 不需要回调、不需要信号/槽、不需要状态机
 */
class TcpClient extends EventEmitter {
  constructor() {
    super();
    this.socket = null;
    this.buffer = '';
    this.lanes = new Map();
  }

  /**
   * 连接服务器（异步，不阻塞 UI）
   */
  connect(host, port) {
    if (this.socket) throw new Error('连接已存在');
    this.buffer = '';
    return new Promise((resolve, reject) => {
      this.socket = net.createConnection({ host, port }, () => {
        this.socket.setTimeout(0);
        this.emit('connected');
        resolve();
      });
      this.socket.setTimeout(5000, () => this.socket?.destroy(new Error('连接超时')));

      this.socket.on('error', (err) => {
        if (this.listenerCount('error')) this.emit('error', err);
        reject(err);
      });

      this.socket.setEncoding('utf8');
      this.socket.on('data', (data) => {
        this.buffer += data;
        this._parseMessages();
      });

      this.socket.on('close', () => {
        this.socket = null;
        this.emit('disconnected');
      });
    });
  }

  /**
   * 带字符串感知的 JSON 帧解析（和 Qt 版本同样的逻辑）
   */
  _parseMessages() {
    while (true) {
      const msg = this._extractOneMessage();
      if (!msg) break;
      try {
        const json = JSON.parse(msg);
        this.emit('message', json);
      } catch (e) {
        this.close();
      }
    }
  }

  _extractOneMessage() {
    let braceCount = 0;
    let startPos = -1;
    let inString = false;

    for (let i = 0; i < this.buffer.length; i++) {
      const c = this.buffer[i];

      if (c === '"') {
        let backslashCount = 0;
        let j = i;
        while (j > 0 && this.buffer[j - 1] === '\\') {
          backslashCount++;
          j--;
        }
        if (backslashCount % 2 === 0) {
          inString = !inString;
        }
      }

      if (!inString) {
        if (c === '{') {
          if (startPos === -1) startPos = i;
          braceCount++;
        } else if (c === '}') {
          braceCount--;
          if (braceCount === 0 && startPos !== -1) {
            const msg = this.buffer.substring(startPos, i + 1);
            this.buffer = this.buffer.substring(i + 1);
            return msg;
          }
        }
      }
    }

    if (this.buffer.length > 1024 * 1024) {
      this.close();
      this.buffer = '';
    }

    return null;
  }

  /**
   * 发送 JSON 对象（无需手动序列化）
   */
  sendJson(obj) {
    if (this.socket && !this.socket.destroyed) {
      this.socket.write(JSON.stringify(obj));
    } else throw new Error('连接已断开');
  }

  /**
   * 发送请求并等待指定类型的响应 —— 这是 async/await 的核心
   *
   * 用法：
   *   const resp = await tcp.sendAndWait(
   *     { msgid: 1, username: "test", password: "123" },  // 发送
   *     2,      // 期望收到的响应 msgid
   *     5000    // 超时（毫秒）
   *   );
   *   console.log(resp.errno); // 直接拿到响应数据
   */
  sendAndWait(sendObj, waitMsgId, timeout = 5000) {
    if (sendObj.request_id) return this._wait(sendObj, waitMsgId, timeout);
    const previous = this.lanes.get(waitMsgId) || Promise.resolve();
    const pending = previous.catch(() => {}).then(() => this._wait(sendObj, waitMsgId, timeout));
    this.lanes.set(waitMsgId, pending);
    pending.finally(() => {
      if (this.lanes.get(waitMsgId) === pending) this.lanes.delete(waitMsgId);
    }).catch(() => {});
    return pending;
  }

  _wait(sendObj, waitMsgId, timeout) {
    return new Promise((resolve, reject) => {
      const cleanup = () => {
        this.off('message', handler);
        this.off('disconnected', disconnected);
        clearTimeout(timer);
      };
      const disconnected = () => { cleanup(); reject(new Error('连接已断开')); };
      const timer = setTimeout(() => {
        cleanup();
        if (!sendObj.request_id) this.close();
        reject(new Error(`等待 msgid ${waitMsgId} 超时`));
      }, timeout);

      const handler = (msg) => {
        if (msg.msgid === waitMsgId &&
            (!sendObj.request_id || msg.request_id === sendObj.request_id)) {
          cleanup();
          resolve(msg);
        }
      };

      this.on('message', handler);
      this.once('disconnected', disconnected);
      try { this.sendJson(sendObj); } catch (error) { cleanup(); reject(error); }
    });
  }

  close() {
    if (this.socket) {
      this.socket.destroy();
    }
  }
}

// 导出给 renderer 使用
if (typeof module !== 'undefined') {
  module.exports = TcpClient;
}
