#ifndef VIDEO_PLAYER_MEDIA_SOURCE_H_
#define VIDEO_PLAYER_MEDIA_SOURCE_H_

#include <QByteArray>
#include <QMetaType>
#include <QString>

// 只描述输入，不包含聊天业务或解码状态；作为值对象跨线程复制。
struct MediaSource {
  enum class Kind { LocalFile, Http };  // 本地随机读取与带授权的 HTTP 范围读取。
  Kind kind = Kind::LocalFile;          // 决定输入校验、网络选项与缓冲策略。
  QString location;                    // 本地绝对路径，或经业务模块验证的 HTTP URL。
  QString name;                        // 窗口展示的文件名，不展示授权信息。
  QByteArray headers;                  // HTTP 请求头，每行以 CRLF 结尾；本地为空。
  qint64 expires_at_ms = 0;             // HTTP 凭证的 UTC 毫秒截止时间；本地不用。
  bool IsOnline() const { return kind == Kind::Http; }  // 是否需要网络读取策略。
};
Q_DECLARE_METATYPE(MediaSource)

#endif  // VIDEO_PLAYER_MEDIA_SOURCE_H_
