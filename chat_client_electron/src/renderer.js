// ABOUTME: Renders chat views and handles account and conversation interactions.
// ABOUTME: Connects the Electron interface to the chat server over TCP.
// ========== 消息类型（和服务端 public.h 一一对应）==========
const MsgType = {
  LoginMsg: 1, LoginMsgAck: 2,
  RegMsg: 3, RegMsgAck: 4,
  OTOMsg: 5, AddFriendMsg: 6,
  CreateGroupMsg: 7, AddGroupMsg: 8,
  GroupChatMsg: 9, loginOutMsg: 10,
  InitMsg: 11, InitMsgAck: 12,
  AddFriendMsgAck: 13, AddGroupMsgAck: 14,
  CreateGroupMsgAck: 15,
  HistoryMsg: 16, HistoryMsgAck: 17,
  RemoveFriendMsg: 18, RemoveGroupMsg: 19,
  NewMsg: 20, NewMsgAck: 21,
  addNewMsgCnt: 22, removeNewMsgCnt: 23,
  imageReq: 24, imageReqAck: 25,
};

// sendAndWait 负责处理的响应类型，通用 handler 跳过
const RESPONSE_TYPES = new Set([
  MsgType.LoginMsgAck, MsgType.RegMsgAck, MsgType.InitMsgAck,
  MsgType.HistoryMsgAck, MsgType.NewMsgAck, 27,
  MsgType.AddFriendMsgAck, MsgType.AddGroupMsgAck, MsgType.CreateGroupMsgAck,
]);

// ========== 应用状态 ==========
const state = {
  userId: -1,
  userName: '',
  isLoginMode: true,
  contacts: new Map(),   // name → { id, isGroup, unread }
  avatars: new Map(),     // name → data:image/jpeg;base64,...
  messages: new Map(),    // name → [{ text, time, isMine, senderName }]
  currentChat: null,
  dialogOP: -1,           // 1=加好友 2=加群 3=建群
  contextTarget: null,    // 右键菜单目标联系人
  regAvatar: '',          // 注册时选择的头像 base64 数据
  jobs: new Map(),
  cursors: new Map(),
  connected: false,
  historyRequest: 0,
};

// ========== TCP 客户端 ==========
const tcp = new TcpClient();

// ========== DOM ==========
const $ = (id) => document.getElementById(id);
const loginView    = $('login-view');
const chatView     = $('chat-view');
const loginTitle   = $('login-title');
const usernameIn   = $('username');
const passwordIn   = $('password');
const loginBtn     = $('login-btn');
const switchMode   = $('switch-mode');
const userInfo     = $('user-info');
const contactList  = $('contact-list');
const chatHeader   = $('chat-header');
const messagesDiv  = $('messages');
const msgInput     = $('msg-input');
const sendBtn      = $('send-btn');
// 弹窗
const dialogOverlay = $('dialog-overlay');
const dialogTitle   = $('dialog-title');
const dialogError   = $('dialog-error');
const dialogInput   = $('dialog-input');
const dialogConfirm = $('dialog-confirm');
const dialogClose   = $('dialog-close');
// 右键菜单
const contextMenu = $('context-menu');
const ctxRemove   = $('ctx-remove');
// 注册头像
const avatarSelector = $('avatar-selector');
const avatarPreview  = $('avatar-preview');
const avatarInput    = $('avatar-input');
const imageView = new ImageView();

// ========== 图片压缩（注册头像）==========
function compressImage(file, maxSize, quality) {
  return new Promise((resolve) => {
    const reader = new FileReader();
    reader.onload = (e) => {
      const img = new Image();
      img.onload = () => {
        const canvas = document.createElement('canvas');
        let w = img.width, h = img.height;
        if (w > maxSize || h > maxSize) {
          if (w > h) { h = h * maxSize / w; w = maxSize; }
          else { w = w * maxSize / h; h = maxSize; }
        }
        canvas.width = w;
        canvas.height = h;
        canvas.getContext('2d').drawImage(img, 0, 0, w, h);
        const base64 = canvas.toDataURL('image/jpeg', quality).split(',')[1];
        resolve(base64);
      };
      img.src = e.target.result;
    };
    reader.readAsDataURL(file);
  });
}

