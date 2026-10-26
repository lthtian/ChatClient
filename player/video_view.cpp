#include "video_view.h"

#include <QPainter>

VideoView::VideoView(QWidget* parent) : QWidget(parent) {
  setObjectName("videoView");
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void VideoView::SetImage(QImage image, VideoGeometry geometry) {
  geometry_ = geometry;
  image_ = std::move(image);
  // update 请求事件循环安排重绘，不在此处直接调用 paintEvent。
  update();
}

void VideoView::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);
  if (image_.isNull()) return;
  painter.setWorldTransform(geometry_.PixelTransform(image_.size(), size()));
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(QPointF(), image_);
}
