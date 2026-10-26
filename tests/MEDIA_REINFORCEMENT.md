# 播放器加固实验入口

这里记录阶段 1/2 的构建、真实素材实验和结果口径。详细知识正文固定在：

- `D:/chat_server/ChatServer/docs/音视频学习/加固01-格式能力与正确显示.md`
- `D:/chat_server/ChatServer/docs/音视频学习/加固02-时间线时钟与缓冲.md`

代码来自本仓库 `codex/media-reinforcement`。`VideoGeometry` 处理 SAR 和正交显示方向；`MediaWindow` 与 `AudioOutput` 提供默认关闭的播放观测。RTC 共用的 `VideoPicture` 默认几何为单位变换，原始摄像头的翻转处理保持在采集路径。

## 1. 构建

使用项目原有 Qt 5.15.2 / MinGW 8.1 x64 / FFmpeg 8.1.1 shared。以下为普通 CMD 命令，在仓库根目录执行。实验目标只编译真实播放器与少量实验代码，不启动业务服务。

```bat
set "PATH=F:\Qt\Tools\mingw810_64\bin;F:\Qt\5.15.2\mingw81_64\bin;D:\chat\_deps\ffmpeg-8.1.1-full_build-shared\bin;C:\Windows\System32;C:\Windows"
mkdir build\media-reinforcement
cd build\media-reinforcement
qmake -o Makefile ../../tests/media_reinforcement.pro -spec win32-g++ CONFIG+=debug CONFIG-=release
mingw32-make -j2 debug
```

主客户端现有输出目录为 `build/rtc`，共享模块清单 `.pri` 变化后执行一次 qmake，再 `mingw32-make -j2 debug`。`build/rtc-smoke` 同理使用 `../../tests/rtc_smoke.pro`。普通 C++ 编辑直接增量 make。

本机曾发生多个独立 C++ 构建并行时的 `cc1plus.exe: out of memory allocating 65536 bytes`。该次命令失败，所有编译子进程自然退出；其他构建结束后，原参数增量编译剩余对象与链接通过。它不属于源码编译错误，也没有更改工具链、项目输出路径或终止其他程序。内存紧张时按目标顺序构建。

## 2. 素材与几何/时间实验

素材生成入口在后端仓库的 `lessons/media_reinforcement/generate_samples.py`，其 README 记录参数。当前实际样例目录：

```text
C:/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/media_reinforcement/build/samples
```

目录包含 `base.mp4/base.mkv`、`sar_4_3.mp4`、`rotate_90/180/270.mp4`、`rotate_90_sar.mp4`、`mirror.mp4`、`rotate_45.mp4`、`audio_late.mp4`、`audio_gap.mkv` 和 `vfr_bframes.mp4`。输出目录保存 manifest、实际元数据与生成命令。素材来自合成图形和合成声音，不使用个人录音录像。

从仓库根目录运行，保留上节 PATH：

```bat
set "QT_QPA_PLATFORM=offscreen"
set "QT_PLUGIN_PATH=F:\Qt\5.15.2\mingw81_64\plugins"
set "PYTHON=C:\Users\lds\AppData\Local\hermes\hermes-agent\venv\Scripts\python.exe"
build\media-reinforcement\debug\media_reinforcement.exe C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer\lessons\media_reinforcement\build\samples build\media-reinforcement\results
```

`offscreen` 使用真实 Qt 画布绘制而不弹出实验窗口。它不模拟解码器；输入仍经真实 FFmpeg 打开、解码、重采样。需要观察交互窗口时可在专用 CMD 中清除 `QT_QPA_PLATFORM` 后运行。

该入口执行以下代表验证：

- 完整解码正常素材，确认解码尾部排空、视频 PTS 不倒退，记录音频块和时间间隔。
- 800×600 画布的实际截图，计算有色区域边界并核对四角色块；不只检查“有图像”。
- 45 度矩阵触发预期的明确不支持错误；错误写入 `result.json`，不当作其他用例的通过条件。
- 检查 RTC 三字段 `VideoPicture` 聚合及 `SetImage(image)` 默认方向；完整 RTC 网络复验由后端真实 RTC 联调负责。
- 使用真实 `MediaWindow` 播放，验证暂停位置冻结、暂停 seek 到 2.3 秒、恢复、快速重新打开及 EOF 消费。
- 音轨晚起、音频时间缺口和 VFR/B 帧输入；对实际解码得到的 PCM/PTS 做判断。

结果包括 `result.json`、`timeline.jsonl` 及八张 PNG。重复运行 JSONL 会追加，因此对比独立运行时使用不同结果目录。

## 3. HTTP 断供与取消

`player_network_lab.py` 启动一个只绑定 127.0.0.1 的真实静态 Range 源站，以及已有 `http_media_relay.py` 中转，再运行同一 Qt 实验程序。HTTP 正文来自指定 MP4 的实际字节；本实验不接入业务授权，`X-Lab` 头仅满足实验输入描述，不表示测试过 ChatServer 鉴权。

```bat
"%PYTHON%" tests\player_network_lab.py build\media-reinforcement\debug\media_reinforcement.exe C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer\lessons\media_reinforcement\build\samples\base.mp4 build\media-reinforcement\network-results
```

运行过程为：起播 → 创建 gate 暂停真实响应正文 → 等待已有缓冲耗尽 → 检查媒体时钟冻结 → 删除 gate 恢复 → 阻塞读取期间 seek/重新打开 → 检查新代次正常起播。