function resetAvatarSelector() {
  state.regAvatar = '';
  avatarPreview.classList.remove('has-image');
  const prev = avatarPreview.querySelector('img');
  if (prev) prev.remove();
  avatarInput.value = '';
}

avatarPreview.addEventListener('click', () => avatarInput.click());
avatarInput.addEventListener('change', async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  state.regAvatar = await compressImage(file, 200, 0.85);
  let img = avatarPreview.querySelector('img');
  if (!img) {
    img = document.createElement('img');
    avatarPreview.insertBefore(img, avatarPreview.firstChild);
  }
  img.src = `data:image/jpeg;base64,${state.regAvatar}`;
});

// ========== 连接服务器 ==========
async function init() {
  loginTitle.textContent = '正在连接服务器...';
  loginBtn.disabled = true;
  usernameIn.disabled = true;
  passwordIn.disabled = true;

  try {
    await tcp.connect();
    state.connected = true;
    loginTitle.textContent = '登录';
    loginBtn.disabled = false;
    usernameIn.disabled = false;
    passwordIn.disabled = false;
    usernameIn.focus();
  } catch (err) {
    loginTitle.textContent = '连接失败: ' + err.message;
    loginBtn.disabled = false;
    usernameIn.disabled = false;
    passwordIn.disabled = false;
  }
}

// ========== 登录 / 注册 ==========
async function handleLogin() {
  const username = usernameIn.value.trim();
  const password = passwordIn.value.trim();
  if (!username || !password) { loginTitle.textContent = '输入不能为空'; return; }

  loginBtn.disabled = true;
  loginTitle.textContent = '请稍候...';

  const msgId = state.isLoginMode ? MsgType.LoginMsg : MsgType.RegMsg;
  const ackId = state.isLoginMode ? MsgType.LoginMsgAck : MsgType.RegMsgAck;

  const payload = { msgid: msgId, username, password };
  if (!state.isLoginMode && state.regAvatar) {
    payload.avatar = state.regAvatar;
  }

  try {
    if (!state.connected) { await tcp.connect(); state.connected = true; }
    const resp = await tcp.sendAndWait(payload, ackId, 5000);
    if (resp.errno === 0) {
      if (state.isLoginMode) {
        if (state.userId !== Number(resp.id)) {
          state.contacts.clear(); state.messages.clear(); state.avatars.clear(); state.jobs.clear();
          state.cursors.clear(); state.currentChat = null;
          messagesDiv.replaceChildren(); imageView.cache.clear();
          chatHeader.textContent = '选择一个联系人开始聊天';
        }
        state.userId = parseInt(resp.id);
        state.userName = resp.name;
        loginTitle.textContent = '登录成功';
        showChatView();
        await loadContacts();
        for (const job of await window.chat.jobs()) state.jobs.set(job.id, job);
        renderImageTasks();
        if (state.currentChat) renderMessages();
      } else {
        loginTitle.textContent = '注册成功，请登录';
        state.isLoginMode = true;
        switchMode.textContent = '没有账号? 去注册';
        avatarSelector.style.display = 'none';
        resetAvatarSelector();
      }
    } else {
      loginTitle.textContent = resp.errmsg || '操作失败';
      usernameIn.value = '';
      passwordIn.value = '';
    }
  } catch (_) {
    loginTitle.textContent = '请求超时，请重试';
  }
  loginBtn.disabled = false;
}

