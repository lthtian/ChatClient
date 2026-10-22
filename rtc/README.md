# 摄像头 WebRTC 模块

本模块直接接入 ChatClient。聊天层申请发布或观看授权，`CameraWindow` 负责界面与采集，`RtcConnection` 在工作线程中建立媒体连接。它不启动 OBS 或阶段 9.1 的程序。

## 阅读顺序

1. `live_chat.cpp`：创建入口、聊天卡片、`live_create / live_read / live_end`。
2. `camera_window.h/.cpp`：相机原始画面、麦克风开关、观看输出和异步关闭。
3. `rtc_connection.h/.cpp`：SDP Offer/Answer、ICE、媒体轨道、RTP/RTCP、会话释放。
4. `media_codec.h/.cpp`：H.264 / Opus 编码和解码，像素转换、PCM 重采样。
5. `rtp.h/.cpp`：有限乱序缓冲、H.264 NAL 重组和关键帧恢复。
6. `microphone_capture.h/.cpp`：设备线程独占麦克风启停与采集，向编码线程提供对应时间区间的 PCM。

复用 `broadcast/audio_capture` 的 WASAPI 麦克风采集，以及 `player` 的图像显示、音频设备和媒体批次数据结构。RTC 不使用 MP4/FLV 解封装入口。

麦克风的打开和关闭不在编码循环内执行。设备尚未准备好时继续发送画面与 Opus 静音；准备好后按公共 QPC 原点读取 PCM，不能为麦克风单独重置时间线。`tests/microphone_capture_smoke.pro` 使用真实麦克风验证启停与本地编解码，不上传或保存录音。

## 构建依赖

已使用 Qt 5.15.2 / MinGW 8.1 x64、FFmpeg 8.1.1 shared、libdatachannel 0.24.6 和 Mbed TLS 3.6.4。Qt 工程通过 `camera_rtc.pri` 链接静态 RTC 依赖，`RTC_ROOT` 默认是 `D:/chat/_deps/rtc`，也可通过 qmake 参数指定。

`dependency-sources.json` 记录源码版本与下载归档 SHA-256。libdatachannel 的子模块必须放到对应的 `deps` 目录；Mbed TLS 也需要它所固定的 `framework` 子模块，不能仅下载空目录。各第三方源码中的许可证和版权声明必须保留。

依赖目录布局：

```text
RTC_ROOT/
  libdatachannel/            源码及其 deps 子模块
  mbedtls/                  源码及 framework 子模块
  mbedtls-build/             Mbed TLS 构建目录
  datachannel-build/         libdatachannel 构建目录
  install/                  Mbed TLS 静态库和头文件
```

以下命令在普通 CMD 中执行，先进入 `RTC_ROOT`。路径是本机实际 Kit；其他电脑应选择同一 ABI 的工具链。

```bat
set PATH=F:\Qt\Tools\mingw810_64\bin;F:\Qt\Tools\CMake_64\bin;F:\Qt\Tools\Ninja;C:\Windows\System32;C:\Windows
python mbedtls\scripts\config.py set MBEDTLS_SSL_DTLS_SRTP
cmake -S mbedtls -B mbedtls-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%CD%/install -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF
cmake --build mbedtls-build --parallel 2
cmake --install mbedtls-build
cmake -S libdatachannel -B datachannel-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%CD%/install -DUSE_MBEDTLS=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON -DNO_WEBSOCKET=ON
cmake --build datachannel-build --target datachannel-static --parallel 2
```

`MBEDTLS_SSL_DTLS_SRTP` 使 DTLS 能协商 SRTP 密钥配置。缺少它，RTC 依赖不能完成构建。使用 `datachannel-static` 目标，Qt 工程中的 `RTC_STATIC` 与其对应；不混用其他编译器生成的 C++ 静态库。

Qt 主程序在项目根目录下的独立输出目录构建：

```bat
mkdir build\rtc
cd build\rtc
set PATH=F:\Qt\Tools\mingw810_64\bin;F:\Qt\5.15.2\mingw81_64\bin;C:\Windows\System32;C:\Windows
qmake -o Makefile ../../ChatClient.pro -spec win32-g++ CONFIG+=debug CONFIG-=release
mingw32-make -j2 debug
windeployqt --debug --compiler-runtime debug\ChatClient.exe
```

FFmpeg shared DLL 仍按主工程原有方式放在 EXE 旁边。已有 Makefile 时，普通源文件修改只需要增量 make。云端必须部署支持 RTC 的 ChatServer/SRS、数据库迁移和信令配置，单独更新客户端不够。

## 联调与边界

`tests/rtc_smoke.pro` 是诊断目标：真实编码、RTC 网络、SRS 分发和解码，不伪造服务器响应；`devices` 参数只枚举设备。正式界面通过“创建直播 → 摄像头直播”进入。

接收器需要服务端同时提供两条轨道的 RTCP SR。SRS 6.0.184 原版没有这个发送逻辑，项目使用服务端 `deploy/srs-6.0.184-sender-report.patch` 补齐；`build-srs.sh` 会检查并应用补丁。不能用未打补丁的同版本 SRS 替换部署产物。构建与真实联调记录保存在服务端工作区 `docs/STAGE10_RTC_DELIVERY.md`。

720p/30fps、H.264/Opus，麦克风默认关闭。没有音频输出设备时可走无声显示；相机和音频设备仍需要实际硬件验证。本模块没有实现 TURN 中继、回声消除或自适应码率，UDP 媒体路径不通时会明确报错。
