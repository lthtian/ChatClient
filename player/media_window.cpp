#include "media_window.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QStyleOptionSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace {
QString TimeText(qint64 us) {
  const qint64 seconds = std::max<qint64>(0, us / 1000000);
  return QString("%1:%2")
      .arg(seconds / 60, 2, 10, QLatin1Char('0'))
      .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
}  // namespace

ProgressSlider::ProgressSlider(QWidget* parent)
    : QSlider(Qt::Horizontal, parent) {
  setRange(0, 10000);
}
void ProgressSlider::SetFromMouse(int x) {
  QStyleOptionSlider option;
  initStyleOption(&option);
  const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option,
                                               QStyle::SC_SliderGroove, this);
  const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option,
                                               QStyle::SC_SliderHandle, this);
  setValue(QStyle::sliderValueFromPosition(
      minimum(), maximum(), x - groove.x() - handle.width() / 2,
      std::max(1, groove.width() - handle.width()), option.upsideDown));
}
void ProgressSlider::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QSlider::mousePressEvent(event);
    return;
  }
  setSliderDown(true);
  SetFromMouse(event->x());
  event->accept();
}
void ProgressSlider::mouseMoveEvent(QMouseEvent* event) {
  if (isSliderDown())
    SetFromMouse(event->x());
  else
    QSlider::mouseMoveEvent(event);
}
void ProgressSlider::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && isSliderDown()) {
    SetFromMouse(event->x());
    setSliderDown(false);
    event->accept();
  } else
    QSlider::mouseReleaseEvent(event);
}

MediaWindow::MediaWindow()
    : view_(new VideoView(this)),
      play_button_(new QPushButton(this)),
      stop_button_(new QPushButton(QString::fromUtf8("停止"), this)),
      mute_button_(new QPushButton(QString::fromUtf8("静音"), this)),
      progress_(new ProgressSlider(this)),
      volume_(new QSlider(Qt::Horizontal, this)),
      status_(new QLabel(this)),
      quality_(new QComboBox(this)),
      decoder_(new MediaDecoder) {
  trace_clock_.start();
  const QString trace_path = qEnvironmentVariable("CHAT_MEDIA_TRACE");
  if (!trace_path.isEmpty()) {
    trace_file_.setFileName(trace_path);
    if (trace_file_.open(QIODevice::WriteOnly | QIODevice::Append)) {
      trace_timer_.setInterval(500);
      connect(&trace_timer_, &QTimer::timeout, this, [this] { Trace("sample"); });
      trace_timer_.start();
    } else qWarning("Cannot open CHAT_MEDIA_TRACE output");
  }
  qRegisterMetaType<MediaBatch>();
  qRegisterMetaType<MediaSource>();
  setWindowTitle(QString::fromUtf8("音视频播放器"));
  resize(960, 640);
  setMinimumSize(600, 360);
  CreateControls();
  ConnectDecoder();
  timer_.setInterval(10);
  timer_.setTimerType(Qt::PreciseTimer);
  connect(&timer_, &QTimer::timeout, this, &MediaWindow::Tick);
  reconnect_timer_.setSingleShot(true);
  connect(&reconnect_timer_, &QTimer::timeout, this, [this] {
    if (source_.IsLive() && !live_ended_ && state_ == State::Buffering)
      RefreshAuthorization(0, true);
  });
  thread_.start();
  UpdateControls();
}
MediaWindow::~MediaWindow() {
  Trace("close");
  audio_.Close();
  thread_.requestInterruption();
  thread_.quit();
  thread_.wait();
}

