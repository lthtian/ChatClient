# C++ 图片进程测试

先按服务端仓库 `media/README.md` 构建 `chat_image`。测试使用真实程序与临时图片文件，不使用进程模拟。

设置 `CHAT_IMAGE_TOOL` 为程序的绝对路径，Windows 还需将 OpenCV DLL 目录加入本次进程的 PATH，然后执行：

```sh
node --test tests/image_processor.test.js
```

验证 Electron 内置 Node 时，设置 `ELECTRON_RUN_AS_NODE=1`，用 Electron 可执行文件执行同样的 `--test` 参数，并等待进程结束、检查退出码与 TAP 输出。在 Windows 上建议由 `subprocess.run` 等调用方式明确等待 GUI 子系统程序退出。

`src/image_processor.js` 供主进程调用，传入 `toolPath`、`sourcePath`、`outputDirectory` 和可选 `AbortSignal`。可执行文件及路径都必须由可信主进程确定，不能直接暴露为 renderer 可任意传参的 IPC。

当前调用模块尚未接入窗口界面或上传任务；UI、受限 preload、发送持久化与网络链路仍在实施清单中。
