// ABOUTME: Publishes a bounded chat API to the isolated renderer.
// ABOUTME: Keeps sockets, filesystem paths and Electron objects in the main process.
const { contextBridge, ipcRenderer } = require('electron');
async function invoke(action, value) {
  const response = await ipcRenderer.invoke('chat:invoke', action, value);
  if (!response.ok) throw new Error(response.error);
  return response.data;
}
contextBridge.exposeInMainWorld('chat', {
  connect: () => invoke('connect'),
  request: value => invoke('request', value),
  send: value => invoke('send', value),
  history: value => invoke('history', value),
  jobs: () => invoke('jobs'),
  pick: target => invoke('pick', target),
  retry: id => invoke('retry', id),
  cancel: id => invoke('cancel', id),
  load: value => invoke('load', value),
  cancelLoad: id => invoke('cancelLoad', id),
  on: (event, callback) => {
    const listener = (_event, type, value) => { if (type === event) callback(value); };
    ipcRenderer.on('chat:event', listener);
    return () => ipcRenderer.removeListener('chat:event', listener);
  },
});
