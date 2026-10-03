#include "video_view.h"

#include <QPainter>

VideoView::VideoView(QWidget* parent) : QWidget(parent) {
  setObjectName("videoView");
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void VideoView::SetImage(QImage image) {
  image_ = std::move(image);
  // update 请求事件循环安排重绘，不在此处直接调用 paintEvent。
  update();
}

void VideoView::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);
  if (image_.isNull()) return;
  const QSize size = image_.size().scaled(this->size(), Qt::KeepAspectRatio);
  const QRect target((width() - size.width()) / 2,
                     (height() - size.height()) / 2, size.width(),
                     size.height());
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(target, image_);
}
