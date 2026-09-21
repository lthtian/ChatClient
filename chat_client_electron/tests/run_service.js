// ABOUTME: Runs client integration checks against an explicitly supplied isolated server.
// ABOUTME: Uses the Electron runtime for storage tests and a real desktop window for rendering.
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const electron = require('electron');
const port = process.argv[2], output = process.argv[3], mode = process.argv[4] || 'online';
const scripts = mode === 'offline' ? ['offline_window.test.js']
  : ['image_client.test.js', 'message_sync.test.js', 'file_client.test.js', 'image_window.test.js'];
for (const script of scripts) {
  const env = { ...process.env };
  if (script.includes('window')) delete env.ELECTRON_RUN_AS_NODE;
  else env.ELECTRON_RUN_AS_NODE = '1';
  const args = [path.join(__dirname, script), port];
  if (script.includes('window')) args.push(output);
  const result = spawnSync(electron, args, { env, stdio: 'inherit', timeout: 120000 });
  if (result.error) { process.stderr.write(result.error.message + '\n'); process.exit(1); }
  if (result.status !== 0) process.exit(result.status || 1);
}
