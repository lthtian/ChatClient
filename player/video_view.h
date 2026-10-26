#ifndef VIDEO_PLAYER_VIDEO_VIEW_H_
#define VIDEO_PLAYER_VIDEO_VIEW_H_
#include <QImage>
#include <QWidget>
#include "video_geometry.h"

// 只负责保留和绘制当前图像，不负责解码或播放时钟。
class VideoView : public QWidget {
 public:
  explicit VideoView(QWidget* parent = nullptr);  // 创建可随窗口伸缩的画布。
  void SetImage(QImage image, VideoGeometry geometry = {});  // 保存拥有像素的图像，并请求事件循环重绘。
  QImage image() const { return image_; }  // 返回共享像素引用的 QImage 值。
 protected:
  void paintEvent(
      QPaintEvent* event) override;  // 居中等比例显示，其余区域填黑。
 private:
  VideoGeometry geometry_;  // 绘制时应用，像素本身不再分配旋转副本。
  QImage image_;  // 当前画面，QImage 的引用计数保证绘制时像素仍然有效。
};
#endif  // VIDEO_PLAYER_VIDEO_VIEW_H_
