// ABOUTME: Displays opaque file messages and persistent send task states.
// ABOUTME: Keeps download progress across message rerenders and saves only through the main process.
class FileView {
  constructor() {
    this.transfers = new Map();
    window.chat.on('progress', value => {
      for (const transfer of this.transfers.values()) if (transfer.requestId === value.requestId) {
        transfer.text = value.total ? `下载 ${Math.floor(value.loaded / value.total * 100)}%` : '下载完成';
        this.refresh(transfer);
      }
    });
  }
  size(bytes) { return bytes >= 1048576 ? `${(bytes / 1048576).toFixed(2)} MB` : `${bytes} 字节`; }
  reset() {
    for (const transfer of this.transfers.values()) if (transfer.active) window.chat.cancelLoad(transfer.requestId).catch(() => {});
    this.transfers.clear();
  }
  refresh(transfer) {
    for (const element of document.querySelectorAll('.file-message')) {
      if (element.dataset.media !== transfer.mediaId) continue;
      element.querySelector('.file-status').textContent = transfer.text;
      element.querySelector('.file-save').disabled = transfer.active;
      element.querySelector('.file-cancel').hidden = !transfer.active;
    }
  }
  bubble(message, target) {
    const job = message.job, media = message.media;
    const element = document.createElement('div'); element.className = 'file-message';
    const title = document.createElement('div'); title.className = 'file-name';
    title.textContent = job ? job.name : media.name;
    const size = document.createElement('div'); size.className = 'file-size';
    size.textContent = this.size(job ? job.bytes : media.bytes);
    element.append(title, size);
    if (job) {
      element.dataset.task = job.id;
      const status = document.createElement('div'); status.className = 'picture-state';
      const actions = document.createElement('div'); actions.className = 'picture-actions';
      element.append(status, actions); this.updateTask(element, job); return element;
    }
    element.dataset.media = media.media_id;
    let transfer = this.transfers.get(media.media_id);
    if (!transfer) {
      transfer = { mediaId: media.media_id, active: false, text: '' };
      this.transfers.set(media.media_id, transfer);
    }
    const status = document.createElement('div'); status.className = 'file-status'; status.textContent = transfer.text;
    const save = document.createElement('button'); save.className = 'file-save picture-link';
    save.textContent = '另存为'; save.disabled = transfer.active;
    const cancel = document.createElement('button'); cancel.className = 'file-cancel picture-link';
    cancel.textContent = '取消下载'; cancel.hidden = !transfer.active;
    cancel.onclick = () => window.chat.cancelLoad(transfer.requestId).catch(error => imageNotice(imageError(error.message)));
    save.onclick = async () => {
      transfer.active = true; transfer.requestId = crypto.randomUUID(); transfer.text = '选择保存位置…';
      this.refresh(transfer);
      try {
        const result = await window.chat.saveFile({ requestId: transfer.requestId, mediaId: media.media_id, target });
        transfer.text = result.canceled ? '' : `已保存：${result.name}`;
      } catch (error) { transfer.text = error.message === 'AbortError' || error.message.includes('aborted')
        ? '下载已取消，可重新保存' : imageError(error.message); }
      finally { transfer.active = false; this.refresh(transfer); }
    };
    element.append(status, save, cancel); return element;
  }
  updateTask(element, job) {
    const states = { preparing: '正在准备文件…', queued: '等待上传…', requesting: '准备上传…',
      uploading: `上传 ${job.progress}%`, processing: '上传完成，服务端校验中…', confirming: '正在确认发送结果…',
      confirmed: '已发送，缓存待整理', failed: imageError(job.error), paused: '连接已断开 · 待继续',
      canceling: '正在取消…', sent: '已发送' };
    element.querySelector('.picture-state').textContent = states[job.state] || job.state;
    renderTaskActions(element, job);
  }
}
