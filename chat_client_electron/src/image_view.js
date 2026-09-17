// ABOUTME: Renders lazy image thumbnails, persistent send states and an original-image viewer.
// ABOUTME: Cancels obsolete loads and keeps retryable failures local to each image.
const imageErrors = {
  byte_limit: '图片最大 20 MB', pixel_limit: '图片最多 2400 万像素，单边不超过 16000 像素',
  invalid_image: '图片已损坏，无法解码', unsupported_format: '仅支持静态 JPEG 和 PNG',
  processor_unavailable: '图片处理工具未安装', process_failed: '图片处理失败',
  queue_full: '待发送图片最多 20 张，请先处理已有任务', ENOSPC: '本地磁盘空间不足',
  expired: '上传任务已过期', unauthorized: '请重新登录', forbidden: '没有访问这段会话的权限',
  not_found: '图片不存在或无权访问', busy: '正在处理，请稍后重试',
  insecure_endpoint: '图片服务需要 HTTPS 或本地安全隧道', size_mismatch: '图片未完整传输',
  disconnected: '连接已断开，重新登录后可继续', task_store_unreadable: '待发送记录无法读取，请保留数据目录并检查',
};
function imageError(error) { return imageErrors[error] || `操作失败（${error || '连接中断'}），可以重试`; }
function imageNotice(message) {
  const element = document.getElementById('image-notice');
  element.textContent = message; element.hidden = !message;
}

