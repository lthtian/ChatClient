// ABOUTME: Runs local client tests using the same Node runtime as the desktop application.
// ABOUTME: Supplies the bundled image helper while keeping real-server integration tests explicit.
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const result = spawnSync(require('electron'), ['--test',
  'tests/message_store.test.js', 'tests/image_cache.test.js', 'tests/file_cache.test.js', 'tests/tcp.test.js',
  'tests/image_transfer.test.js', 'tests/image_processor.test.js'], {
  cwd: path.join(__dirname, '..'), stdio: 'inherit',
  env: { ...process.env, ELECTRON_RUN_AS_NODE: '1',
    CHAT_IMAGE_TOOL: process.env.CHAT_IMAGE_TOOL || path.join(__dirname, '..', 'resources', 'native', 'chat_image.exe') },
});
if (result.error) { process.stderr.write(result.error.message + '\n'); process.exit(1); }
process.exit(result.status === null ? 1 : result.status);