// ========== 加载联系人 ==========
async function loadContacts() {
  userInfo.textContent = `${state.userName} (${state.userId})`;

  try {
    const resp = await tcp.sendAndWait(
      { msgid: MsgType.InitMsg, id: state.userId },
      MsgType.InitMsgAck, 5000
    );

    state.contacts.clear();
    if (resp.friends) {
      for (const s of resp.friends) {
        const f = JSON.parse(s);
        state.contacts.set(f.name, { id: parseInt(f.id), isGroup: false, unread: 0 });
        requestAvatar(f.name, parseInt(f.id));
      }
    }
    if (resp.groups) {
      for (const s of resp.groups) {
        const g = JSON.parse(s);
        state.contacts.set(g.groupname, { id: parseInt(g.id), isGroup: true, unread: 0 });
      }
    }
    renderContacts();

    // 异步查询未读消息数
    for (const [name, info] of state.contacts) {
      queryUnread(name, info);
    }
  } catch (err) {
    console.error('加载联系人失败:', err);
  }

  // 也请求自己的头像
  requestAvatar(state.userName, state.userId);
}

async function queryUnread(name, info) {
  try {
    const resp = await tcp.sendAndWait(
      { msgid: MsgType.NewMsg, userid: state.userId, sender: info.id, name, isgroup: info.isGroup },
      MsgType.NewMsgAck, 3000
    );
    if (resp.cnt > 0) { info.unread = resp.cnt; renderContacts(); }
  } catch (_) {}
}

// ========== 聊天 ==========
async function loadHistory(name, older = false) {
  const info = state.contacts.get(name);
  if (!info) return;
  const request = ++state.historyRequest;
  const previous = state.messages.get(name) || [];
  const startCount = previous.length;
  $('history-status').textContent = '加载中…';
  $('history-more').disabled = true;
  if (!older) renderMessages(true);

  try {
    const resp = await window.chat.history({ conversation: { is_group: info.isGroup, target: info.id },
      before_id: older ? Number(state.cursors.get(name) || 0) : 0, limit: 50 });
    if (request !== state.historyRequest || state.currentChat !== name) return;
    const height = messagesDiv.scrollHeight;
    const top = messagesDiv.scrollTop;
    const arrived = (state.messages.get(name) || []).slice(startCount);
    const messages = resp.messages.map(chatMessage);
    const combined = older ? [...messages, ...(state.messages.get(name) || [])] : [...messages, ...arrived];
    const ids = new Set();
    state.messages.set(name, combined.filter(message => {
      if (!message.message_id) return true;
      if (ids.has(message.message_id)) return false;
      ids.add(message.message_id); return true;
    }));
    state.cursors.set(name, resp.next_cursor);
    $('history-more').hidden = !resp.next_cursor;
    $('history-more').textContent = '加载更早消息';
    $('history-more').onclick = () => loadHistory(name, true);
    $('history-status').textContent = '';
    // 清除未读计数
    tcp.sendJson({ msgid: MsgType.removeNewMsgCnt, userid: state.userId, sender: info.id, isgroup: info.isGroup });
    info.unread = 0;
    renderContacts();
    renderMessages(!older);
    if (older) messagesDiv.scrollTop = top + messagesDiv.scrollHeight - height;
  } catch (err) {
    if (request !== state.historyRequest) return;
    $('history-status').textContent = '历史消息加载失败';
    $('history-more').hidden = false;
    $('history-more').textContent = '重试';
    $('history-more').onclick = () => loadHistory(name, older);
  } finally {
    if (request === state.historyRequest) $('history-more').disabled = false;
  }
}

function chatMessage(message) {
  return { ...message, time: new Date(message.time).toLocaleString(),
    isMine: Number(message.sender_id) === state.userId, senderName: message.sender_name };
}

function conversationName(target) {
  return [...state.contacts].find(([, info]) => info.id === target.target && info.isGroup === target.is_group)?.[0];
}

