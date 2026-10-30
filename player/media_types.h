#pragma once

#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include <QVector>
#include "video_geometry.h"

// 可跨线程传递的独立画面；两条轨道共用媒体起点，时间单位为微秒。
struct VideoPicture {
  QImage image;            // 拥有像素的 RGB 图像，不借用 AVFrame 内存。
  qint64 pts_us = 0;       // 画面的显示位置。
  qint64 duration_us = 0;  // 画面的预计持续时间。
  VideoGeometry geometry{};  // SAR 与正交显示矩阵，随画面跨线程传递。
};

// 设备格式的 PCM 块：S16、小端、左右声道交错。
struct AudioBlock {
  QByteArray pcm;     // 拥有采样字节，长度为每声道采样数 × 4。
  qint64 pts_us = 0;  // 第一个采样在公共媒体时间轴上的位置。
};

// 一次解码交付的结果；一个编码包可能没有输出，也可能产生多个帧。
struct MediaBatch {
  QVector<VideoPicture> video;  // 本次可用的视频画面。
  QVector<AudioBlock> audio;    // 本次可用的 PCM 块。
  bool eof = false;            // 输入与解码器及重采样器均已排空。
};
Q_DECLARE_METATYPE(MediaBatch)

// 探测事实与本机打开结果分开记录；有解码器不代表轨道一定能打开。
struct MediaTrack {
  int index = -1;
  QString type, codec, profile, language, title, format;
  bool is_default = false;
  bool attached_picture = false;
  bool decoder_available = false;
  QString failure, error;
};
struct MediaTracks {
  QVector<MediaTrack> items;
  int video = -1;
  int audio = -1;
  QString audio_notice;
};
Q_DECLARE_METATYPE(MediaTracks)
