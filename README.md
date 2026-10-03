# ChatClient

Qt 5 聊天客户端，支持发送 MP4、从实时消息或历史记录在线播放，也可下载完整文件、通过 SHA-256 检查后本地播放。文件上限 100 MiB，私聊和群聊共用媒体协议。

## 运行

2026-10-03 已完成公网直连切换，并在本地 SSH 隧道关闭后通过实际视频收发和播放验证。

直接启动 `build/Desktop_Qt_5_15_2_MinGW_64_bit-Debug/debug/ChatClient.exe`，不需要 SSH 隧道。登录并选择好友或群聊，点击“发送 MP4”。视频卡片提供“在线播放”和“下载并播放”。缓存存在时后者显示“播放”，鼠标停留可查看本地路径。

“在线播放”由 FFmpeg 通过带授权的 HTTP Range 读取，不创建本地视频文件；窗口显示在线/本地模式。支持暂停、跳转、网络重缓冲及凭证过期后恢复当前位置。无音频输出设备时仍显示视频，状态栏提示无声播放；重新打开视频会再次检测设备。

视频缓存保存在 `%LOCALAPPDATA%/cache/ChatClient/videos/<SHA-256>.mp4`，本机对应 `C:/Users/lds/AppData/Local/cache/ChatClient/videos`，退出客户端后保留。每次播放仍向服务器确认读取权限，随后分块校验本地文件的长度和摘要；缓存有效则直接播放，缺失或损坏才重新下载。不提供离线授权或断点续传。需要清理磁盘时，可在关闭播放器后删除缓存中的 MP4，后续点击会重新下载。

- 聊天：`39.105.18.142:7000`，经服务器 Nginx 转发到聊天服务。
- 文件：`http://39.105.18.142/media`，经服务器 Nginx 转发到 `127.0.0.1:6002`；媒体存储目录不作为静态网站公开。
- 服务器的云安全组需要放行入方向 TCP 7000 和 80。

`CHAT_HOST`、`CHAT_PORT` 可覆盖聊天入口；`CHAT_MEDIA_URL` 可覆盖客户端明确允许的公网 HTTP 文件地址，需填完整 `/media` 路径，并与服务端返回值一致。采用 HTTP 是当前学习联调的明确选择，文件和授权凭证明文传输。

## 构建

Qt Creator 使用 Qt 5.15.2 / MinGW 8.1 / x64 Kit 打开 `ChatClient.pro`。当前播放器使用 Qt 5 的 QAudioOutput 接口。

- 播放器源码位于本项目的 `player/` 目录，主工程和联调工程都通过相对路径引用。
- FFmpeg 开发包默认目录：`D:/chat/_deps/ffmpeg-8.1.1-full_build-shared`，可用环境变量 `FFMPEG_ROOT` 修改。
- `player/media_playback.pri` 配置播放器源码、Qt Multimedia 和 FFmpeg 依赖；客户端构建不需要服务端项目目录。

播放器模块：`media_window` 负责播放窗口和调度，`media_decoder` 负责解封装、解码及格式转换，`audio_output` 负责音频设备和 PCM 输出，`video_view` 负责画面绘制。`media_source` 明确区分本地路径和 HTTP 描述，读取差异集中在 `MediaDecoder::OpenInput`；`MediaTransfer::requestPlaybackSource` 负责聊天授权。客户端的播放器修改在此目录完成。

在现有 Debug 构建目录，首次接入或修改 `.pro/.pri` 后运行 qmake，再增量编译；仅修改 C++ 文件时直接运行 make。以下为 CMD 命令：

```bat
set "PATH=F:\Qt\Tools\mingw810_64\bin;F:\Qt\5.15.2\mingw81_64\bin;C:\Windows\System32;C:\Windows"
F:\Qt\5.15.2\mingw81_64\bin\qmake.exe -o Makefile ../../ChatClient.pro -spec win32-g++ CONFIG+=debug CONFIG+=qml_debug
F:\Qt\Tools\mingw810_64\bin\mingw32-make.exe -j4 debug
F:\Qt\5.15.2\mingw81_64\bin\windeployqt.exe --debug --compiler-runtime debug\ChatClient.exe
copy D:\chat\_deps\ffmpeg-8.1.1-full_build-shared\bin\*.dll debug\
```

构建目录的 `debug` 文件夹包含可执行文件、Qt/FFmpeg DLL 和 Qt 插件，启动时保留整套目录。在线播放需要服务器运行支持 Range 的版本；当前公网服务已部署。服务端仍在 Windows 本机 WSL 编译 Linux 产物后上传，不在小内存服务器编译。

## 必要联调