function receiveImage(message, target) {
  const name = conversationName(target);
  if (!name) return;
  const messages = state.messages.get(name) || [];
  if (messages.some(value => value.message_id === message.message_id)) return;
  messages.push(chatMessage(message)); state.messages.set(name, messages);
  if (state.currentChat === name) renderMessages(Number(message.sender_id) === state.userId);
  else if (Number(message.sender_id) !== state.userId) {
    const info = state.contacts.get(name); info.unread++;
    tcp.sendJson({ msgid: MsgType.addNewMsgCnt, userid: state.userId, sender: info.id, isgroup: info.isGroup });
    renderContacts();
  }
}

window.chat.on('task', job => {
  if (job.state === 'sent' || job.state === 'canceled') {
    state.jobs.delete(job.id);
    renderImageTasks();
    if (job.message) receiveImage(job.message, job.conversation);
    if (state.currentChat === conversationName(job.conversation)) renderMessages();
    return;
  }
  state.jobs.set(job.id, job);
  renderImageTasks();
  if (state.currentChat !== conversationName(job.conversation)) return;
  const element = messagesDiv.querySelector(`[data-task="${job.id}"]`);
  if (element) imageView.updateTask(element, job);
  else renderMessages(true);
});
window.chat.on('image-error', error => imageNotice(imageError(error)));
window.addEventListener('chat-error', event => imageNotice(imageError(event.detail)));
tcp.on('disconnected', () => {
  state.connected = false; state.historyRequest++;
  imageView.reset(); imageView.close();
  $('image-task-dialog').close();
  imageView.cache.clear();
  chatView.style.display = 'none'; loginView.style.display = 'flex';
  loginTitle.textContent = '连接已断开，请重新登录；图片任务已保留';
  loginBtn.disabled = false;
});
$('image-send').onclick = async () => {
  const info = state.contacts.get(state.currentChat);
  if (!info) { imageNotice('先选择一个联系人或群聊'); return; }
  try { imageNotice(''); await window.chat.pick({ is_group: info.isGroup, target: info.id }); }
  catch (error) { imageNotice(imageError(error.message)); }
};

function renderImageTasks() {
  $('image-tasks').textContent = state.jobs.size ? `待发送 ${state.jobs.size}` : '待发送';
  if (!$('image-task-dialog').open) return;
  const list = $('image-task-list'); list.replaceChildren();
  if (!state.jobs.size) { list.textContent = '没有未完成的图片任务'; return; }
  for (const job of state.jobs.values()) {
    const item = document.createElement('div'); item.className = 'image-task-item';
    const title = document.createElement('p');
    title.textContent = `${conversationName(job.conversation) || `会话 ${job.conversation.target}`} · ${job.name}`;
    item.append(title, imageView.bubble({ job }, job.conversation)); list.append(item);
  }
}
$('image-tasks').onclick = () => { $('image-task-dialog').showModal(); renderImageTasks(); };
$('image-tasks-close').onclick = () => $('image-task-dialog').close();

function sendMessage() {
  const text = msgInput.value.trim();
  if (!text || !state.currentChat) return;
  const info = state.contacts.get(state.currentChat);
  if (!info) return;

  const msgs = state.messages.get(state.currentChat) || [];
  msgs.push({ text, time: new Date().toLocaleString(), isMine: true, senderName: state.userName });
  state.messages.set(state.currentChat, msgs);
  renderMessages();
  msgInput.value = '';

  if (!info.isGroup) {
    tcp.sendJson({ msgid: MsgType.OTOMsg, id: state.userId, sender: state.userName, to: info.id, message: text });
  } else {
    tcp.sendJson({ msgid: MsgType.GroupChatMsg, userid: state.userId, sendername: state.userName, groupid: info.id, groupname: state.currentChat, message: text });
  }
}

// ========== 头像 ==========
function requestAvatar(name, id) {
  tcp.sendJson({ msgid: MsgType.imageReq, userid: id, username: name });
}

function handleImageReqAck(msg) {
  if (msg.isSuccess === 'false' || !msg.image_data) return;
  const dataUrl = `data:image/jpeg;base64,${msg.image_data}`;
  state.avatars.set(msg.username, dataUrl);

  // 如果有自己的新头像，更新联系人列表显示
  renderContacts();
  // 如果当前聊天窗口有消息，刷新消息以更新头像
  if (state.currentChat) renderMessages();
}

