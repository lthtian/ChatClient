// ========== 配置 ==========
const SERVER_HOST = '39.105.18.142';
const SERVER_PORT = 7000;

// ========== 消息类型（和服务端的 public.h 一一对应）==========
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

// 响应类型的 msgid（这些由 sendAndWait 处理，不走通用 handler）
const RESPONSE_TYPES = new Set([
  MsgType.LoginMsgAck, MsgType.RegMsgAck, MsgType.InitMsgAck,
  MsgType.HistoryMsgAck, MsgType.NewMsgAck, MsgType.imageReqAck,
  MsgType.AddFriendMsgAck, MsgType.AddGroupMsgAck, MsgType.CreateGroupMsgAck,
]);

// ========== 应用状态 ==========
const state = {
  userId: -1,
  userName: '',
  isLoginMode: true,
  contacts: new Map(),  // name → { id, isGroup, unread }
  messages: new Map(),   // name → [{ text, time, isMine, senderName }]
  currentChat: null,
};

// ========== TCP 客户端 ==========
const tcp = new TcpClient();

// ========== DOM 元素 ==========
const $ = (id) => document.getElementById(id);
const loginView   = $('login-view');
const chatView    = $('chat-view');
const loginTitle  = $('login-title');
const usernameIn  = $('username');
const passwordIn  = $('password');
const loginBtn    = $('login-btn');
const switchMode  = $('switch-mode');
const contactList = $('contact-list');
const chatHeader  = $('chat-header');
const messagesDiv = $('messages');
const msgInput    = $('msg-input');
const sendBtn     = $('send-btn');
const userInfo    = $('user-info');

// ========== 连接服务器 ==========
async function init() {
  loginTitle.textContent = '正在连接服务器...';
  loginBtn.disabled = true;
  usernameIn.disabled = true;
  passwordIn.disabled = true;

  try {
    await tcp.connect(SERVER_HOST, SERVER_PORT);
    loginTitle.textContent = '登录';
    loginBtn.disabled = false;
    usernameIn.disabled = false;
    passwordIn.disabled = false;
    usernameIn.focus();
  } catch (err) {
    loginTitle.textContent = '连接失败: ' + err.message;
  }
}

// ========== 登录 / 注册 ==========
async function handleLogin() {
  const username = usernameIn.value.trim();
  const password = passwordIn.value.trim();

  if (!username || !password) {
    loginTitle.textContent = '输入不能为空';
    return;
  }

  loginBtn.disabled = true;
  loginTitle.textContent = '请稍候...';

  const msgId  = state.isLoginMode ? MsgType.LoginMsg : MsgType.RegMsg;
  const ackId  = state.isLoginMode ? MsgType.LoginMsgAck : MsgType.RegMsgAck;

  try {
    const resp = await tcp.sendAndWait({ msgid: msgId, username, password }, ackId, 5000);

    if (resp.errno === 0) {
      if (state.isLoginMode) {
        loginTitle.textContent = '登录成功';
        state.userId = resp.id;
        state.userName = resp.name;
        showChatView();
        await loadContacts();
      } else {
        loginTitle.textContent = '注册成功，请登录';
        state.isLoginMode = true;
        switchMode.textContent = '没有账号? 去注册';
      }
    } else {
      loginTitle.textContent = resp.errmsg || '操作失败';
      usernameIn.value = '';
      passwordIn.value = '';
    }
  } catch (err) {
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
      MsgType.InitMsgAck,
      5000
    );

    // 解析好友列表
    if (resp.friends) {
      for (const friendStr of resp.friends) {
        const f = JSON.parse(friendStr);
        state.contacts.set(f.name, { id: f.id, isGroup: false, unread: 0 });
      }
    }

    // 解析群组列表
    if (resp.groups) {
      for (const groupStr of resp.groups) {
        const g = JSON.parse(groupStr);
        state.contacts.set(g.groupname, { id: g.id, isGroup: true, unread: 0 });
      }
    }

    renderContacts();

    // 异步查询每个联系人的未读消息数（不阻塞 UI）
    for (const [name, info] of state.contacts) {
      queryUnread(name, info);
    }
  } catch (err) {
    console.error('加载联系人失败:', err);
  }
}

async function queryUnread(name, info) {
  try {
    const resp = await tcp.sendAndWait(
      { msgid: MsgType.NewMsg, userid: state.userId, sender: info.id, name, isgroup: info.isGroup },
      MsgType.NewMsgAck,
      3000
    );
    if (resp.cnt > 0) {
      info.unread = resp.cnt;
      renderContacts();
    }
  } catch (_) { /* 超时不影响使用 */ }
}

