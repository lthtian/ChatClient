# ChatClient

Qt 5 聊天客户端，支持分享 MP4/MOV、MKV、WebM 等独立视频文件，从实时消息或历史记录在线播放，也可下载完整文件、通过 SHA-256 检查后本地播放。文件上限 100 MiB，私聊和群聊共用媒体协议。容器与编码按内容识别，能否实际播放由当前 FFmpeg 构建和文件内容决定。

## 运行

2026-10-03 已完成公网直连切换，并在本地 SSH 隧道关闭后通过实际视频收发和播放验证。

本分支产物位于 `build/rtc/debug/ChatClient.exe`。登录并选择好友或群聊，点击“发送视频”。客户端在后台检查视频轨道与首帧，再计算摘要并上传；服务器保存独立探测结果后发布消息。卡片显示容器、视频编码、尺寸与平均帧率，并提供“在线播放”和“下载并播放”。缓存存在时后者显示“播放”，鼠标停留可查看本地路径。

此协议需要配套部署本分支 ChatServer 与 media_process。描述随原件保存，并与 SHA-256 绑定；历史缺少描述的文件显示“请重新上传”，不会自动回填。普通文件仍保留原件下载能力，音频封面不作为视频轨道。

播放器按默认标记和实际初始化结果选择轨道，提供音轨菜单及“轨道信息”。切换音轨保留位置和播放/暂停状态，可能短暂重新缓冲；无可用音轨时自动无声播放并持续提示原因。语言和标题来自容器，未设置系统语言优先规则。相关设计见[解码与轨道选择](D:/chat_server/ChatServer/docs/音视频学习/优化02-解码与轨道选择.md)。

“在线播放”由 FFmpeg 通过带授权的 HTTP Range 读取，不创建本地视频文件；窗口显示在线/本地模式。支持暂停、跳转、网络重缓冲及凭证过期后恢复当前位置。无音频输出设备时仍显示视频，状态栏提示无声播放；重新打开视频会再次检测设备。

视频缓存保存在 `%LOCALAPPDATA%/cache/ChatClient/videos/<SHA-256>.media`，本机对应 `C:/Users/lds/AppData/Local/cache/ChatClient/videos`，退出客户端后保留。每次播放仍向服务器确认读取权限与媒体描述，随后分块校验本地文件的长度和摘要；缓存有效则直接播放，缺失或损坏才重新下载。不提供离线授权或断点续传。需要清理磁盘时，可在关闭播放器后删除缓存文件，后续点击会重新下载。

- 聊天：`39.105.18.142:7000`，经服务器 Nginx 转发到聊天服务。
- 文件：`http://39.105.18.142/media`，经服务器 Nginx 转发到 `127.0.0.1:6002`；媒体存储目录不作为静态网站公开。
- 服务器的云安全组需要放行入方向 TCP 7000 和 80。

`CHAT_HOST`、`CHAT_PORT` 可覆盖聊天入口；`CHAT_MEDIA_URL` 可覆盖客户端明确允许的公网 HTTP 文件地址，需填完整 `/media` 路径，并与服务端返回值一致。采用 HTTP 是当前学习联调的明确选择，文件和授权凭证明文传输。

## 构建

Qt Creator 使用 Qt 5.15.2 / MinGW 8.1 / x64 Kit 打开 `ChatClient.pro`。当前播放器使用 Qt 5 的 QAudioOutput 接口。

- 播放器源码位于本项目的 `player/` 目录，主工程和联调工程都通过相对路径引用。
- FFmpeg 开发包默认目录：`D:/chat/_deps/ffmpeg-8.1.1-full_build-shared`，可用环境变量 `FFMPEG_ROOT` 修改。
- `player/media_playback.pri` 配置播放器源码、Qt Multimedia 和 FFmpeg 依赖；客户端构建不需要服务端项目目录。

播放器模块：`media_window` 负责窗口和播放控制，`media_decoder` 负责文件/HTTP/HLS 输入、解封装和定位；`track_decoder` 完成单轨道解码及 RGB/PCM 转换，`playback` 管理缓存、播放时钟和到期画面，后两者也用于 RTC 观看。`audio_output` 负责音频设备和 PCM 输出，`video_view` 负责画面绘制。`media_source` 描述文件和 HTTP 输入，读取差异集中在 `MediaDecoder::OpenInput`；`MediaTransfer::requestPlaybackSource` 负责聊天授权。