// ========== 弹窗（加好友/加群/建群）==========
function showDialog(op) {
  state.dialogOP = op;
  dialogError.textContent = '';
  dialogInput.value = '';
  if (op === 1) {
    dialogTitle.textContent = '添加好友';
    dialogInput.placeholder = '请输入好友用户名';
  } else if (op === 2) {
    dialogTitle.textContent = '加入群组';
    dialogInput.placeholder = '请输入群组名称';
  } else if (op === 3) {
    dialogTitle.textContent = '创建群组';
    dialogInput.placeholder = '请输入群组名称';
  }
  dialogOverlay.style.display = 'flex';
  dialogInput.focus();
}

function hideDialog() {
  dialogOverlay.style.display = 'none';
  state.dialogOP = -1;
}

async function handleDialogConfirm() {
  const name = dialogInput.value.trim();
  if (!name) return;
  dialogConfirm.disabled = true;
  dialogError.textContent = '';

  let req, ackId;
  if (state.dialogOP === 1) {
    req = { msgid: MsgType.AddFriendMsg, id: state.userId, friendname: name };
    ackId = MsgType.AddFriendMsgAck;
  } else if (state.dialogOP === 2) {
    req = { msgid: MsgType.AddGroupMsg, userid: state.userId, groupname: name, role: 'normal' };
    ackId = MsgType.AddGroupMsgAck;
  } else if (state.dialogOP === 3) {
    req = { msgid: MsgType.CreateGroupMsg, userid: state.userId, groupname: name };
    ackId = MsgType.CreateGroupMsgAck;
  } else {
    dialogConfirm.disabled = false;
    return;
  }

  try {
    const resp = await tcp.sendAndWait(req, ackId, 5000);
    if (resp.errno === 0) {
      if (state.dialogOP === 1) {
        state.contacts.set(resp.friendname, { id: parseInt(resp.friendid), isGroup: false, unread: 0 });
        requestAvatar(resp.friendname, parseInt(resp.friendid));
      } else {
        state.contacts.set(resp.groupname, { id: parseInt(resp.groupid), isGroup: true, unread: 0 });
      }
      renderContacts();
      hideDialog();
    } else {
      dialogError.textContent = resp.errmsg || '操作失败';
    }
  } catch (_) {
    dialogError.textContent = '请求超时';
  }
  dialogConfirm.disabled = false;
}

// ========== 右键菜单（删除好友/退群）==========
function showContextMenu(e, name) {
  e.preventDefault();
  state.contextTarget = name;
  const info = state.contacts.get(name);
  ctxRemove.textContent = info.isGroup ? '退出/移除群聊' : '删除好友';
  contextMenu.style.display = 'block';
  contextMenu.style.left = e.clientX + 'px';
  contextMenu.style.top = e.clientY + 'px';
}

function hideContextMenu() {
  contextMenu.style.display = 'none';
  state.contextTarget = null;
}

function handleRemove() {
  const name = state.contextTarget;
  const info = state.contacts.get(name);
  if (!info) { hideContextMenu(); return; }

  if (!info.isGroup) {
    tcp.sendJson({ msgid: MsgType.RemoveFriendMsg, userid: state.userId, friendid: info.id });
  } else {
    tcp.sendJson({ msgid: MsgType.RemoveGroupMsg, userid: state.userId, groupid: info.id });
  }

  state.contacts.delete(name);
  state.messages.delete(name);
  if (state.currentChat === name) {
    state.currentChat = null;
    chatHeader.textContent = '选择一个联系人开始聊天';
    messagesDiv.innerHTML = '';
  }
  renderContacts();
  hideContextMenu();
}