void MediaWindow::CreateControls() {
  auto* open = new QPushButton(QString::fromUtf8("打开视频"), this);
  auto* fullscreen = new QPushButton(QString::fromUtf8("全屏"), this);
  play_button_->setObjectName("mediaPlayButton");
  progress_->setObjectName("mediaProgress");
  mute_button_->setCheckable(true);
  volume_->setRange(0, 100);
  volume_->setValue(70);
  volume_->setMaximumWidth(120);
  volume_->setToolTip(QString::fromUtf8("音量"));
  auto* controls = new QHBoxLayout;
  for (auto* button : {open, play_button_, stop_button_, mute_button_})
    controls->addWidget(button);
  controls->addWidget(volume_);
  controls->addStretch();
  quality_->setObjectName("mediaQuality");
  quality_->setToolTip(QStringLiteral("清晰度"));
  quality_->hide();
  controls->addWidget(quality_);
  connect(quality_, QOverload<int>::of(&QComboBox::activated), this, &MediaWindow::ChangeRendition);
  controls->addWidget(fullscreen);
  auto* layout = new QVBoxLayout(this);
  layout->addWidget(view_, 1);
  layout->addWidget(progress_);
  layout->addWidget(status_);
  layout->addLayout(controls);
  connect(open, &QPushButton::clicked, this, [this] {
    const QString file = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("打开本地视频"), {},
        QString::fromUtf8(
            "视频 (*.mp4 *.mkv *.mov *.avi *.webm);;所有文件 (*)"));
    if (!file.isEmpty()) OpenFile(file);
  });
  connect(play_button_, &QPushButton::clicked, this,
          &MediaWindow::TogglePlayback);
  connect(stop_button_, &QPushButton::clicked, this, &MediaWindow::Stop);
  connect(fullscreen, &QPushButton::clicked, this,
          &MediaWindow::ToggleFullscreen);
  connect(volume_, &QSlider::valueChanged, this, [this](int value) {
    audio_.SetVolume(value, mute_button_->isChecked());
  });
  connect(mute_button_, &QPushButton::toggled, this,
          [this](bool muted) { audio_.SetVolume(volume_->value(), muted); });
  // 拖动期间冻结播放；松手只定位一次，避免每移动一个像素就清空解码器。
  connect(progress_, &QSlider::sliderPressed, this, [this] {
    resume_after_drag_ = state_ == State::Playing ||
                         (state_ == State::Buffering && play_after_buffering_);
    if (state_ == State::Playing) TogglePlayback();
  });
  connect(progress_, &QSlider::valueChanged, this, [this](int value) {
    if (progress_->isSliderDown())
      status_->setText(QString::fromUtf8("定位到 ") +
                       TimeText(duration_us_ * value / 10000));
  });
  connect(progress_, &QSlider::sliderReleased, this, [this] {
    StartSeek(duration_us_ * progress_->value() / 10000, resume_after_drag_);
  });
  const auto shortcut = [this](const QKeySequence& key, auto action) {
    auto* item = new QShortcut(key, this);
    connect(item, &QShortcut::activated, this, action);
  };
  shortcut(QKeySequence(Qt::Key_Space), [this] { TogglePlayback(); });
  shortcut(QKeySequence(Qt::Key_Left),
           [this] { SeekTo(PositionUs() - 5000000); });
  shortcut(QKeySequence(Qt::Key_Right),
           [this] { SeekTo(PositionUs() + 5000000); });
  shortcut(QKeySequence(Qt::Key_F), [this] { ToggleFullscreen(); });
  shortcut(QKeySequence(Qt::Key_Escape), [this] {
    if (isFullScreen()) showNormal();
  });
}

