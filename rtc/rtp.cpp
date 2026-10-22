#include "rtp.h"

#include <algorithm>
#include <stdexcept>
#include "wire.h"

namespace camera {
namespace {
constexpr int kMaxAccessUnit = 8 * 1024 * 1024;
const QByteArray kStartCode = QByteArray::fromHex("00000001");

}  // namespace

bool ParseRtp(const QByteArray& bytes, RtpPacket& packet) {
  if (bytes.size() < 12 || (quint8(bytes[0]) >> 6) != 2) return false;
  int offset = 12 + 4 * (quint8(bytes[0]) & 0x0f);
  if (offset > bytes.size()) return false;
  if (quint8(bytes[0]) & 0x10) {
    if (offset + 4 > bytes.size()) return false;
    const int extension_size = int(Read16(bytes.constData() + offset + 2)) * 4;
    offset += 4 + extension_size;
    if (offset > bytes.size()) return false;
  }
  int end = bytes.size();
  if (quint8(bytes[0]) & 0x20) {
    const int padding = quint8(bytes.back());
    if (!padding || padding > end - offset) return false;
    end -= padding;
  }
  if (offset >= end) return false;
  packet.sequence = Read16(bytes.constData() + 2);
  packet.timestamp = Read32(bytes.constData() + 4);
  packet.ssrc = Read32(bytes.constData() + 8);
  packet.payload_type = quint8(bytes[1]) & 0x7f;
  packet.marker = (quint8(bytes[1]) & 0x80) != 0;
  packet.payload = bytes.mid(offset, end - offset);
  return true;
}

void ReorderBuffer::Insert(RtpPacket packet, qint64 now_us) {
  if (!initialized_) { next_ = packet.sequence; initialized_ = true; }
  const qint64 number = Unwrap(packet.sequence, next_, 16);
  // 一个会话内的大幅跳号不按普通乱序处理，避免任意报文占满内存。
  if (number < next_ || number - next_ > 4096 || pending_.count(number) || pending_.size() >= 256) {
    ++rejected_;
    return;
  }
  pending_.emplace(number, Entry{std::move(packet), now_us});
}

void ReorderBuffer::Drain(qint64 now_us, const Deliver& deliver, const Gap& gap) {
  while (!pending_.empty()) {
    auto first = pending_.begin();
    if (first->first != next_) {
      if (pending_.size() < 256 && now_us - first->second.arrival < 50000) return;
      // 等待已到上限：明确通知上层有缺口，不能把断裂的 FU-A 拼成完整 NAL。
      gap(first->first - next_);
      next_ = first->first;
    }
    auto packet = std::move(first->second.packet);
    pending_.erase(first);
    ++next_;
    deliver(packet);
  }
}

void H264Assembler::Clear() {
  active_ = damaged_ = has_idr_ = has_vcl_ = has_sps_ = has_pps_ = false;
  access_unit_.clear();
  fragment_.clear();
}

void H264Assembler::Lost() {
  damaged_ = true;
  waiting_idr_ = true;
  fragment_.clear();
}

bool H264Assembler::AddNal(const QByteArray& nal) {
  if (nal.isEmpty() || (quint8(nal[0]) & 0x80) ||
      access_unit_.size() + nal.size() + 4 > kMaxAccessUnit) return false;
  const int type = quint8(nal[0]) & 31;
  if (type < 1 || type > 23) return false;
  // 这一教学配置使用一组 SPS/PPS；参数集保存在编码层，不是把 RGB 图缓存下来。
  if (type == 7) { sps_ = nal; has_sps_ = true; }
  if (type == 8) { pps_ = nal; has_pps_ = true; }
  has_vcl_ |= type >= 1 && type <= 5;
  has_idr_ |= type == 5;
  access_unit_.append(kStartCode).append(nal);
  return true;
}

void H264Assembler::Push(const RtpPacket& packet, const Complete& complete) {
  if (!active_ || timestamp_ != packet.timestamp) {
    if (active_) { ++dropped_; waiting_idr_ = true; }
    Clear();
    active_ = true;
    timestamp_ = packet.timestamp;
  }
  const auto& payload = packet.payload;
  if (payload.isEmpty() || (quint8(payload[0]) & 0x80)) { Lost(); return; }
  const int type = quint8(payload[0]) & 31;
  bool valid = true;
  if (type >= 1 && type <= 23) {
    valid = fragment_.isEmpty() && AddNal(payload);
  } else if (type == 24) {
    // STAP-A 是多个“长度+NAL”的聚合，FFmpeg 作为发送端时可能采用它。
    int position = 1;
    valid = fragment_.isEmpty();
    while (valid && position + 2 <= payload.size()) {
      const int length = Read16(payload.constData() + position);
      position += 2;
      if (!length || length > payload.size() - position) { valid = false; break; }
      valid = AddNal(payload.mid(position, length));
      position += length;
    }
    valid &= position == payload.size();
  } else if (type == 28 && payload.size() > 2) {
    const quint8 flags = quint8(payload[1]);
    const bool first = flags & 0x80, last = flags & 0x40;
    const quint8 header = (quint8(payload[0]) & 0xe0) | (flags & 31);
    valid = !(flags & 0x20) && !(first && last) && (flags & 31) >= 1 && (flags & 31) <= 23;
    if (first) {
      valid &= fragment_.isEmpty();
      fragment_ = QByteArray(1, char(header));
    } else {
      valid &= !fragment_.isEmpty() && quint8(fragment_[0]) == header;
    }
    if (valid && fragment_.size() + payload.size() - 2 <= kMaxAccessUnit) {
      fragment_.append(payload.constData() + 2, payload.size() - 2);
      if (last) { valid = AddNal(fragment_); fragment_.clear(); }
    } else valid = false;
  } else valid = false;
  if (!valid) Lost();
  if (!packet.marker) return;
  // Marker 是访问单元边界，不是 I 帧标志；FU 没结束时即使有 Marker 也不解码。
  if (damaged_ || !fragment_.isEmpty() || !has_vcl_ ||
      (waiting_idr_ && (!has_idr_ || sps_.isEmpty() || pps_.isEmpty()))) {
    if (damaged_ || !fragment_.isEmpty()) waiting_idr_ = true;
    if (has_vcl_ || damaged_ || !fragment_.isEmpty()) ++dropped_;
  } else {
    QByteArray prefix;
    if (has_idr_) {
      if (!has_sps_) prefix.append(kStartCode).append(sps_);
      if (!has_pps_) prefix.append(kStartCode).append(pps_);
      waiting_idr_ = false;
    }
    complete(prefix + access_unit_, timestamp_);
  }
  Clear();
}
}  // namespace camera
