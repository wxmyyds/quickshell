#include "observationprotocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QtEndian>
#include <cmath>
#include <utility>

namespace CirrusObservation {
const QStringList &trackNames()
{
    static const QStringList names{"original.standard",    "original.enhanced",     "translation.standard",
                                   "translation.enhanced", "romanization.standard", "romanization.enhanced"};
    return names;
}
bool integer(const QJsonValue &v, qint64 minimum, qint64 maximum)
{
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= double(minimum) &&
           v.toDouble() <= double(maximum) && std::floor(v.toDouble()) == v.toDouble();
}
bool counter(const QJsonValue &v, quint64 *result)
{
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]{0,19})$"));
    if (!v.isString() || !pattern.match(v.toString()).hasMatch())
        return false;
    bool ok = false;
    const auto number = v.toString().toULongLong(&ok);
    if (ok && result)
        *result = number;
    return ok;
}
bool uuid(const QJsonValue &v)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return v.isString() && pattern.match(v.toString()).hasMatch() &&
           v.toString() != QStringLiteral("00000000-0000-0000-0000-000000000000");
}
bool text(const QJsonValue &v)
{
    return v.isString() && v.toString().size() <= 1024 * 1024 && v.toString().isValidUtf16() &&
           !v.toString().contains(QChar::Null);
}
namespace {
bool member(const QJsonValue &v, const QStringList &values)
{
    return v.isString() && values.contains(v.toString());
}
bool time(const QJsonValue &v) { return v.isNull() || integer(v, 0, MaximumMs); }
bool nullableBool(const QJsonValue &v) { return v.isNull() || v.isBool(); }
bool summary(const QJsonObject &s)
{
    return member(s["availability"], {"missing", "empty", "present", "malformed"}) &&
           member(s["timing"], {"plainText", "lines", "words"}) &&
           integer(s["providerOffsetMs"], -MaximumMs, MaximumMs) &&
           integer(s["malformedLines"], 0, 2147483647) && integer(s["lineCount"], 0, MaximumLines) &&
           ((s["availability"] == "present") == (s["lineCount"].toInt() > 0));
}
} // namespace
// QJsonDocument accepts duplicate object keys. Reject them (including escaped
// spellings) before reducing a message, and reject invalid UTF-8 rather than
// replacing it. This gate does not constrain future response fields.
bool parseObject(const QByteArray &bytes, QJsonObject &object)
{
    if (bytes.isEmpty() || bytes.size() > FrameBytes)
        return false;
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString decoded = utf8(bytes);
    if (utf8.hasError() || !decoded.isValidUtf16())
        return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    qsizetype at = 0;
    const auto whitespace = [&] {
        while (at < bytes.size() &&
               (bytes[at] == ' ' || bytes[at] == '\n' || bytes[at] == '\r' || bytes[at] == '\t'))
            ++at;
    };
    const auto stringEnd = [&] {
        ++at;
        while (at < bytes.size()) {
            const auto c = bytes[at++];
            if (c == '\\')
                ++at;
            else if (c == '"')
                break;
        }
    };
    const auto walk = [&](auto &&self, int depth) -> bool {
        if (depth > 24)
            return false;
        whitespace();
        if (at >= bytes.size())
            return false;
        const auto c = bytes[at];
        if (c == '{') {
            ++at;
            whitespace();
            QSet<QString> keys;
            while (at < bytes.size() && bytes[at] != '}') {
                const auto start = at;
                stringEnd();
                const auto key = QJsonDocument::fromJson('[' + bytes.mid(start, at - start) + ']')
                                     .array()
                                     .first()
                                     .toString();
                if (keys.contains(key))
                    return false;
                keys.insert(key);
                whitespace();
                ++at; // colon; syntax was checked by QJsonDocument
                if (!self(self, depth + 1))
                    return false;
                whitespace();
                if (bytes[at] != ',')
                    break;
                ++at;
                whitespace();
            }
            ++at;
        } else if (c == '[') {
            ++at;
            whitespace();
            while (at < bytes.size() && bytes[at] != ']') {
                if (!self(self, depth + 1))
                    return false;
                whitespace();
                if (bytes[at] != ',')
                    break;
                ++at;
            }
            ++at;
        } else if (c == '"') {
            stringEnd();
        } else {
            while (at < bytes.size() && bytes[at] != ',' && bytes[at] != '}' && bytes[at] != ']' &&
                   bytes[at] != ' ' && bytes[at] != '\n' && bytes[at] != '\r' && bytes[at] != '\t')
                ++at;
        }
        return true;
    };
    if (!walk(walk, 0))
        return false;
    object = document.object();
    return true;
}
bool snapshot(const QJsonObject &s)
{
    if (!uuid(s["instanceId"]) || !uuid(s["viewEpoch"]) || !(s["trackId"].isNull() || uuid(s["trackId"])) ||
        !counter(s["sequence"]) || !counter(s["lyricsRevision"]) ||
        !member(s["availability"], {"ready", "unavailable"}))
        return false;
    // An unavailable view explicitly revokes all other content.
    if (s["availability"] == "unavailable")
        return true;
    const auto p = s["playback"].toObject();
    const auto t = s["timing"].toObject();
    const auto c = s["clock"].toObject();
    const auto l = s["lyrics"].toObject();
    quint64 sample = 0, observed = 0;
    if (!member(p["state"],
                {"empty", "resolving", "loading", "ready", "playing", "paused", "ended", "failed"}) ||
        !member(p["intent"], {"play", "pause", "stop"}) || !time(t["positionMs"]) || !time(t["durationMs"]) ||
        !time(t["songPositionMs"]) || !counter(c["sampleMonoMs"], &sample) ||
        !(c["positionObservedMonoMs"].isNull() ||
          (counter(c["positionObservedMonoMs"], &observed) && observed <= sample)) ||
        !member(l["state"], {"none", "loading", "ready", "failed"}) ||
        !member(l["documentExportStatus"],
                {"notReady", "preparing", "ready", "limitExceeded", "invalidData"}) ||
        !member(l["cursorExportStatus"], {"notReady", "ready", "limitExceeded", "invalidData"}) ||
        !integer(l["userOffsetMs"], -120000, 120000) || !nullableBool(l["providerNoLyric"]) ||
        !nullableBool(l["instrumental"]) || !l["tracks"].isObject())
        return false;
    qint64 previewStart = 0;
    if (!t["preview"].isNull()) {
        const auto preview = t["preview"].toObject();
        if (!integer(preview["startMs"], 0, MaximumMs) || !integer(preview["endMs"], 1, MaximumMs) ||
            preview["endMs"].toInteger() <= preview["startMs"].toInteger())
            return false;
        previewStart = preview["startMs"].toInteger();
        if (!t["positionMs"].isNull() &&
            t["positionMs"].toInteger() > preview["endMs"].toInteger() - previewStart)
            return false;
    }
    if (!t["positionMs"].isNull() && !t["songPositionMs"].isNull() &&
        (t["positionMs"].toInteger() > MaximumMs - previewStart ||
         t["songPositionMs"].toInteger() != t["positionMs"].toInteger() + previewStart))
        return false;
    if (!t["positionMs"].isNull() && !t["durationMs"].isNull() &&
        t["positionMs"].toInteger() > t["durationMs"].toInteger())
        return false;
    const auto tracks = l["tracks"].toObject();
    qsizetype cursorEntries = 0;
    for (const auto &name : trackNames()) {
        if (!tracks[name].isObject())
            return false;
        const auto track = tracks[name].toObject();
        const auto sum = track["summary"].toObject();
        if (!track["summary"].isNull() && !summary(sum))
            return false;
        if (l["documentExportStatus"] == "ready" && !summary(sum))
            return false;
        if (track["cursor"].isNull()) {
            if (l["cursorExportStatus"] == "ready")
                return false;
            continue;
        }
        const auto cursor = track["cursor"].toObject();
        if (!summary(sum) || !cursor["lines"].isArray() || !cursor["words"].isArray())
            return false;
        const auto lines = cursor["lines"].toArray();
        const auto words = cursor["words"].toArray();
        cursorEntries += lines.size() + words.size();
        if (cursorEntries > 32768)
            return false;
        for (const auto &index : lines) {
            if (!integer(index, 0, sum["lineCount"].toInt() - 1))
                return false;
        }
        for (const auto &entry : words) {
            const auto w = entry.toObject();
            if (!integer(w["line"], 0, sum["lineCount"].toInt() - 1) || !integer(w["word"], 0, 29999) ||
                !lines.contains(w["line"]))
                return false;
        }
    }
    return true;
}
bool page(const QJsonObject &p, const QJsonObject &s, int from)
{
    const int count = s["lineCount"].toInt(-1);
    if (!integer(p["lineCount"], 0, MaximumLines) || p["lineCount"].toInt() != count ||
        !integer(p["fromLine"], 0, count) || p["fromLine"].toInt() != from ||
        p["availability"] != s["availability"] || !p["lines"].isArray())
        return false;
    const auto lines = p["lines"].toArray();
    const auto next = from + lines.size();
    if (lines.size() > 128 || next > count ||
        (next == count ? !p["nextLine"].isNull()
                       : (!integer(p["nextLine"], from + 1, count - 1) || p["nextLine"].toInt() != next)))
        return false;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (!lines[i].isObject())
            return false;
        const auto l = lines[i].toObject();
        if (!integer(l["index"], from + i, from + i) || !text(l["text"]) || !time(l["startMs"]) ||
            !time(l["endMs"]) || !l["words"].isArray())
            return false;
        const auto start = l["startMs"].toInteger(-1);
        const auto end = l["endMs"].toInteger(-1);
        if (start >= 0 && end >= 0 && end < start)
            return false;
        const auto words = l["words"].toArray();
        if (words.size() > 30000)
            return false;
        for (qsizetype j = 0; j < words.size(); ++j) {
            const auto word = words[j].toObject();
            if (!integer(word["index"], j, j) || !text(word["text"]) ||
                !integer(word["startMs"], 0, MaximumMs) ||
                !integer(word["durationMs"], 0, MaximumMs - word["startMs"].toInteger()))
                return false;
        }
    }
    return true;
}
QByteArray frame(const QJsonObject &object)
{
    const auto body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (body.isEmpty() || body.size() > FrameBytes)
        return {};
    QByteArray result(4, '\0');
    qToBigEndian(quint32(body.size()), result.data());
    result.append(body);
    return result;
}
bool Framer::expired(qint64 now) const
{
    return m_started >= 0 && (now < m_started || now - m_started >= 2000);
}
qsizetype Framer::needed() const
{
    return m_bytes.size() < 4 ? 4 - m_bytes.size() : 4 + qsizetype(m_length) - m_bytes.size();
}
bool Framer::complete() const { return !m_failed && m_length > 0 && needed() == 0; }
bool Framer::feed(QByteArrayView bytes, qint64 now)
{
    if (m_failed || expired(now) || bytes.size() > needed())
        return !(m_failed = true);
    if (!bytes.isEmpty() && m_started < 0)
        m_started = now;
    m_bytes.append(bytes.data(), bytes.size());
    if (m_bytes.size() == 4 && m_length == 0) {
        m_length = qFromBigEndian<quint32>(m_bytes.constData());
        if (m_length == 0 || m_length > FrameBytes)
            return !(m_failed = true);
    }
    return true;
}
QByteArray Framer::take()
{
    if (!complete())
        return {};
    auto body = m_bytes.mid(4);
    *this = Framer{};
    return body;
}
} // namespace CirrusObservation