void MediaWindow::ConnectDecoder() {
  connect(decoder_, &MediaDecoder::TimelineReady, this,
      [this](quint64 session, qint64 origin, qint64 seek_origin) {
    if (session != session_) { ++stale_results_; return; }
    origin_us_ = origin; seek_origin_us_ = seek_origin; timeline_known_ = true;
    Trace("timeline");
  });
  connect(decoder_, &MediaDecoder::AudioDiscontinuity, this,
      [this](quint64 session, qint64 actual, qint64 expected) {
    if (session != session_) { ++stale_results_; return; }
    Trace("audio_discontinuity", {{"actual_pts_us", double(actual)},
          {"expected_pts_us", double(expected)}, {"delta_us", double(actual - expected)}});
  });
  decoder_->moveToThread(&thread_);
  connect(&thread_, &QThread::finished, decoder_, &QObject::deleteLater);
  // 显式排队连接保证槽函数在 decoder_ 所属线程执行。
  connect(this, &MediaWindow::OpenRequested, decoder_, &MediaDecoder::Open,
          Qt::QueuedConnection);
  connect(this, &MediaWindow::CloseRequested, decoder_, &MediaDecoder::Close, Qt::QueuedConnection);
  connect(this, &MediaWindow::AudioEnabledRequested, decoder_,
          &MediaDecoder::SetAudioEnabled, Qt::QueuedConnection);
  connect(this, &MediaWindow::OutputRateRequested, decoder_,
          &MediaDecoder::SetOutputRate, Qt::QueuedConnection);
  connect(this, &MediaWindow::ReadRequested, decoder_, &MediaDecoder::Read,
          Qt::QueuedConnection);
  connect(this, &MediaWindow::SeekRequested, decoder_, &MediaDecoder::Seek,
          Qt::QueuedConnection);
  connect(decoder_, &MediaDecoder::Opened, this,
          [this](quint64 session, qint64 duration, bool has_audio,
                 qint64 audio_end) {
            if (session != session_) { ++stale_results_; return; }
            try {
              opened_ = true;
              duration_us_ = duration;
              // 切档或续签恢复的位置也必须落在新输入的有效时间范围内。
              if (duration_us_ > 0)
                base_us_ = std::clamp<qint64>(base_us_, 0, std::max<qint64>(0, duration_us_ - 1000));
              audio_end_us_ = audio_end;
              silent_audio_ = false;
              if (has_audio) {
                const int rate = audio_.Open();
                silent_audio_ = rate == 0;
                emit AudioEnabledRequested(!silent_audio_);
                if (rate > 0) {
                  emit OutputRateRequested(rate);
                  audio_.Restart(base_us_);
                  status_->setToolTip(audio_.Description());
                } else {
                  status_->setToolTip(QStringLiteral("没有音频输出设备，使用单调时钟无声播放；重新打开会再次检测设备"));
                }
              } else
                status_->setToolTip(
                    QString::fromUtf8("此文件没有音轨，使用单调时钟"));
              Trace("opened");
              // 重新授权后是重新打开容器，要先恢复媒体位置再继续送包。
              if (!source_.IsLive() && base_us_ > 0) {
                in_flight_ = true;
                emit SeekRequested(base_us_, session_);
              } else {
                RequestData();
              }
            } catch (const std::exception& e) {
              Fail(QString::fromUtf8(e.what()));
            }
          });
  connect(decoder_, &MediaDecoder::Seeked, this, [this](quint64 session) {
    if (session != session_) { ++stale_results_; return; }
    in_flight_ = false;
    Trace("seek_completed");
    RequestData();
  });
  connect(decoder_, &MediaDecoder::BatchReady, this,
          [this](quint64 session, MediaBatch batch) {
            // 排队信号已经发出后无法撤回；代次检查避免 seek
            // 后出现旧声音和旧画面。
            if (session != session_) { ++stale_results_; return; }
            in_flight_ = false;
            try {
              AcceptBatch(std::move(batch));
              if (source_.IsLive() && eof_) {
                Fail(QStringLiteral("直播连接已断开"));
                return;
              }
              TryStart();
              Tick();
              RequestData();
            } catch (const std::exception& e) {
              Fail(QString::fromUtf8(e.what()));
            }
          });
  connect(decoder_, &MediaDecoder::Failed, this,
          [this](quint64 session, QString message) {
            if (session != session_) { ++stale_results_; return; }
            if (AuthorizationExpired())
              RefreshAuthorization(PositionUs(), state_ == State::Playing ||
                  (state_ == State::Buffering && play_after_buffering_));
            else
              Fail(message);
          });
}

void MediaWindow::UpdateRenditions() {
  quality_->clear();
  for (const auto& item : source_.renditions) {
    quality_->addItem(item.id, item.id);
    if (item.id == source_.rendition) quality_->setCurrentIndex(quality_->count() - 1);
  }
  quality_->setVisible(source_.kind == MediaSource::Kind::Hls && !source_.renditions.isEmpty());
}

void MediaWindow::ChangeRendition(int index) {
  if (awaiting_authorization_ || index < 0 || index >= source_.renditions.size()) return;
  const auto item = source_.renditions[index];
  if (item.id == source_.rendition) return;
  const qint64 position = PositionUs();
  const bool play = state_ == State::Playing || (state_ == State::Buffering && play_after_buffering_);
  MediaSource selected = source_;
  selected.rendition = item.id;
  selected.location = item.location;
  // 列表和分片共用凭证；过期时携带所选档位重新授权。
  if (AuthorizationExpired()) {
    selected.location.clear();
    selected.headers.clear();
    selected.expires_at_ms = 0;
  }
  OpenSource(std::move(selected), position, play);
}

