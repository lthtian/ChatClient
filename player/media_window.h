#ifndef VIDEO_PLAYER_MEDIA_WINDOW_H_
#define VIDEO_PLAYER_MEDIA_WINDOW_H_

#include <QElapsedTimer>
#include <QSlider>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <deque>

#include "audio_output.h"
#include "video_view.h"

class QLabel;
class QComboBox;
class QPushButton;

// 将左键点击位置直接映射到进度值；拖动时只更新预览，释放后执行 seek。
class ProgressSlider : public QSlider {
 public:
  explicit ProgressSlider(QWidget* parent);  // 创建水平进度条。
 protected:
  void mousePressEvent(
      QMouseEvent* event) override;                  // 标记拖动并设置点击位置。
  void mouseMoveEvent(QMouseEvent* event) override;  // 跟随拖动更新目标值。
  void mouseReleaseEvent(
      QMouseEvent* event) override;  // 提交位置并发出释放信号。
 private:
  void SetFromMouse(int x);  // 根据样式提供的滑槽和滑块尺寸换算数值。
};

// GUI 线程中的播放调度与控件。后台只做媒体运算，设备也在 GUI 线程使用。
class MediaWindow : public QWidget {
  Q_OBJECT
 public:
  enum class State { Empty, Buffering, Playing, Paused, Stopped, Ended, Error };
  Q_ENUM(State)
  MediaWindow();            // 创建控件、连接信号并启动解码工作线程。
  ~MediaWindow() override;  // 停设备、请求中断、退出并等待工作线程。
  void OpenFile(const QString& filename);  // 本地入口：直接打开已下载文件。
  void OpenOnline(const QString& name);    // 在线入口：先向聊天模块申请读取凭证。
  void SetOnlineSource(quint64 session, MediaSource source, const QString& error);
  // 上述回调只接受当前授权请求；聊天模块负责提供地址、头和到期时间。
  bool IsOnline() const { return source_.IsOnline(); }  // 当前输入是否来自网络。
  void SeekTo(qint64 position_us);  // 跳到媒体位置，保留当前播放/暂停意图。
  void TogglePlayback();            // 暂停、恢复，或从停止/结束状态重播。
  void Stop();  // 清掉声音和缓存并回到零；下一次播放从头定位。
  State state() const { return state_; }  // 获取当前播放状态。
  bool HasAudioOutput() const { return audio_.IsOpen(); }  // 是否实际启用了声音设备。
  qint64 PositionUs() const;              // 获取公共媒体时间轴上的播放位置。
  qint64 DurationUs() const {
    return duration_us_;
  }                                             // 获取容器时长，未知为零。
  QString ErrorText() const { return error_; }  // 获取最近失败说明。
  QImage CurrentImage() const { return view_->image(); }  // 获取当前显示画面。

 signals:
  void OpenRequested(MediaSource source,
                     quint64 session);  // 将输入描述复制给工作线程。
  void AuthorizationRequested(quint64 session, QString rendition);  // 初次/过期时申请整套授权并保留档位。
  void AudioEnabledRequested(bool enabled);  // 无设备时通知后台跳过音轨，避免 PCM 堆积。
  void OutputRateRequested(int rate);   // 把设备采样率交给后台重采样器。
  void ReadRequested(quint64 session);  // 请求读取一个包，最多一个请求在途。
  void SeekRequested(qint64 position_us,
                     quint64 session);  // 请求后台定位及清状态。
  void FramePresented(qint64 pts_us);  // 实际交给画布的媒体位置，用于观察调度。

