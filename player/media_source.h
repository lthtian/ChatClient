#ifndef VIDEO_PLAYER_MEDIA_SOURCE_H_
#define VIDEO_PLAYER_MEDIA_SOURCE_H_

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>

struct MediaRendition {
  QString id;        // 服务端实际就绪的档位，例如 480p。
  QString location;  // 该档的播放列表 URL，与整套 HLS 共用授权头。
  int width = 0;     // 实际编码宽度。
  int height = 0;    // 实际编码高度。
};

// 只描述输入，不包含聊天业务或解码状态；作为值对象跨线程复制。
struct MediaSource {
  enum class Kind { LocalFile, Http, Hls };  // 本地文件、HTTP MP4、HTTP HLS 列表及分片。
  Kind kind = Kind::LocalFile;          // 决定输入校验、网络选项与缓冲策略。
  QString location;                    // 本地绝对路径，或经业务模块验证的 HTTP URL。
  QString name;                        // 窗口展示的文件名，不展示授权信息。
  QByteArray headers;                  // HTTP 请求头，每行以 CRLF 结尾；本地为空。
  qint64 expires_at_ms = 0;             // HTTP 凭证的 UTC 毫秒截止时间；本地不用。
  QVector<MediaRendition> renditions;   // 仅包含服务端返回的可用档位。
  QString rendition;                  // 当前档位；续签时保留选择。
  QString processing_notice;          // 原件可播而 HLS 排队、处理中或失败时的说明。
  bool IsOnline() const { return kind != Kind::LocalFile; }  // 是否需要网络读取策略。
};
Q_DECLARE_METATYPE(MediaSource)

#endif  // VIDEO_PLAYER_MEDIA_SOURCE_H_
