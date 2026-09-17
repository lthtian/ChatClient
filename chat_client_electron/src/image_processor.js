// ABOUTME: Runs the C++ image task from the Electron main process.
// ABOUTME: Returns thumbnail metadata through a bounded child process contract.
const { execFile } = require('node:child_process');
const path = require('node:path');

function failure(code, diagnostics = '') {
  const error = new Error(code);
  error.code = code;
  error.diagnostics = diagnostics;
  return error;
}

async function processImage({ toolPath, sourcePath, outputDirectory, signal }) {
  if (signal?.aborted) throw failure('aborted');
  if (![toolPath, sourcePath, outputDirectory].every(
    value => typeof value === 'string' && path.isAbsolute(value))) {
    throw failure('invalid_argument');
  }
  return new Promise((resolve, reject) => {
    execFile(toolPath, [sourcePath, outputDirectory, 'thumbnail'], {
      shell: false,
      windowsHide: true,
      encoding: 'utf8',
      timeout: 30000,
      maxBuffer: 64 * 1024,
      signal,
    }, (error, stdout, stderr) => {
      if (signal?.aborted) return reject(failure('aborted'));
      if (error?.code === 'ENOENT') return reject(failure('processor_unavailable'));
      if (error?.killed) return reject(failure('process_timeout', stderr));
      let result;
      try {
        result = JSON.parse(stdout);
      } catch (_) {
        return reject(failure('process_failed', stderr));
      }
      if (error) {
        const allowed = ['invalid_argument', 'io_error', 'invalid_image',
          'unsupported_format', 'byte_limit', 'pixel_limit'];
        return reject(failure(allowed.includes(result?.error) ? result.error : 'process_failed', stderr));
      }
      const dimensions = [result?.source?.width, result?.source?.height,
        result?.thumbnail?.width, result?.thumbnail?.height];
      const mimeTypes = ['image/jpeg', 'image/png'];
      if (!dimensions.every(value => Number.isSafeInteger(value) && value > 0) ||
          !mimeTypes.includes(result?.source?.mime) ||
          !mimeTypes.includes(result?.thumbnail?.mime) ||
          result?.thumbnail?.key !== 'thumbnail' ||
          !Number.isSafeInteger(result?.thumbnail?.bytes) || result.thumbnail.bytes <= 0) {
        return reject(failure('process_failed', stderr));
      }
      resolve(result);
    });
  });
}

module.exports = { processImage };