void MediaWindow::ClearPending(qint64 position_us) {
  pictures_.clear();
  picture_bytes_ = 0;
  received_video_pts_us_ = received_audio_pts_us_ = presented_pts_us_ = -1;
  in_flight_ = eof_ = false;
  base_us_ = last_video_end_us_ = position_us;
  audio_clock_ = false;
  clock_.invalidate();
}
void MediaWindow::OpenFile(const QString& filename) {
  MediaSource source;
  source.location = filename;
  source.name = QFileInfo(filename).fileName();
  OpenSource(std::move(source), 0, true);
}

void MediaWindow::OpenOnline(const QString& name) {
  MediaSource source;
  source.kind = MediaSource::Kind::Http;
  source.name = name;
  OpenSource(std::move(source), 0, true);
}

void MediaWindow::OpenLive(const QString& name, bool hls) {
  reconnect_attempts_ = 0;
  live_ended_ = false;
  MediaSource source;
  source.kind = hls ? MediaSource::Kind::LiveHls : MediaSource::Kind::LiveFlv;
  source.name = name;
  OpenSource(std::move(source), 0, true);
}

void MediaWindow::SetLiveState(const QString& state) {
  if (!source_.IsLive()) return;
  if (state == "interrupted" && (state_ == State::Playing || (state_ == State::Buffering && opened_))) {
    // 发布会话已经断开，关闭旧时间线；重推可能从零开始，必须重新建立共同起点。
    Fail(QStringLiteral("推流暂时中断"));
    return;
  }
  if (state != "ended") return;
  live_ended_ = true;
  reconnect_timer_.stop();
  ++session_;
  decoder_->RequestSession(session_);
  emit CloseRequested(session_);
  timer_.stop();
  audio_.Close();
  ClearPending(0);
  opened_ = awaiting_authorization_ = false;
  state_ = State::Ended;
  UpdateControls();
}

void MediaWindow::OpenSource(MediaSource source, qint64 position_us, bool play_after) {
  reconnect_timer_.stop();
  ++session_;
  decoder_->RequestSession(session_);
  timer_.stop();
  audio_.Close();
  ClearPending(position_us);
  source_ = std::move(source);
  timeline_known_ = false;
  UpdateRenditions();
  silent_audio_ = false;
  error_.clear();
  opened_ = false;
  rebuffering_ = false;
  duration_us_ = 0;
  audio_end_us_ = -1;
  state_ = State::Buffering;
  play_after_buffering_ = play_after;
  if (position_us == 0) view_->SetImage({});
  setWindowTitle(source_.name + (source_.IsOnline() ? QStringLiteral(" · 在线播放")
                                                   : QStringLiteral(" · 本地播放")));
  awaiting_authorization_ = source_.IsOnline() && source_.location.isEmpty();
  Trace("open_requested");
  UpdateControls();
  if (awaiting_authorization_)
    emit AuthorizationRequested(session_, source_.rendition);
  else
    emit OpenRequested(source_, session_);
}

void MediaWindow::SetOnlineSource(quint64 session, MediaSource source, const QString& error) {
  if (session != session_ || !awaiting_authorization_) return;
  awaiting_authorization_ = false;
  if (!error.isEmpty()) {
    if (source_.IsLive() && error == "live_ended") SetLiveState("ended");
    else Fail(error);
    return;
  }
  if (!source.IsOnline() || source.location.isEmpty() || source.headers.isEmpty() ||
      source.expires_at_ms <= QDateTime::currentMSecsSinceEpoch() + 1000) {
    Fail(QStringLiteral("无效或已过期的在线播放凭证"));
    return;
  }
  source_ = std::move(source);
  UpdateRenditions();
  UpdateControls();
  emit OpenRequested(source_, session_);
}

bool MediaWindow::AuthorizationExpired() const {
  return source_.IsOnline() && source_.expires_at_ms > 0 &&
         QDateTime::currentMSecsSinceEpoch() + 1000 >= source_.expires_at_ms;
}

