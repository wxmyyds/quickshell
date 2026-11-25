#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QStringList>

namespace CirrusObservation {
constexpr qsizetype RequestBytes = 4096;
constexpr qsizetype FrameBytes = 1024 * 1024;
constexpr qsizetype SnapshotBytes = 256 * 1024;
constexpr qsizetype DocumentBytes = 8 * 1024 * 1024;
constexpr qint64 MaximumMs = 9007199254740991LL;
constexpr int MaximumLines = 10000;
const QStringList &trackNames();
bool integer(const QJsonValue &value, qint64 minimum, qint64 maximum);
bool counter(const QJsonValue &value, quint64 *result = nullptr);
bool uuid(const QJsonValue &value);
bool text(const QJsonValue &value);
bool parseObject(const QByteArray &bytes, QJsonObject &object);
bool snapshot(const QJsonObject &value);
bool page(const QJsonObject &value, const QJsonObject &summary, int from);
QByteArray frame(const QJsonObject &value);
// One bounded frame, fixed deadline from its first byte. The caller supplies
// monotonic time, making fragmentation and timeout behavior independently testable.
class Framer {
  public:
    bool feed(QByteArrayView bytes, qint64 now);
    bool expired(qint64 now) const;
    qsizetype needed() const;
    bool complete() const;
    QByteArray take();

  private:
    QByteArray m_bytes;
    qint64 m_started = -1;
    quint32 m_length = 0;
    bool m_failed = false;
};
} // namespace CirrusObservation
