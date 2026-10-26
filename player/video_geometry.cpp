#include "video_geometry.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void VideoGeometry::SetDisplayMatrix(const int32_t* matrix) {
  // ISO 显示矩阵使用行向量；平移在独立画布居中时消除。
  if (matrix[2] != 0 || matrix[5] != 0 || matrix[8] != (1 << 30))
    throw std::runtime_error("本播放器不支持透视或非标准齐次显示矩阵");
  int axes[4];
  const int indices[]{0, 1, 3, 4};
  for (int i = 0; i < 4; ++i) {
    const double value = matrix[indices[i]] / 65536.0;
    axes[i] = static_cast<int>(std::round(value));
    if (std::abs(value - axes[i]) > 2.0 / 65536 || std::abs(axes[i]) > 1)
      throw std::runtime_error("本播放器仅支持 0/90/180/270 度及镜像的显示矩阵");
  }
  if (axes[0] * axes[0] + axes[1] * axes[1] != 1 ||
      axes[2] * axes[2] + axes[3] * axes[3] != 1 ||
      axes[0] * axes[2] + axes[1] * axes[3] != 0)
    throw std::runtime_error("本播放器不支持缩放或剪切显示矩阵");
  a = axes[0]; b = axes[1]; c = axes[2]; d = axes[3];
}

QTransform VideoGeometry::PixelTransform(QSize pixels, QSize viewport) const {
  if (pixels.isEmpty() || viewport.isEmpty()) return {};
  const qreal sar = sar_num > 0 && sar_den > 0 ? qreal(sar_num) / sar_den : 1;
  const QTransform orientation(a * sar, b * sar, c, d, 0, 0);
  const QRectF bounds = orientation.mapRect(QRectF(QPointF(), QSizeF(pixels)));
  const qreal scale = std::min(viewport.width() / bounds.width(),
                               viewport.height() / bounds.height());
  const qreal x = (viewport.width() - bounds.width() * scale) / 2;
  const qreal y = (viewport.height() - bounds.height() * scale) / 2;
  return QTransform(a * sar * scale, b * sar * scale, c * scale, d * scale,
                    x - bounds.x() * scale, y - bounds.y() * scale);
}