void MediaWindow::RefreshAuthorization(qint64 position_us, bool play_after) {
  MediaSource source = source_;
  source.location.clear();
  source.headers.clear();
  source.expires_at_ms = 0;
  OpenSource(std::move(source), source_.IsLive() ? 0 : position_us, play_after);
}

void MediaWindow::AcceptBatch(MediaBatch batch) {
  for (auto& picture : batch.video) {
    received_video_pts_us_ = picture.pts_us;
    picture_bytes_ += picture.image.sizeInBytes();
    pictures_.push_back(std::move(picture));
  }
  if (picture_bytes_ > 64 * 1024 * 1024 || pictures_.size() > 180)
    throw std::runtime_error("声画交错间隔过大，画面缓存达到本例上限");
  for (auto& block : batch.audio) {
    received_audio_pts_us_ = block.pts_us;
    audio_.Append(std::move(block));
  }
  eof_ = batch.eof;
}
qint64 MediaWindow::BufferAheadUs(qint64 requested_us) const {
  if (pictures_.empty()) return std::min<qint64>(requested_us, 150000);
  const auto& picture = pictures_.back();
  const qint64 bytes = std::max<qint64>(1, picture.image.sizeInBytes());
  // 解复用会同时推进音视频；音频预读也要为随之读出的 RGB 帧预留空间。
  const qint64 frames = std::max<qint64>(1, 32 * 1024 * 1024 / bytes - 2);
  return std::min(requested_us, frames * std::max<qint64>(1, picture.duration_us));
}
void MediaWindow::RequestData() {
  if (!opened_ || in_flight_ || eof_ ||
      (state_ != State::Buffering && state_ != State::Playing))
    return;
  const qint64 position = PositionUs();
  if (AuthorizationExpired()) {
    RefreshAuthorization(position, state_ == State::Playing || play_after_buffering_);
    return;
  }
  const qint64 ahead = BufferAheadUs(source_.IsOnline() ? 1000000 : 150000);
  const bool need_video = pictures_.empty() ||
      (pictures_.back().pts_us < position + ahead && picture_bytes_ < 32 * 1024 * 1024);
  const bool need_audio =
      audio_.IsOpen() && audio_.BufferedEndUs() < position + ahead &&
      (audio_end_us_ < 0 || audio_.BufferedEndUs() + 1000 < audio_end_us_);
  if (state_ == State::Buffering || need_video || need_audio) {
    in_flight_ = true;
    emit ReadRequested(session_);
  }
}
void MediaWindow::TryStart() {
  if (state_ != State::Buffering) return;
  if (pictures_.empty() && !(rebuffering_ && eof_)) {
    if (eof_) throw std::runtime_error("定位后没有可显示的画面");
    return;
  }
  // 起播和持续预读共用内存预算，避免等待音频时超过视频缓存上限。
  const qint64 ready_us = BufferAheadUs(source_.IsOnline() ? 500000 : 100000);
  const bool video_ready = !source_.IsOnline() || !play_after_buffering_ || eof_ ||
      picture_bytes_ >= 32 * 1024 * 1024 ||
      (!pictures_.empty() && pictures_.back().pts_us + pictures_.back().duration_us >= base_us_ + ready_us);
  const bool audio_ready =
      !audio_.IsOpen() || eof_ || audio_.BufferedEndUs() >= base_us_ + ready_us ||
      (audio_end_us_ >= 0 && audio_.BufferedEndUs() + 1000 >= audio_end_us_);
  if (!video_ready || !audio_ready) return;
  audio_clock_ =
      audio_.IsOpen() && (audio_end_us_ < 0 || base_us_ < audio_end_us_);
  state_ = play_after_buffering_ ? State::Playing : State::Paused;
  if (source_.IsLive() && play_after_buffering_) reconnect_attempts_ = 0;
  if (play_after_buffering_) {
    audio_.Resume();
    clock_.start();
    timer_.start();
  } else {
    // 暂停 seek 只显示目标预览，不向设备写 PCM；已有 PCM 留给恢复播放。
    if (!rebuffering_ && !pictures_.empty()) {
      auto picture = std::move(pictures_.front());
      pictures_.pop_front();
      picture_bytes_ -= picture.image.sizeInBytes();
      view_->SetImage(std::move(picture.image), picture.geometry);
      last_video_end_us_ = picture.pts_us + picture.duration_us;
      presented_pts_us_ = picture.pts_us;
      emit FramePresented(picture.pts_us);
    }
    audio_.Pause();
  }
  rebuffering_ = false;
  UpdateControls();
}
qint64 MediaWindow::PositionUs() const {
  if (state_ != State::Playing) return base_us_;
  if (audio_clock_) return audio_.PositionUs();
  return base_us_ + (clock_.isValid() ? clock_.nsecsElapsed() / 1000 : 0);
}