`tests/video_chat_smoke.pro` 是独立的真实服务联调入口，使用 QtTest 等待界面事件，不包含模拟服务。运行时会注册两个专用账号并互加好友，然后验证 MP4 上传、实时接收、下载摘要、本地播放、在线播放/seek/不落盘、历史恢复、文字消息，以及弹窗关闭、缓存复用和损坏恢复。缓存校验启用 QStandardPaths 测试模式，只修改 `qttest/cache/ChatClient/videos` 下的专用文件。

在单独的构建目录用相同 Qt Kit 构建此 `.pro`，配置好 Qt/FFmpeg 运行库后，运行 `video_chat_smoke.exe <完整MP4路径>`。默认连接公网 7000，HTTP 使用服务端返回的地址。它会写出 `stage6-test-data.json` 记录本次创建的账号编号，以及两张界面截图。测试账号及已发布文件保留在服务端，重复执行会产生额外测试数据。

2026-10-02 验证记录：

- `ChatClient / Debug / MinGW x64`：**Full build passed**，最终增量编译 8.77 秒。命令为 `cmd.exe /d /c "F:\Qt\Tools\mingw810_64\bin\mingw32-make.exe -j4 debug"`，工作目录及 PATH 如上；完整输出保存在 `stage6-build-final.log`。EXE 已更新为 18,813,912 字节，PE Machine 为 0x8664。
- `D:\test_video.mp4`：真实服务器收发、下载 SHA-256、画面播放、暂停 seek、历史恢复和文字消息通过，联调耗时 4.18 秒。
- 双声道 Sintel 样本：上传、推送、下载及摘要通过；当前系统没有可枚举音频输出设备，实际有声播放等待连接设备后补验。
- 既有代码仍有成员初始化顺序、未使用参数和 Qt 弃用信号警告；新增媒体模块未产生编译警告。

2026-10-03 直连改动：`ChatClient / Debug / MinGW x64` **Full build passed**，增量 14.88 秒，EXE 18,815,102 字节、PE 0x8664。沿用上述 make 命令，输出为构建目录 `direct-build.log`；联调程序构建通过 9.18 秒。

关闭本地 SSH 隧道、确认 16000/16001 没有监听后，使用 `D:\test_video.mp4` 经公网 7000/80 验证上传、实时接收、下载 SHA-256、画面播放、暂停 seek、历史恢复及文字消息，6.03 秒通过。日志为 `build/video-chat-smoke/run-direct.log`；Nginx 记录对应 PUT/GET 均为 200，下载响应 561704 字节。

2026-10-03 传输界面与缓存修正：`ChatClient / Debug / MinGW x64` **Full build passed**，7.48 秒；`video_chat_smoke / Debug / MinGW x64` 构建通过，7.64 秒。均在各自已有构建目录使用上述受控 PATH 执行 `cmd.exe /d /c "F:\Qt\Tools\mingw810_64\bin\mingw32-make.exe -j4 debug"`，日志为 `cache-layout-build.log`，本次编译无警告。公网真实联调 5.53 秒通过，日志为 `build/video-chat-smoke/run-cache-layout.log`；包含成功和取消时进度窗口隐藏、重复播放不发起下载且文件修改时间不变、等长度损坏缓存重新下载、1200×800 与 800×600 布局截图检查。


2026-10-03 第七阶段验证：

- ChatClient / Debug / MinGW x64：Full build passed，最终增量 9.93 秒，`stage7-final-build.log`。EXE 19,098,506 字节，PE 0x8664，2026-10-03 14:07:51；Qt 插件及 FFmpeg DLL 已部署。构建命令沿用上述受控 PATH 下的 `cmd.exe /d /c "F:\Qt\Tools\mingw810_64\bin\mingw32-make.exe -j4 debug"`。
- 原无音轨 MP4 及带 AAC 双声道的 Sintel 均通过公网在线播放、定位和原下载缓存流程。耳机已被 Qt 识别，实际设备输出路径与时钟通过；自动验证不代替人耳听感判断。
- `--expiry` 真实等待 302 秒，验证恢复播放时刷新授权并保留位置。`--no-audio-device` 仅禁用测试进程后续 Qt 插件搜索，验证真实 Qt 无音频后端时继续无声播放，不改变 Windows 设备。
- `tests/http_media_relay.py <实际媒体URL> <暂停标记文件> <地址输出文件>` 转发真实服务字节，不模拟服务数据。设置 `CHAT_TEST_RELAY` 为输出地址、`CHAT_TEST_GATE` 为同一标记文件，再运行联调程序，可验证断粮、时钟冻结、恢复以及阻塞 seek 的取消。逐请求状态、Range 和已发送字节写入地址文件同目录的 `.requests.jsonl`，不记录凭证。
- `run-stage7-public.log`、`run-stage7-network.log`、`run-stage7-relay-audio.log` 保存对应结果。取消阻塞读取时的两条 FFmpeg 中断/partial file 诊断已单独捕获并匹配确认；其后本地输入正常起播。首帧前实际传输 563,744 / 4,372,373 字节，验证未收完整文件即起播。
