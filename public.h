#pragma once

// 公共文件

enum EnMsgType
{
    LoginMsg = 1,       // 登录消息
    LoginMsgAck = 2,    // 登录消息的回应
    RegMsg = 3,         // 注册消息
    RegMsgAck = 4,      // 注册消息的回应
    OTOMsg = 5,         // 私聊消息
    AddFriendMsg = 6,   // 添加好友消息
    CreateGroupMsg = 7, // 创建群组消息
    AddGroupMsg = 8,    // 加入群组消息
    GroupChatMsg = 9,   // 群聊消息
    loginOutMsg = 10,   // 登出消息
    InitMsg = 11,        // 前端界面初始化消息
    InitMsgAck = 12,     // 前端界面初始化消息的回应
    AddFriendMsgAck = 13, // 添加好友消息的回应
    AddGroupMsgAck = 14,  // 加入群组消息的回应
    CreateGroupMsgAck = 15, // 创建群组消息的回应
    HistoryMsg = 16,        // 查看历史消息
    HistoryMsgAck = 17, // 查看历史消息的回应
    RemoveFriendMsg = 18,   // 移除好友消息
    RemoveGroupMsg = 19,    // 移除群组消息
    NewMsg = 20,            // 查询未查看消息数
    NewMsgAck = 21,         // 查询未查看消息数的回应
    addNewMsgCnt = 22,      // 消息数+1
    removeNewMsgCnt = 23,    // 消息数置0
    imageReq = 24,          // 图片请求
    imageReqAck = 25,       // 图片请求的回应
};