void MediaWindow::Tick() {
  if (state_ != State::Playing) return;
  try {
    const bool audio_finished =
        eof_ ||
        (audio_end_us_ >= 0 && audio_.BufferedEndUs() + 1000 >= audio_end_us_);
    audio_.Pump(audio_finished);
    if (audio_clock_ && audio_finished && audio_.Drained()) {
      // 音轨先结束时，以同一个位置接续单调时钟，让剩余视频继续显示。
      base_us_ = audio_.PositionUs();
      audio_clock_ = false;
      clock_.restart();
    }
    const qint64 position = PositionUs();
    while (!pictures_.empty() && pictures_.front().pts_us <= position) {
      auto picture = std::move(pictures_.front());
      pictures_.pop_front();
      picture_bytes_ -= picture.image.sizeInBytes();
      last_video_end_us_ = picture.pts_us + picture.duration_us;
      // 若另一张图也已到显示时间，跳过这张迟到图；只绘制最后一张到期画面。
      if (!pictures_.empty() && pictures_.front().pts_us <= position) continue;
      view_->SetImage(std::move(picture.image), picture.geometry);
      presented_pts_us_ = picture.pts_us;
      emit FramePresented(picture.pts_us);
    }
    if (eof_ && pictures_.empty() && position >= last_video_end_us_ &&
        audio_.Drained()) {
      base_us_ = std::max(last_video_end_us_,
                          audio_.IsOpen() ? audio_.BufferedEndUs() : 0);
      state_ = State::Ended;
      timer_.stop();
    }
    // 本地读取很快；网络输入可能暂时跟不上设备，不能让无音轨时钟继续空跑。
    if (source_.IsOnline() && !eof_ && state_ == State::Playing &&
        ((audio_clock_ && !audio_finished && audio_.BufferedEndUs() - position < 30000) ||
         (pictures_.empty() && position >= last_video_end_us_))) {
      BufferUnderrun(audio_clock_ ? position : std::min(position, last_video_end_us_));
    }
    RequestData();
    UpdateControls();
  } catch (const std::exception& e) {
    Fail(QString::fromUtf8(e.what()));
  }
}

void MediaWindow::BufferUnderrun(qint64 position_us) {
  base_us_ = position_us;
  audio_.Pause();
  timer_.stop();
  state_ = State::Buffering;
  play_after_buffering_ = true;
  rebuffering_ = true;
  Trace("underrun");
  // 不清空队列、不 seek：只等待在途 Read 和后续请求补足数据。
}

