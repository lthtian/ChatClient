// ABOUTME: Verifies the image process contract using the compiled C++ executable.
// ABOUTME: Checks real files, structured errors and cancellation before execution.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { processImage } = require('../src/image_processor');

const fixture = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGPgUbLwA2EABYEBaWcDN6YAAAAASUVORK5CYII=',
  'base64'
);
const toolPath = process.env.CHAT_IMAGE_TOOL;
assert.ok(toolPath, 'CHAT_IMAGE_TOOL must point to the compiled C++ executable');

async function setup(t) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'chat-image-process-'));
  t.after(() => fs.rm(directory, { recursive: true, force: true }));
  const sourcePath = path.join(directory, '图片 & sample.png');
  await fs.writeFile(sourcePath, fixture);
  return { toolPath, sourcePath, outputDirectory: path.join(directory, 'cache') };
}

test('runs the image executable with Unicode and shell characters in its path', async (t) => {
  const input = await setup(t);
  const result = await processImage(input);
  assert.deepEqual(result.source, { width: 2, height: 1, mime: 'image/png' });
  assert.equal(result.thumbnail.key, 'thumbnail');
  assert.equal(result.thumbnail.mime, 'image/png');
  const file = await fs.readFile(path.join(input.outputDirectory, 'objects', 'thumbnail'));
  assert.equal(file.length, result.thumbnail.bytes);
  assert.deepEqual(file.subarray(0, 8), fixture.subarray(0, 8));
});

test('returns the native validation error for damaged input', async (t) => {
  const input = await setup(t);
  await fs.writeFile(input.sourcePath, Buffer.from([0xff, 0xd8, 0xff, 0xe0, 0, 16]));
  await assert.rejects(processImage(input), { code: 'invalid_image' });
});

test('keeps native decoder diagnostics out of JSON parsing', async (t) => {
  const input = await setup(t);
  const damaged = Buffer.from(fixture);
  damaged[45] ^= 0xff;
  await fs.writeFile(input.sourcePath, damaged);
  await assert.rejects(processImage(input), (error) => {
    assert.equal(error.code, 'invalid_image');
    assert.match(error.diagnostics, /libpng error:/);
    return true;
  });
});

test('does not start an already canceled task', async (t) => {
  const input = await setup(t);
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(processImage({ ...input, signal: controller.signal }),
    { code: 'aborted' });
  await assert.rejects(fs.stat(input.outputDirectory), { code: 'ENOENT' });
});

test('reports a missing executable explicitly', async (t) => {
  const input = await setup(t);
  await assert.rejects(processImage({ ...input, toolPath: path.join(input.outputDirectory, 'missing') }),
    { code: 'processor_unavailable' });
});
