// ABOUTME: Streams media files with byte limits, cancellation and transfer progress.
// ABOUTME: Accepts HTTPS or loopback HTTP descriptors without following redirects.
const http = require('node:http');
const https = require('node:https');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const { Transform } = require('node:stream');
const { pipeline } = require('node:stream/promises');

function endpoint(descriptor) {
  const url = new URL(descriptor.url);
  if (url.username || url.password || url.hash ||
      !(url.protocol === 'https:' || (url.protocol === 'http:' &&
        ['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname)))) throw new Error('insecure_endpoint');
  if (!['GET', 'PUT'].includes(descriptor.method)) throw new Error('invalid_descriptor');
  return url;
}

function counter(expected, progress) {
  let received = 0;
  return new Transform({ transform(chunk, encoding, callback) {
    received += chunk.length;
    if (received > expected) return callback(new Error('size_mismatch'));
    progress?.({ loaded: received, total: expected });
    callback(null, chunk);
  }, flush(callback) { callback(received === expected ? null : new Error('size_mismatch')); } });
}

async function transferFile({ descriptor, file, bytes, signal, progress, uploaded }) {
  if (!Number.isSafeInteger(bytes) || bytes < 0 || bytes > 100 * 1024 * 1024) throw new Error('byte_limit');
  const url = endpoint(descriptor);
  const upload = descriptor.method === 'PUT';
  const headers = { ...descriptor.headers };
  if (upload) headers['Content-Length'] = bytes;
  const request = (url.protocol === 'https:' ? https : http).request(url, {
    method: descriptor.method, headers, signal,
  });
  request.setTimeout(30000, () => request.destroy(new Error('transfer_timeout')));
  const deadline = setTimeout(() => request.destroy(new Error('transfer_timeout')), bytes > 20 * 1024 * 1024 ? 1800000 : 180000);
  const response = new Promise((resolve, reject) => {
    request.once('response', resolve);
    request.once('error', reject);
  });
  response.catch(() => {});
  let writing;
  try {
    if (upload) {
      writing = pipeline(fs.createReadStream(file), counter(bytes, progress), request, { signal }).then(() => uploaded?.());
      writing.catch(() => {});
    } else request.end();
    const incoming = await response;
    if (incoming.statusCode !== 200) {
      let body = '';
      for await (const chunk of incoming) {
        body += chunk.toString('utf8');
        if (body.length > 8192) { incoming.destroy(); break; }
      }
      let code = `http_${incoming.statusCode}`;
      try { code = JSON.parse(body).error || code; } catch (_) {}
      throw new Error(code);
    }
    if (upload) {
      let body = '';
      for await (const chunk of incoming) {
        body += chunk.toString('utf8');
        if (body.length > 8192) throw new Error('invalid_response');
      }
      await writing;
      if (JSON.parse(body).ok !== true) throw new Error('invalid_response');
    } else {
      if (Number(incoming.headers['content-length']) !== bytes) {
        incoming.destroy(); throw new Error('size_mismatch');
      }
      await pipeline(incoming, counter(bytes, progress), fs.createWriteStream(file, { flags: 'wx' }), { signal });
    }
  } catch (error) {
    request.destroy();
    if (writing) await writing.catch(() => {});
    if (!upload) await fsp.rm(file, { force: true });
    throw error;
  } finally { clearTimeout(deadline); }
}

module.exports = { transferFile, endpoint };