在现有 Debug 构建目录，首次接入或修改 `.pro/.pri` 后运行 qmake，再增量编译；仅修改 C++ 文件时直接运行 make。以下为 CMD 命令：

```bat
set "PATH=F:\Qt\Tools\mingw810_64\bin;F:\Qt\5.15.2\mingw81_64\bin;C:\Windows\System32;C:\Windows"
F:\Qt\5.15.2\mingw81_64\bin\qmake.exe -o Makefile ../../ChatClient.pro -spec win32-g++ CONFIG+=debug CONFIG+=qml_debug
F:\Qt\Tools\mingw810_64\bin\mingw32-make.exe -j4 debug
F:\Qt\5.15.2\mingw81_64\bin\windeployqt.exe --debug --compiler-runtime debug\ChatClient.exe
copy D:\chat\_deps\ffmpeg-8.1.1-full_build-shared\bin\*.dll debug\
```

构建目录的 `debug` 文件夹包含可执行文件、Qt/FFmpeg DLL 和 Qt 插件，启动时保留整套目录。在线播放需要服务器运行支持 Range 的版本；当前公网服务已部署。服务端仍在 Windows 本机 WSL 编译 Linux 产物后上传，不在小内存服务器编译。

服务端对单非封面视频轨、至多单音轨的 MOV 家族输入排队执行现有 HLS 处理器；多音轨、多视频轨和 MKV/WebM 等走原件播放，HLS 状态为 not_applicable，保留原件轨道。处理期间播放器显示“原件播放 · 清晰度处理中”，完成后重新打开视频可选择已生成的档位。处理失败不影响原件下载，播放器会提示失败。客户端下载按钮始终取得原始文件；recognized 表示有视频轨，不保证整段文件解码成功。

## 必要联调

桌面直播：选择好友/群聊后点“创建直播”，选择显示器（默认主屏），点击“开始直播”自动申请凭证、采集、编码并推流。输出 1280×720 / 30fps、H.264 + AAC、两秒 GOP；电脑声音与默认麦克风分别开关。麦克风默认关闭，可在开始前或直播中开启；一路设备不可用会提示并继续发送其他媒体。卡片变成“直播中”后可选择“观看直播”（HTTP-FLV）或“HLS 观看”。

直播卡片的“桌面直播”可打开控制窗口并重新开播；失败显示原因，重试自动申请凭证。点击“结束直播”或关闭控制窗口会停止设备、网络发布并结束服务端业务记录。关闭聊天窗口先结束直播。模块位于 `broadcast/`，直接链接 FFmpeg 编码库与 Windows WASAPI，不调用 9.1 EXE、OBS 或 FFmpeg 命令行。

直播暂停会关闭接收，恢复重新加入当前直播；不提供进度 seek。推流中断后有限重试，发布者“结束”后停止接收。客户端只保留播放缓存，不保存直播文件。推流端到云端还需要安全组开放 TCP1935；观看沿用 HTTP80，聊天沿用7000。`live_chat.cpp` 管理卡片，`live_transfer.cpp` 申请直播源，播放器用 `LiveFlv/LiveHls` 与点播明确分支。

`tests/live_playback_smoke.pro` 复用真实聊天授权和播放器，验证两种直播格式、暂停重新加入、零 seek、状态结束；配合服务端真实 fixture 验证断流后重推恢复，不使用模拟后端。服务端链路与客户端链路的知识分别记录在阶段9.2文档。

`tests/video_chat_smoke.pro` 是独立的真实服务联调入口，使用 QtTest 等待界面事件，不包含模拟服务。运行时会注册两个专用账号并互加好友，然后验证视频上传、描述驱动的实时卡片、下载摘要、本地播放、在线播放/seek/不落盘、历史恢复与播放、文字消息，以及弹窗关闭、缓存复用和损坏恢复。缓存校验启用 QStandardPaths 测试模式，只修改 `qttest/cache/ChatClient/videos` 下的专用文件。

在单独的构建目录用相同 Qt Kit 构建此 `.pro`，配置好 Qt/FFmpeg 运行库后，运行 `video_chat_smoke.exe <完整视频路径>`。使用 CHAT_HOST/CHAT_PORT 指向测试服务，HTTP 使用服务端返回的地址。服务端 `tests/media_chat_capabilities_test.py` 在本机隔离 MySQL/Redis/存储环境中驱动七个样本，结束时清理测试数据；结果在 `build/video-chat-smoke/chat-samples`。直接连接其他服务器执行时，测试账号和已发布文件会保留在该服务器。

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
