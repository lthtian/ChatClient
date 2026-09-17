// ABOUTME: Connects renderer conversation actions to the preload chat API.
// ABOUTME: Provides event subscriptions without exposing Node or raw sockets.
class TcpClient {
  connect() { return window.chat.connect(); }
  on(event, handler) { return window.chat.on(event, handler); }
  sendJson(value) { window.chat.send(value).catch(error => window.dispatchEvent(new CustomEvent('chat-error', { detail: error.message }))); }
  sendAndWait(value) { return window.chat.request(value); }
}