 private:
  void OpenSource(MediaSource source, qint64 position_us, bool play_after);  // 共用的打开流程。
  bool AuthorizationExpired() const;  // 仅 HTTP 输入检查短期凭证，预留一秒余量。
  void RefreshAuthorization(qint64 position_us, bool play_after);  // 保留目标和播放意图重新授权。
  void BufferUnderrun(qint64 position_us);  // 保留待播放数据，冻结设备和时钟等待补充。
  void CreateControls();  // 构造界面与快捷键，并连接用户操作。
  void UpdateRenditions();  // 从服务端源描述生成清晰度选项。
  void ChangeRendition(int index);  // 保留位置和播放意图，重新打开所选媒体列表。
  void ConnectDecoder();  // 建立跨线程请求、结果及错误信号连接。
  void StartSeek(qint64 position_us,
                 bool play_after);     // 清 GUI/设备状态并发起定位。
  void AcceptBatch(MediaBatch batch);  // 接收拥有独立内存的结果并检查缓存上限。
  void RequestData();                  // 在媒体准备不足且无在途请求时继续读包。
  void TryStart();  // 首批数据准备好后开始播放或显示暂停预览。
  void Tick();      // 补音频、读取时钟、选择到期画面并检查结束。
  void Fail(const QString& message);  // 清理播放资源并显示错误，允许重新打开。
  void ClearPending(qint64 position_us);  // 清缓存和本次时钟，设置定位目标。
  void UpdateControls();    // 根据状态更新时间、按钮及非拖动中的进度条。
  void ToggleFullscreen();  // 在普通窗口与全屏间切换。

  VideoView* view_;           // 复用第三阶段的等比例画布，Qt 父对象拥有它。
  QPushButton* play_button_;  // 播放、暂停与重播按钮。
  QPushButton* stop_button_;  // 停止并回到零的按钮。
  QPushButton* mute_button_;  // 可勾选的静音按钮。
  ProgressSlider* progress_;  // 0～10000 的相对媒体位置。
  QSlider* volume_;           // 0～100 的感知音量滑块。
  QLabel* status_;            // 状态及时间显示。
  QComboBox* quality_;        // HLS 已就绪档位；本地和单文件 HTTP 不显示。
  MediaDecoder* decoder_;  // 工作线程拥有操作权，由 finished/deleteLater 销毁。
  QThread thread_;         // GUI 线程中的线程管理对象，内部运行解码事件循环。
  QTimer timer_;           // 10ms 唤醒调度，不把 timeout 次数当作播放时间。
  AudioOutput audio_;      // GUI 线程中的音频输出和 PCM 缓冲。
  QElapsedTimer clock_;    // 无音轨或音轨先结束时使用的单调时钟。
  std::deque<VideoPicture> pictures_;  // 声画交错读取产生的少量待显示画面。
  State state_ = State::Empty;         // 当前界面与播放控制状态。
  MediaSource source_;                // 本地路径或 HTTP 描述，后续解码与播放共用。
  bool awaiting_authorization_ = false;  // 有一个尚未返回的业务授权请求。
  bool rebuffering_ = false;           // 网络断粮后的等待；保留当前画面和未消费 PCM。
  QString error_;                      // 当前错误说明。
  quint64 session_ = 0;                // 打开、seek、停止递增，屏蔽旧结果。
  qint64 duration_us_ = 0;             // 总时长，单位微秒。
  qint64 audio_end_us_ = -1;           // 容器描述的音轨结束时间，未知为 -1。
  qint64 base_us_ = 0;                 // 暂停位置或单调时钟锚点。
  qint64 last_video_end_us_ = 0;       // 最近显示画面的结束位置。
  qint64 picture_bytes_ = 0;           // 当前画面缓存字节量，限制为 64 MiB。
  bool opened_ = false;                // 后台文件是否已打开成功。
  bool in_flight_ = false;             // 是否已有尚未返回的 Read 请求。
  bool eof_ = false;                   // 后台是否已经排空全部媒体数据。
  bool play_after_buffering_ = true;   // 打开或 seek 准备完成后的播放意图。
  bool resume_after_drag_ = false;     // 拖动前是否播放，释放后恢复相同状态。
  bool silent_audio_ = false;          // 文件有音轨但无输出设备，界面说明无声播放。
  bool audio_clock_ = false;           // 是否正在使用设备消费进度作为主时钟。
};
#endif  // VIDEO_PLAYER_MEDIA_WINDOW_H_
