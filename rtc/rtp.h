#pragma once

#include <QByteArray>
#include <QVector>
#include <functional>
#include <map>

namespace camera {
constexpr int kClockRate = 90000;  // H.264 RTP 时钟固定为 90 kHz，与帧率无关。
constexpr int kDatagramSize = 1200;  // 整个 RTP 报文的最大字节数，不含 UDP/IP 头。

struct RtpPacket {
  quint16 sequence = 0;     // 每发一个 RTP 包加一，65535 后回到 0。
  quint32 timestamp = 0;    // 当前访问单元的采样/显示时刻，单位为 1/90000 秒。
  quint32 ssrc = 0;         // 本场发送源标识，不是用户 ID 或流下标。
  quint8 payload_type = 96; // 动态负载编号；本例通过 SDP 约定 96=H264/90000。
  bool marker = false;     // H.264 中表示这个访问单元的最后一个 RTP 包。
  QByteArray payload;      // 拥有载荷内存，不含 RTP 头、扩展与 padding。
};

// 校验 RTP 边界并跳过 CSRC、扩展及 padding。
bool ParseRtp(const QByteArray& bytes, RtpPacket& packet);

class ReorderBuffer {
 public:
  using Deliver = std::function<void(const RtpPacket&)>;
  using Gap = std::function<void(qint64)>;
  // 记录报文及到达时间；重复、过晚和异常远的序号不加入缓冲。
  void Insert(RtpPacket packet, qint64 now_us);
  // 连续数据立即交付；缺包最多等待 50ms 或 256 包，然后报告缺口。
  void Drain(qint64 now_us, const Deliver& deliver, const Gap& gap);
  quint64 rejected() const { return rejected_; }  // 重复/过晚/异常序号包数。
  int size() const { return int(pending_.size()); }  // 当前待重排包数。
 private:
  struct Entry {
    RtpPacket packet;   // 缓冲中的一个完整 RTP 包。
    qint64 arrival;     // 首次到达的单调时钟，单位微秒。
  };
  bool initialized_ = false;          // 是否已经由首包建立序号基准。
  qint64 next_ = 0;                   // 下一个期待的展开序号。
  quint64 rejected_ = 0;              // 未进入缓冲的报文计数。
  std::map<qint64, Entry> pending_;   // 按展开序号排序，容量有上限。
};

class H264Assembler {
 public:
  using Complete = std::function<void(QByteArray, quint32)>;
  // 输入已经按序排列的 RTP 包；完整访问单元通过回调交给解码器。
  void Push(const RtpPacket& packet, const Complete& complete);
  // 缺包使参考链不再可靠，丢弃当前单元并等待下一份 IDR。
  void Lost();
  quint64 dropped() const { return dropped_; }  // 损坏或等待 IDR 时丢弃的访问单元数。
 private:
  // 将完整 NAL 加入 Annex B 单元，同时识别 SPS/PPS、VCL 和 IDR。
  bool AddNal(const QByteArray& nal);
  // 清空当前单元，保留参数集和等待 IDR 的恢复状态。
  void Clear();
  bool active_ = false;       // 是否正在收集一个时间戳对应的访问单元。
  bool damaged_ = false;      // 当前单元是否损坏。
  bool waiting_idr_ = true;   // 初次加入/丢包后等待随机访问点。
  bool has_idr_ = false;      // 当前单元中是否有 IDR slice。
  bool has_vcl_ = false;      // 当前单元中是否有图像编码数据。
  bool has_sps_ = false;      // 当前单元是否已经包含 SPS。
  bool has_pps_ = false;      // 当前单元是否已经包含 PPS。
  quint32 timestamp_ = 0;     // 当前访问单元的 RTP 时间戳。
  QByteArray access_unit_;   // Annex B 数据，最多 8 MiB。
  QByteArray fragment_;      // 正在重组的 FU-A NAL，含还原后的 NAL 头。
  QByteArray sps_, pps_;      // 最近的参数集，供中途起播和 IDR 恢复使用。
  quint64 dropped_ = 0;      // 被丢弃的访问单元计数。
};
}  // namespace camera
