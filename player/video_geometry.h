#ifndef VIDEO_PLAYER_VIDEO_GEOMETRY_H_
#define VIDEO_PLAYER_VIDEO_GEOMETRY_H_

#include <QSize>
#include <QTransform>
#include <cstdint>

// 像素宽高比与正交显示方向；默认值也用于已经定向的 RTC 原始图像。
struct VideoGeometry {
  int sar_num = 1;
  int sar_den = 1;
  int a = 1, b = 0, c = 0, d = 1;

  void SetDisplayMatrix(const int32_t* matrix);  // 非正交/透视矩阵抛出明确错误。
  QTransform PixelTransform(QSize pixels, QSize viewport) const;
};

#endif  // VIDEO_PLAYER_VIDEO_GEOMETRY_H_