void MediaWindow::SeekTo(qint64 position_us) {
  StartSeek(position_us,
            state_ == State::Playing ||
                (state_ == State::Buffering && play_after_buffering_));
}
void MediaWindow::StartSeek(qint64 position_us, bool play_after) {
  if (source_.IsLive()) return;
  if (!opened_ || state_ == State::Error) return;
  const qint64 target = std::clamp<qint64>(
      position_us, 0, std::max<qint64>(0, duration_us_ - 1000));
  if (AuthorizationExpired()) {
    RefreshAuthorization(target, play_after);
    return;
  }
  try {
    ++session_;
    decoder_->RequestSession(session_);
    rebuffering_ = false;
    timer_.stop();
    ClearPending(target);
    audio_.Restart(target);
    state_ = State::Buffering;
    play_after_buffering_ = play_after;
    // 定位也是在途操作，等 Seeked 返回后再发读包请求。
    in_flight_ = true;
    UpdateControls();
    Trace("seek_requested", {{"target_us", double(target)}});
    emit SeekRequested(target, session_);
  } catch (const std::exception& e) {
    Fail(QString::fromUtf8(e.what()));
  }
}
void MediaWindow::TogglePlayback() {
  if (source_.IsLive()) {
    if (live_ended_) return;
    if (state_ == State::Playing || state_ == State::Buffering) {
      // 直播暂停直接中断接收，不积累过时音视频；恢复时重新申请入口加入当前流。
      reconnect_timer_.stop();
      ++session_;
      decoder_->RequestSession(session_);
      emit CloseRequested(session_);
      timer_.stop();
      audio_.Close();
      ClearPending(0);
      opened_ = awaiting_authorization_ = false;
      state_ = State::Paused;
      UpdateControls();
    } else {
      reconnect_attempts_ = 0;
      RefreshAuthorization(0, true);
    }
    return;
  }
  if (state_ == State::Playing) {
    base_us_ = PositionUs();
    audio_.Pause();
    state_ = State::Paused;
    timer_.stop();
  } else if (state_ == State::Paused) {
    if (AuthorizationExpired()) {
      RefreshAuthorization(base_us_, true);
      return;
    }
    audio_.Resume();
    state_ = State::Playing;
    clock_.restart();
    timer_.start();
    Tick();
  } else if (state_ == State::Stopped || state_ == State::Ended) {
    if (opened_) StartSeek(0, true);
    else if (source_.IsOnline()) RefreshAuthorization(0, true);
    else OpenSource(source_, 0, true);
  } else if (state_ == State::Buffering)
    play_after_buffering_ = !play_after_buffering_;
  UpdateControls();
}
void MediaWindow::Stop() {
  if (state_ == State::Empty) return;
  try {
    reconnect_timer_.stop();
    if (source_.IsLive()) opened_ = false;
    ++session_;
    decoder_->RequestSession(session_);
    if (source_.IsLive()) emit CloseRequested(session_);
    awaiting_authorization_ = rebuffering_ = false;
    timer_.stop();
    ClearPending(0);
    audio_.Restart(0);
    audio_.Pause();
    state_ = State::Stopped;
    view_->SetImage({});
    UpdateControls();
  } catch (const std::exception& e) {
    Fail(QString::fromUtf8(e.what()));
  }
}
void MediaWindow::Fail(const QString& message) {
  if (RetryLive(message)) return;
  base_us_ = PositionUs();
  ++session_;
  decoder_->RequestSession(session_);
  if (source_.IsLive()) emit CloseRequested(session_);
  awaiting_authorization_ = false;
  state_ = State::Error;
  error_ = message;
  Trace("failure");
  timer_.stop();
  audio_.Close();
  pictures_.clear();
  picture_bytes_ = 0;
  UpdateControls();
}

bool MediaWindow::RetryLive(const QString& message) {
  if (!source_.IsLive() || live_ended_ || state_ == State::Paused || state_ == State::Stopped ||
      reconnect_attempts_ >= 5) return false;
  ++session_;
  decoder_->RequestSession(session_);
  emit CloseRequested(session_);
  timer_.stop();
  audio_.Close();
  ClearPending(0);
  opened_ = awaiting_authorization_ = false;
  state_ = State::Buffering;
  play_after_buffering_ = true;
  error_ = message;
  reconnect_timer_.start(std::min(5000, 500 * (1 << reconnect_attempts_++)));
  UpdateControls();
  return true;
}
QJsonObject MediaWindow::DiagnosticSnapshot() const {
  const auto maybe_pts = [](qint64 value) {
    return value < 0 ? QJsonValue() : QJsonValue(double(value));
  };
  return {{"schema", 1}, {"window", QString::number(quintptr(this), 16)},
      {"monotonic_us", double(trace_clock_.nsecsElapsed() / 1000)},
      {"session", QString::number(session_)},
      {"state", QMetaEnum::fromType<State>().valueToKey(int(state_))},
      {"position_us", double(PositionUs())}, {"duration_us", double(duration_us_)},
      {"source_kind", int(source_.kind)},
      {"origin_us", timeline_known_ ? QJsonValue(double(origin_us_)) : QJsonValue()},
      {"seek_origin_us", timeline_known_ ? QJsonValue(double(seek_origin_us_)) : QJsonValue()},
      {"clock", audio_clock_ ? "audio_device_estimate" : "monotonic"},
      {"video_received_pts_us", maybe_pts(received_video_pts_us_)},
      {"audio_received_pts_us", maybe_pts(received_audio_pts_us_)},
      {"video_presented_pts_us", maybe_pts(presented_pts_us_)},
      {"video_front_pts_us", pictures_.empty() ? QJsonValue() : QJsonValue(double(pictures_.front().pts_us))},
      {"video_tail_end_us", pictures_.empty() ? QJsonValue() :
          QJsonValue(double(pictures_.back().pts_us + pictures_.back().duration_us))},
      {"video_queue_bytes", double(picture_bytes_)}, {"video_queue_frames", int(pictures_.size())},
      {"buffer_target_us", double(BufferAheadUs(source_.IsOnline() ? 1000000 : 150000))},
      {"in_flight", in_flight_}, {"eof", eof_}, {"rebuffering", rebuffering_},
      {"stale_results", QString::number(stale_results_)}, {"audio", audio_.Diagnostics()}};
}