// ========== 接收推送消息 ==========
tcp.on('message', (msg) => {
  if (RESPONSE_TYPES.has(msg.msgid)) return;

  switch (msg.msgid) {
    case 28:
      receiveImage(msg.message, msg.conversation);
      break;
    case MsgType.OTOMsg:
      handleIncomingChat(msg, false);
      break;
    case MsgType.GroupChatMsg:
      handleIncomingChat(msg, true);
      break;
    case MsgType.imageReqAck:
      handleImageReqAck(msg);
      break;
  }
});

function handleIncomingChat(msg, isGroup) {
  const name = isGroup ? msg.groupname : msg.sender;
  const senderName = isGroup ? msg.sendername : msg.sender;

  if (state.currentChat !== name) {
    const info = state.contacts.get(name);
    if (info) {
      info.unread = (info.unread || 0) + 1;
      renderContacts();
    }
    tcp.sendJson({ msgid: MsgType.addNewMsgCnt, userid: state.userId, sender: info?.id, isgroup: isGroup });
    return;
  }

  const msgs = state.messages.get(name) || [];
  msgs.push({ text: msg.message, time: new Date().toLocaleString(), isMine: false, senderName });
  state.messages.set(name, msgs);
  renderMessages();
}

// ========== UI 渲染 ==========
function showChatView() {
  loginView.style.display = 'none';
  chatView.style.display = 'flex';
}

function renderContacts() {
  contactList.innerHTML = '';
  const sorted = [...state.contacts.entries()].sort((a, b) => {
    if (a[1].unread > 0 && b[1].unread === 0) return -1;
    if (a[1].unread === 0 && b[1].unread > 0) return 1;
    return 0;
  });

  for (const [name, info] of sorted) {
    const li = document.createElement('li');
    if (state.currentChat === name) li.classList.add('active');
    li.dataset.name = name;

    // 头像
    const avatar = document.createElement('div');
    avatar.className = `contact-avatar ${info.isGroup ? 'group' : 'person'}`;
    if (state.avatars.has(name)) {
      avatar.style.backgroundImage = `url(${state.avatars.get(name)})`;
    } else {
      avatar.textContent = info.isGroup ? '群' : name[0];
    }

    const nameSpan = document.createElement('span');
    nameSpan.className = 'contact-name';
    nameSpan.textContent = name;

    const left = document.createElement('div');
    left.className = 'contact-left';
    left.appendChild(avatar);
    left.appendChild(nameSpan);

    li.appendChild(left);

    if (info.unread > 0) {
      const badge = document.createElement('span');
      badge.className = 'unread-badge';
      badge.textContent = info.unread;
      li.appendChild(badge);
    }

    // 左键点击 → 打开聊天
    li.addEventListener('click', () => {
      state.currentChat = name;
      chatHeader.textContent = name;
      renderContacts();
      loadHistory(name);
    });

    // 右键 → 显示菜单
    li.addEventListener('contextmenu', (e) => showContextMenu(e, name));

    contactList.appendChild(li);
  }
}

