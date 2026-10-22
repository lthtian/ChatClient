#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace camera {
// 网络字节序是高字节在前；按字节读写，避免结构体填充和未对齐访问。
inline quint16 Read16(const char* p) {
  return (quint16(quint8(p[0])) << 8) | quint8(p[1]);
}
// 从已经检查过长度的区域读取一个 32 位无符号数。
inline quint32 Read32(const char* p) {
  return (quint32(Read16(p)) << 16) | Read16(p + 2);
}
// 从已经检查过长度的区域读取一个 64 位无符号数。
inline quint64 Read64(const char* p) {
  return (quint64(Read32(p)) << 32) | Read32(p + 4);
}
// 向报文末尾追加 16 位网络字节序整数。
inline void Put16(QByteArray& bytes, quint16 value) {
  bytes.append(char(value >> 8));
  bytes.append(char(value));
}
// 向报文末尾追加 32 位网络字节序整数。
inline void Put32(QByteArray& bytes, quint32 value) {
  Put16(bytes, quint16(value >> 16));
  Put16(bytes, quint16(value));
}
// 向报文末尾追加 64 位网络字节序整数。
inline void Put64(QByteArray& bytes, quint64 value) {
  Put32(bytes, quint32(value >> 32));
  Put32(bytes, quint32(value));
}
// 把回绕的低位数值展开到 reference 附近，适用于距离小于半个周期的计数。
inline qint64 Unwrap(quint32 value, qint64 reference, int bits) {
  const qint64 period = qint64(1) << bits;
  qint64 delta = qint64(value) - (reference & (period - 1));
  if (delta >= period / 2) delta -= period;
  if (delta < -period / 2) delta += period;
  return reference + delta;
}
}  // namespace camera