class ImageView {
  constructor() {
    this.cache = new Map(); this.loading = new Set(); this.viewerId = null;
    this.observer = new IntersectionObserver(entries => {
      for (const entry of entries) if (entry.isIntersecting) {
        this.observer.unobserve(entry.target); entry.target.loadThumbnail?.();
      }
    }, { root: document.getElementById('messages'), rootMargin: '200px' });
    this.viewer = document.getElementById('image-viewer');
    this.image = document.getElementById('original-image');
    this.status = document.getElementById('original-status');
    document.getElementById('viewer-close').onclick = () => this.close();
    this.viewer.addEventListener('cancel', event => { event.preventDefault(); this.close(); });
    this.viewer.addEventListener('click', event => { if (event.target === this.viewer) this.close(); });
    document.getElementById('viewer-retry').onclick = () => this.open(this.current, true);
    document.getElementById('viewer-size').onclick = () => {
      this.image.classList.toggle('actual-size');
      document.getElementById('viewer-size').textContent = this.image.classList.contains('actual-size') ? '适应窗口' : '实际大小';
    };
    window.chat.on('progress', value => {
      if (value.requestId !== this.viewerId) return;
      this.status.textContent = value.loaded === value.total ? '图片解码中…'
        : `原图加载 ${Math.min(99, Math.floor(value.loaded / value.total * 100))}%`;
    });
  }
  reset() {
    this.observer.disconnect();
    for (const id of this.loading) window.chat.cancelLoad(id).catch(() => {});
    this.loading.clear();
  }
  close() {
    if (this.viewerId) window.chat.cancelLoad(this.viewerId).catch(() => {});
    this.viewerId = null; this.viewer.close(); this.image.removeAttribute('src');
  }
  async fetch(media, variant, target, force, progressId) {
    const key = `${media.media_id}-${variant}`;
    if (!force && this.cache.has(key)) {
      const cached = new Image(); cached.src = this.cache.get(key);
      try { await cached.decode(); return this.cache.get(key); }
      catch (_) { this.cache.delete(key); }
    }
    const result = await window.chat.load({ requestId: progressId, mediaId: media.media_id, variant, target, force });
    const probe = new Image(); probe.src = result.url;
    try { await probe.decode(); }
    catch (_) { this.cache.delete(key); throw new Error('invalid_image'); }
    if (this.cache.size >= 100) this.cache.delete(this.cache.keys().next().value);
    this.cache.set(key, result.url);
    return result.url;
  }
  bubble(message, target) {
    const element = document.createElement('div'); element.className = 'picture-message';
    const media = message.media;
    const picture = document.createElement('button'); picture.className = 'picture-button';
    const dimensions = message.job?.source || media || { width: 160, height: 120 };
    const width = Math.min(240, Math.max(80, dimensions.width * Math.min(1, 200 / dimensions.height)));
    picture.style.width = `${width}px`;
    picture.style.aspectRatio = `${dimensions.width} / ${dimensions.height}`;
    picture.style.maxHeight = '240px'; picture.style.minHeight = '48px';
    const img = document.createElement('img'); img.alt = '聊天图片'; img.decoding = 'async';
    const hint = document.createElement('span'); hint.className = 'picture-hint';
    hint.textContent = message.job ? '图片处理中…' : '缩略图加载中…';
    picture.append(img, hint); element.append(picture);
    if (message.job) {
      element.dataset.task = message.job.id;
      const state = document.createElement('div'); state.className = 'picture-state'; element.append(state);
      const actions = document.createElement('div'); actions.className = 'picture-actions'; element.append(actions);
      this.updateTask(element, message.job);
    } else {
      const load = async force => {
        const id = crypto.randomUUID(); this.loading.add(id);
        hint.textContent = '缩略图加载中…'; hint.hidden = false;
        picture.onclick = null;
        try {
          const url = await this.fetch(media, 'thumbnail', target, force, id);
          if (!this.loading.has(id) || !element.isConnected) return;
          img.src = url; hint.hidden = true;
          img.onerror = () => {
            hint.textContent = '缩略图读取失败 · 点击重试'; hint.hidden = false;
            picture.onclick = () => load(true);
          };
          picture.onclick = () => this.open({ media, target, thumbnail: url });
        } catch (error) {
          if (!this.loading.has(id) || !element.isConnected) return;
          hint.textContent = '缩略图加载失败 · 点击重试';
          picture.title = imageError(error.message); picture.onclick = () => load(true);
        } finally { this.loading.delete(id); }
      };
      const original = document.createElement('button'); original.className = 'picture-link'; original.textContent = '查看原图';
      original.onclick = () => this.open({ media, target, thumbnail: img.getAttribute('src') });
      element.append(original); picture.loadThumbnail = () => load(false); this.observer.observe(picture);
    }
    return element;
  }
  updateTask(element, job) {
    const img = element.querySelector('img'); const hint = element.querySelector('.picture-hint');
    if (job.preview) { img.src = job.preview; hint.hidden = true; }
    const states = { preparing: '正在检查图片…', requesting: '准备上传…', queued: '等待上传…',
      uploading: `上传 ${job.progress}%`, processing: '上传完成，服务端处理中…',
      confirming: '正在确认发送结果…', paused: '连接已断开 · 待继续', canceling: '正在取消…',
      failed: imageError(job.error), sent: '已发送' };
    element.querySelector('.picture-state').textContent = states[job.state] || job.state;
    const actions = element.querySelector('.picture-actions'); actions.replaceChildren();
    const button = (label, action) => {
      const node = document.createElement('button'); node.textContent = label;
      node.onclick = async () => { node.disabled = true; try { await action(job.id); }
        catch (error) { imageNotice(imageError(error.message)); } finally { node.disabled = false; } };
      actions.append(node);
    };
    if (['failed', 'paused'].includes(job.state)) button('重试', window.chat.retry);
    if (!['sent', 'canceling'].includes(job.state)) button('取消', window.chat.cancel);
  }
  async open(current, force = false) {
    if (!current) return;
    this.close(); this.current = current;
    this.image.classList.remove('actual-size');
    document.getElementById('viewer-size').textContent = '实际大小';
    document.getElementById('viewer-retry').hidden = true;
    if (current.thumbnail) this.image.src = current.thumbnail;
    const id = crypto.randomUUID(); this.viewerId = id;
    this.status.textContent = '正在加载原图…'; this.viewer.showModal();
    try {
      const url = await this.fetch(current.media, 'original', current.target, force, id);
      if (this.viewerId !== id) return;
      this.image.src = url;
      this.status.textContent = `${current.media.width} × ${current.media.height} · ${(current.media.bytes / 1024 / 1024).toFixed(2)} MB`;
    } catch (error) {
      if (this.viewerId !== id) return;
      this.status.textContent = imageError(error.message); document.getElementById('viewer-retry').hidden = false;
    }
  }
}