function renderMessages(forceBottom = false) {
  const top = messagesDiv.scrollTop;
  const stickBottom = forceBottom || messagesDiv.scrollHeight - top - messagesDiv.clientHeight < 100;
  imageView.reset();
  messagesDiv.innerHTML = '';
  const msgs = [...(state.messages.get(state.currentChat) || [])];
  const contactInfo = state.contacts.get(state.currentChat);
  for (const job of state.jobs.values()) {
    if (conversationName(job.conversation) === state.currentChat &&
        !msgs.some(message => message.client_msg_id === job.id)) {
      msgs.push({ kind: 'image', job, time: new Date(job.createdAt).toLocaleString(),
        isMine: true, senderName: state.userName });
    }
  }

  let lastDate = '';
  let lastTime = 0;

  for (const msg of msgs) {
    // 时间分隔线
    const msgDate = msg.time ? msg.time.split(' ')[0] : '';
    const msgTimeStr = msg.time ? msg.time.split(' ')[1] : '';
    const msgTimestamp = msg.time ? new Date(msg.time).getTime() : 0;

    // 不同日期 → 显示日期
    if (msgDate && msgDate !== lastDate) {
      const sep = document.createElement('div');
      sep.className = 'msg-time-sep';
      sep.textContent = msgDate;
      messagesDiv.appendChild(sep);
      lastDate = msgDate;
      lastTime = msgTimestamp;
    } else if (msgTimestamp - lastTime >= 10 * 60 * 1000 && msgTimeStr) {
      // 同日超过10分钟 → 显示时间
      const sep = document.createElement('div');
      sep.className = 'msg-time-sep';
      sep.textContent = msgTimeStr;
      messagesDiv.appendChild(sep);
      lastTime = msgTimestamp;
    }

    // 消息行
    const row = document.createElement('div');
    row.className = `msg-row ${msg.isMine ? 'right' : 'left'}`;

    // 头像
    const avatar = document.createElement('div');
    const avatarName = msg.isMine ? state.userName : msg.senderName;
    avatar.className = `msg-avatar ${msg.isMine ? 'sent-bg' : 'recv-bg'}`;
    if (state.avatars.has(avatarName)) {
      avatar.style.backgroundImage = `url(${state.avatars.get(avatarName)})`;
    } else {
      avatar.textContent = avatarName ? avatarName[0] : '?';
    }

    // 内容区
    const body = document.createElement('div');
    body.className = 'msg-body';

    // 群聊显示发送者
    if (!msg.isMine && contactInfo?.isGroup && msg.senderName) {
      const sender = document.createElement('div');
      sender.className = 'msg-sender';
      sender.textContent = msg.senderName;
      body.appendChild(sender);
    }

    const bubble = msg.kind === 'image'
      ? imageView.bubble(msg, { is_group: contactInfo.isGroup, target: contactInfo.id })
      : document.createElement('div');
    if (msg.kind !== 'image') {
      bubble.className = `msg-bubble ${msg.isMine ? 'sent' : 'received'}`;
      bubble.textContent = msg.text;
    }
    body.appendChild(bubble);

    // 组装：自己的消息头像在右，对方在左
    if (msg.isMine) {
      row.appendChild(body);
      row.appendChild(avatar);
    } else {
      row.appendChild(avatar);
      row.appendChild(body);
    }

    messagesDiv.appendChild(row);
  }

  messagesDiv.scrollTop = stickBottom ? messagesDiv.scrollHeight : top;
}

// ========== 事件绑定 ==========
loginBtn.addEventListener('click', handleLogin);
usernameIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') passwordIn.focus(); });
passwordIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') handleLogin(); });
switchMode.addEventListener('click', (e) => {
  e.preventDefault();
  state.isLoginMode = !state.isLoginMode;
  loginTitle.textContent = state.isLoginMode ? '登录' : '注册';
  switchMode.textContent = state.isLoginMode ? '没有账号? 去注册' : '已注册? 去登录';
  avatarSelector.style.display = state.isLoginMode ? 'none' : 'flex';
  if (!state.isLoginMode) resetAvatarSelector();
});

sendBtn.addEventListener('click', sendMessage);
msgInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendMessage(); }
});

// 侧边栏按钮
$('btn-add-friend').addEventListener('click', () => showDialog(1));
$('btn-add-group').addEventListener('click', () => showDialog(2));
$('btn-create-group').addEventListener('click', () => showDialog(3));
$('btn-exit').addEventListener('click', () => window.close());

// 弹窗
dialogConfirm.addEventListener('click', handleDialogConfirm);
dialogClose.addEventListener('click', hideDialog);
dialogInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') handleDialogConfirm(); });
dialogOverlay.addEventListener('click', (e) => { if (e.target === dialogOverlay) hideDialog(); });

// 右键菜单
ctxRemove.addEventListener('click', handleRemove);
document.addEventListener('click', hideContextMenu);

// ========== 启动 ==========
init();