void MediaWindow::Trace(const char* event, QJsonObject details) {
  if (!trace_file_.isOpen()) return;
  QJsonObject value = DiagnosticSnapshot();
  value["event"] = event;
  if (!details.isEmpty()) value["details"] = details;
  QByteArray line = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
  constexpr qint64 limit = 8 * 1024 * 1024;
  if (trace_file_.size() + line.size() > limit) {
    trace_timer_.stop(); trace_file_.close();
    qWarning("CHAT_MEDIA_TRACE reached 8 MiB limit; tracing stopped");
    return;
  }
  if (trace_file_.write(line) != line.size() || !trace_file_.flush()) {
    trace_timer_.stop(); trace_file_.close();
    qWarning("CHAT_MEDIA_TRACE write failed; tracing stopped");
  }
}

void MediaWindow::ToggleFullscreen() {
  if (isFullScreen())
    showNormal();
  else
    showFullScreen();
}
void MediaWindow::UpdateControls() {
  if (traced_state_ != state_ || traced_session_ != session_) {
    Trace("state");
    traced_state_ = state_;
    traced_session_ = session_;
  }
  play_button_->setEnabled(state_ != State::Empty && state_ != State::Error);
  stop_button_->setEnabled(state_ != State::Empty && state_ != State::Error);
  progress_->setEnabled(opened_ && duration_us_ > 0 && state_ != State::Error);
  progress_->setVisible(!source_.IsLive());
  if (source_.IsLive() && live_ended_) {
    play_button_->setEnabled(false);
    status_->setText(QStringLiteral("直播已结束"));
    return;
  }
  const bool playing = state_ == State::Playing ||
                       (state_ == State::Buffering && play_after_buffering_);
  play_button_->setText(playing ? QString::fromUtf8("暂停")
                                : QString::fromUtf8("播放"));
  if (state_ == State::Error) {
    status_->setText(QString::fromUtf8("无法播放：") + error_);
    return;
  }
  const QStringList names{
      QString::fromUtf8("请选择视频"), QString::fromUtf8("准备数据"),
      QString::fromUtf8("播放中"),     QString::fromUtf8("已暂停"),
      QString::fromUtf8("已停止"),     QString::fromUtf8("播放结束"),
      QString::fromUtf8("错误")};
  if (!progress_->isSliderDown()) {
    const qint64 position = PositionUs();
    progress_->setValue(duration_us_ > 0
                            ? static_cast<int>(std::clamp<qint64>(
                                  position * 10000 / duration_us_, 0, 10000))
                            : 0);
    QString mode = source_.IsOnline() ? QStringLiteral("在线") : QStringLiteral("本地");
    if (source_.IsLive()) mode = QStringLiteral("直播");
    if (!source_.processing_notice.isEmpty()) mode += QStringLiteral(" · ") + source_.processing_notice;
    if (silent_audio_) mode += QStringLiteral(" · 无声播放（无音频设备）");
    const QString status = awaiting_authorization_ ? QStringLiteral("申请播放权限") :
        rebuffering_ ? QStringLiteral("网络缓冲中") : names[static_cast<int>(state_)];
    status_->setText(mode + " · " + status + (source_.IsLive() ?
        (reconnect_timer_.isActive() ? QStringLiteral(" · 正在重连 %1/5：%2").arg(reconnect_attempts_).arg(error_) : QString()) :
        " · " + TimeText(position) + " / " + TimeText(duration_us_)));
  }
}