gate 只暂停交付，不模拟丢包；已经进入 FFmpeg 或 TCP 缓冲的数据仍会继续消费。故应等到播放器实际进入 `Buffering`，不能把创建 gate 的时刻当作已经断粮。

源站和中转使用系统分配端口。脚本只结束自己创建的中转进程、关闭自己的源站，并移除自己的 gate；不会停止聊天客户端或正式服务。选择新的结果目录，已有 `relay.txt` 或 `pause.gate` 时入口会拒绝混用实验状态。

输出为 `player.log`、`relay.log`、`network.jsonl`、`origin.json`、`relay.requests.jsonl`、`diagnostics.json` 和最终 `result.json`。结束时先解除 gate，再关闭实验实例，避免留下阻塞的请求线程。

## 4. HLS 播放与定位

后端媒体实验生成的 TS/fMP4 单档目录可以由同一入口提供真实 HTTP 文件：

```bat
"%PYTHON%" tests\player_network_lab.py build\media-reinforcement\debug\media_reinforcement.exe C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer\lessons\media_reinforcement\build\run-20261022\hls-ts\480p build\media-reinforcement\hls-ts-results --hls
"%PYTHON%" tests\player_network_lab.py build\media-reinforcement\debug\media_reinforcement.exe C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer\lessons\media_reinforcement\build\run-20261022\hls-fmp4\480p build\media-reinforcement\hls-fmp4-results --hls
```

`PYTHON` 是本机实际 Python 解释器路径；其他机器可改为已安装的 Python 3。

`--hls` 的输入是含 `index.m3u8` 的目录。脚本仅在本地提供该目录，实际播放列表、初始化资源和分片经 HTTP 交给 FFmpeg；验证起播、暂停 seek 到 2.3 秒及恢复至排空。它不生成业务授权，不验证 CDN 缓存或访问控制，后者属于分发实验。

## 5. 在普通播放器中打开观测

```bat
set "CHAT_MEDIA_TRACE=E:\chat_server_qt\ChatClient-rtc\build\player-trace.jsonl"
build\rtc\debug\ChatClient.exe
```

在单独 CMD 中设置变量，只影响从该环境启动的进程。父目录需已存在；不能打开文件时提示并继续正常播放器逻辑。清除变量再启动即可关闭记录。实验程序自行指定结果目录中的日志。

普通状态约 500 ms 采样一次，关键事件立即记录。8 MiB 上限按文件大小计算，已有文件接近限额时可能很快停止追加。多窗口共同记录时用 `window` 与 `session` 区分；应用内多个窗口不保证共享文件写入的跨窗口统一事务语义，分开启动实验更便于阅读。

重要字段见知识正文第 9 节。JSONL 中没有 URL、文件名、授权头或票据。`video_presented_pts_us` 是交给画布的时间，`audio.position_estimate_us` 是设备消费估计，二者都不是物理屏幕/扬声器测量值。

## 6. 本机验证记录

2026-10-22，Debug / MinGW x64：

| 目标 | 构建结果与耗时 | 日志 |
|---|---|---|
| ChatClient | Full build passed；最终增量 34.7459304 秒 | `build/rtc/reinforcement-final-build.log` |
| rtc_smoke | Full build passed；31.371589 秒 | `build/rtc-smoke/reinforcement-build.log` |
| media_reinforcement | Full build passed；内存压力解除后剩余对象与链接 8.2529115 秒；最终实验入口增量 8.459081 秒 | `build/media-reinforcement/build-resume.log`、`build-hls.log` |

主程序 39,538,183 字节，RTC 实验程序 24,106,247 字节，PE Machine 均为 `0x8664`。最终主程序与 RTC 构建日志没有 warning/error。主程序与 RTC 目标分别更新于本机 20:45:51、20:49:17。

真实素材实验 38.2435 秒通过。画布有色范围：普通 MP4/MKV 800×450；SAR4:3 为 800×338；90/270 度为 338×600；SAR4:3 加 90 度为 252×600。四角方向全部匹配。45 度样例按设计拒绝。

晚音轨首 PCM 为 728000 微秒；缺口素材触发一次实际音频间断；VFR 有 160 帧，保留 33333/33334/100000 微秒间隔。快速重新打开的 trace 记录到旧结果被代次过滤；待显示队列观测最大 7,384,320 字节。

HTTP 断供首轮实验在 1233333 微秒冻结，恢复后继续推进，阻塞操作被新打开取代；本次 `player.log` 只有 PASS，`relay.log` 为空。所有请求都指向本次本地静态媒体服务。增加取消前 `in_flight` 断言后的最终复验 5.1721457 秒通过，结果位于 `build/media-reinforcement/network-final`。

TS HLS HTTP 播放、暂停 seek 和排空 9.7084504 秒通过；fMP4 同项 7.4167203 秒通过，输出分别在 `hls-ts-results`、`hls-fmp4-results`。fMP4 seek 时日志记录两次读取 `init.mp4`，并出现一次 `Found duplicated MOOV Atom. Skipped it`：子解封装器跳过重复的初始化盒。脚本将该诊断精确匹配，并要求实际存在两次初始化读取，保存在 `diagnostics.json`；其余未识别诊断会使实验失败。TS 日志只有正常分片读取与 PASS。

本机该 Qt 运行环境没有枚举到音频输出设备：音频解码、重采样及缺口检测已用真实数据验证，播放器采用无声单调时钟分支。不能把这些结果写成实际音频设备消费或听感已通过。真实摄像头/麦克风、所有格式和任意复杂矩阵也不在这些样例的证明范围内。