// ========== 聊天功能 ==========
async function loadHistory(name) {
  const info = state.contacts.get(name);
  if (!info) return;

  state.messages.set(name, []);

  const req = { msgid: MsgType.HistoryMsg };
  if (!info.isGroup) {
    req.isgroup = false;
    req.id1 = state.userId;
    req.id2 = info.id;
  } else {
    req.isgroup = true;
    req.groupid = info.id;
  }

  try {
    const resp = await tcp.sendAndWait(req, MsgType.HistoryMsgAck, 5000);

    if (resp.history) {
      const msgs = [];
      for (const histStr of resp.history) {
        const h = JSON.parse(histStr);
        msgs.push({
          text: h.message,
          time: h.time,
          isMine: String(h.id) === String(state.userId),
          senderName: h.name,
        });
      }
      state.messages.set(name, msgs);
    }

    // 清除未读计数
    tcp.sendJson({
      msgid: MsgType.removeNewMsgCnt,
      userid: state.userId,
      sender: info.id,
      isgroup: info.isGroup,
    });
    info.unread = 0;
    renderContacts();
    renderMessages();
  } catch (err) {
    console.error('加载历史消息失败:', err);
  }
}

function sendMessage() {
  const text = msgInput.value.trim();
  if (!text || !state.currentChat) return;

  const info = state.contacts.get(state.currentChat);
  if (!info) return;

  // 立即显示在界面上
  const msgs = state.messages.get(state.currentChat) || [];
  msgs.push({ text, time: new Date().toLocaleString(), isMine: true, senderName: state.userName });
  state.messages.set(state.currentChat, msgs);
  renderMessages();
  msgInput.value = '';

  // 发送给服务器（不需要等响应，fire-and-forget）
  if (!info.isGroup) {
    tcp.sendJson({
      msgid: MsgType.OTOMsg,
      id: state.userId,
      sender: state.userName,
      to: info.id,
      message: text,
    });
  } else {
    tcp.sendJson({
      msgid: MsgType.GroupChatMsg,
      userid: state.userId,
      sendername: state.userName,
      groupid: info.id,
      groupname: state.currentChat,
      message: text,
    });
  }
}

// ========== 接收推送消息 ==========
tcp.on('message', (msg) => {
  // 响应类型的消息由 sendAndWait 处理，这里只处理推送消息
  if (RESPONSE_TYPES.has(msg.msgid)) return;

  switch (msg.msgid) {
    case MsgType.OTOMsg:
      handleIncomingChat(msg, false);
      break;
    case MsgType.GroupChatMsg:
      handleIncomingChat(msg, true);
      break;
  }
});

function handleIncomingChat(msg, isGroup) {
  const name = isGroup ? msg.groupname : msg.sender;
  const senderName = isGroup ? msg.sendername : msg.sender;

  if (state.currentChat !== name) {
    // 不在当前聊天窗口，增加未读计数
    const info = state.contacts.get(name);
    if (info) {
      info.unread = (info.unread || 0) + 1;
      renderContacts();
    }
    tcp.sendJson({
      msgid: MsgType.addNewMsgCnt,
      userid: state.userId,
      sender: state.contacts.get(name)?.id,
      isgroup: isGroup,
    });
    return;
  }

  // 在当前聊天窗口，直接显示
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

  // 有未读消息的排前面
  const sorted = [...state.contacts.entries()].sort((a, b) => {
    if (a[1].unread > 0 && b[1].unread === 0) return -1;
    if (a[1].unread === 0 && b[1].unread > 0) return 1;
    return 0;
  });

  for (const [name, info] of sorted) {
    const li = document.createElement('li');
    if (state.currentChat === name) li.classList.add('active');

    const iconClass = info.isGroup ? 'group' : 'person';
    const iconText  = info.isGroup ? '群' : name[0];

    li.innerHTML = `
      <span class="contact-name">
        <span class="contact-icon ${iconClass}">${iconText}</span>
        ${name}
      </span>
      ${info.unread > 0 ? `<span class="unread-badge">${info.unread}</span>` : ''}
    `;

    li.addEventListener('click', () => {
      state.currentChat = name;
      chatHeader.textContent = name;
      renderContacts();
      loadHistory(name);
    });

    contactList.appendChild(li);
  }
}

function renderMessages() {
  messagesDiv.innerHTML = '';
  const msgs = state.messages.get(state.currentChat) || [];

  for (const msg of msgs) {
    const div = document.createElement('div');
    div.className = `message ${msg.isMine ? 'sent' : 'received'}`;

    // 群聊中显示发送者姓名
    const info = state.contacts.get(state.currentChat);
    if (!msg.isMine && info?.isGroup && msg.senderName) {
      const sender = document.createElement('div');
      sender.className = 'sender';
      sender.textContent = msg.senderName;
      div.appendChild(sender);
    }

    const text = document.createElement('div');
    text.textContent = msg.text;
    div.appendChild(text);
    messagesDiv.appendChild(div);
  }

  // 滚动到底部
  messagesDiv.scrollTop = messagesDiv.scrollHeight;
}

// ========== 事件绑定 ==========
loginBtn.addEventListener('click', handleLogin);

usernameIn.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') passwordIn.focus();
});

passwordIn.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') handleLogin();
});

switchMode.addEventListener('click', (e) => {
  e.preventDefault();
  state.isLoginMode = !state.isLoginMode;
  loginTitle.textContent = state.isLoginMode ? '登录' : '注册';
  switchMode.textContent = state.isLoginMode ? '没有账号? 去注册' : '已注册? 去登录';
});

sendBtn.addEventListener('click', sendMessage);

msgInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    sendMessage();
  }
});

// ========== 启动 ==========
init();
