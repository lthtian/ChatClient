// ABOUTME: Creates the desktop chat window with an isolated renderer.
// ABOUTME: Installs trusted main-process chat and image services.
const { app, BrowserWindow } = require('electron');
const path = require('path');
const { installBridge } = require('./src/desktop_bridge');

function createWindow() {
  const win = new BrowserWindow({
    width: 1200,
    height: 800,
    minWidth: 800,
    minHeight: 600,
    autoHideMenuBar: true,
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: true,
      preload: path.join(__dirname, 'preload.js')
    }
  });

  installBridge(win);
  win.loadFile(path.join(__dirname, 'src', 'index.html'));
}

app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
  app.quit();
});
