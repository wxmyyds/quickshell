#include "lyrics.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <unistd.h>

namespace P = CirrusObservation;
namespace {
const QString instance = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString view = QStringLiteral("22222222-2222-4222-8222-222222222222");
const QString song = QStringLiteral("33333333-3333-4333-8333-333333333333");
const QString otherSong = QStringLiteral("44444444-4444-4444-8444-444444444444");
const QString cirrus = QStringLiteral("org.mpris.MediaPlayer2.cirrus");
QJsonObject line(int index, const QString &text, qint64 start = -1, qint64 end = -1, QJsonArray words = {})
{
    return {{"index", index},
            {"text", text},
            {"startMs", start < 0 ? QJsonValue{} : QJsonValue(start)},
            {"endMs", end < 0 ? QJsonValue{} : QJsonValue(end)},
            {"words", words}};
}
QJsonObject summary(int count, const QString &timing = QStringLiteral("lines"), qint64 offset = 0)
{
    return {{"availability", count ? "present" : "missing"},
            {"timing", timing},
            {"providerOffsetMs", offset},
            {"malformedLines", 0},
            {"lineCount", count}};
}
QJsonObject snapshot(const QMap<QString, QJsonArray> &data = {}, qint64 position = 0,
                     const QString &track = song, int sequence = 1, int revision = 1)
{
    QJsonObject tracks;
    for (const auto &name : P::trackNames()) {
        const auto lines = data.value(name);
        const auto timing = name.endsWith("enhanced") ? QStringLiteral("words")
                            : !lines.isEmpty() && lines.first().toObject()["startMs"].isNull()
                                ? QStringLiteral("plainText")
                                : QStringLiteral("lines");
        tracks[name] = QJsonObject{{"summary", summary(int(lines.size()), timing)}, {"cursor", QJsonValue{}}};
    }
    return {{"instanceId", instance},
            {"viewEpoch", view},
            {"trackId", track},
            {"sequence", QString::number(sequence)},
            {"lyricsRevision", QString::number(revision)},
            {"availability", "ready"},
            {"playback", QJsonObject{{"state", "paused"}, {"intent", "pause"}}},
            {"track", QJsonObject{{"providerSongId", "123"}, {"entryId", QJsonValue{}}}},
            {"metadata",
             QJsonObject{{"title", "Song"}, {"artists", QJsonArray{"Artist"}}, {"album", QJsonValue{}}}},
            {"metadataExportStatus", "ready"},
            {"timing", QJsonObject{{"positionMs", position},
                                   {"songPositionMs", position},
                                   {"durationMs", 300000},
                                   {"preview", QJsonValue{}}}},
            {"clock", QJsonObject{{"sampleMonoMs", QString::number(sequence * 1000)},
                                  {"positionObservedMonoMs", QString::number(sequence * 1000)}}},
            {"settings", QJsonObject{{"volumeLinear", QJsonValue{}},
                                     {"mode", "queue"},
                                     {"order", "sequential"},
                                     {"repeat", "off"},
                                     {"fmWaiting", false},
                                     {"fmBlocked", false}}},
            {"capabilities", QJsonObject{}},
            {"failure", QJsonValue{}},
            {"lyrics", QJsonObject{{"state", "ready"},
                                   {"documentExportStatus", "ready"},
                                   {"cursorExportStatus", "notReady"},
                                   {"userOffsetMs", 0},
                                   {"providerNoLyric", QJsonValue{}},
                                   {"instrumental", QJsonValue{}},
                                   {"tracks", tracks}}}};
}
QJsonObject page(const QJsonObject &request, const QJsonArray &lines, const QJsonObject &snap)
{
    const auto name = request["track"].toString();
    const auto sum = snap["lyrics"].toObject()["tracks"].toObject()[name].toObject()["summary"].toObject();
    const int from = request["fromLine"].toInt();
    const int limit = request["maxLines"].toInt(128);
    QJsonArray slice;
    for (int i = from; i < lines.size() && i < from + limit; ++i)
        slice.append(lines[i]);
    QJsonObject p{{"v", 1},
                  {"kind", "response"},
                  {"id", request["id"]},
                  {"ok", true},
                  {"track", name},
                  {"availability", sum["availability"]},
                  {"fromLine", from},
                  {"lineCount", int(lines.size())},
                  {"lines", slice},
                  {"nextLine", from + slice.size() == lines.size()
                                   ? QJsonValue{}
                                   : QJsonValue(qint64(from + slice.size()))}};
    for (const auto &key : {"instanceId", "viewEpoch", "trackId", "lyricsRevision"})
        p[key] = snap[key];
    return p;
}
QJsonObject hello(const QJsonObject &request)
{
    return {{"v", 1},
            {"kind", "response"},
            {"id", request["id"]},
            {"ok", true},
            {"instanceId", instance},
            {"methods", QJsonArray{"hello", "getSnapshot", "subscribe", "unsubscribe", "getLyricsSlice"}},
            {"tracks", QJsonArray::fromStringList(P::trackNames())},
            {"limits", QJsonObject{{"requestBytes", 4096},
                                   {"frameBytes", 1048576},
                                   {"snapshotBytes", 262144},
                                   {"documentBytes", 8388608},
                                   {"maxLines", 128},
                                   {"clients", 8},
                                   {"pendingRequests", 4},
                                   {"framesPerSecond", 16},
                                   {"burstFrames", 16},
                                   {"helloMs", 5000},
                                   {"partialFrameMs", 2000},
                                   {"inputBytes", 16384},
                                   {"clientBytes", 2097152},
                                   {"allClientBytes", 16777216},
                                   {"slowMs", 5000}}}};
}
// Real local socket fixture, independent of Cirrus, D-Bus, network, caches and
// checkout paths. The protocol values above are literal v1 public-contract data.
struct Export {
    QTemporaryDir runtime;
    QByteArray oldRuntime = qgetenv("XDG_RUNTIME_DIR");
    QLocalServer server;
    QLocalSocket *peer = nullptr;
    QString space, leaf, endpoint;
    QString instanceId = instance;
    QMap<QString, QJsonArray> data;
    QJsonObject snap;
    QList<QJsonObject> requests;
    QList<QJsonObject> held;
    QList<qint64> received;
    QElapsedTimer clock;
    bool holdPages = false;
    bool eventBeforeSubscribeReply = false;
    int connections = 0;
    int outstanding = 0;
    int maximumOutstanding = 0;
    bool valid = false;

    explicit Export(QMap<QString, QJsonArray> tracks) : data(std::move(tracks)), snap(snapshot(data))
    {
        if (!runtime.isValid())
            return;
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
        space = runtime.path() + QStringLiteral("/com.guang233.Cirrus");
        leaf = space + QStringLiteral("/i-") + instance;
        endpoint = leaf + QStringLiteral("/s");
        clock.start();
        QObject::connect(&server, &QLocalServer::newConnection, &server, [this] {
            peer = server.nextPendingConnection();
            ++connections;
            auto *connection = peer;
            auto framer = std::make_shared<P::Framer>();
            QObject::connect(connection, &QLocalSocket::readyRead, &server, [this, framer, connection] {
                while (connection->bytesAvailable() > 0) {
                    const auto bytes =
                        connection->read(std::min(connection->bytesAvailable(), qint64(framer->needed())));
                    if (!framer->feed(bytes, clock.elapsed())) {
                        connection->abort();
                        return;
                    }
                    if (!framer->complete())
                        continue;
                    QJsonObject r;
                    if (!P::parseObject(framer->take(), r)) {
                        connection->abort();
                        return;
                    }
                    requests.append(r);
                    received.append(clock.elapsed());
                    ++outstanding;
                    maximumOutstanding = std::max(maximumOutstanding, outstanding);
                    if (r["method"] == "hello") {
                        auto response = hello(r);
                        response["instanceId"] = instanceId;
                        send(response);
                    } else if (r["method"] == "subscribe") {
                        if (eventBeforeSubscribeReply)
                            event();
                        send({{"v", 1},
                              {"kind", "response"},
                              {"id", r["id"]},
                              {"ok", true},
                              {"topics", QJsonArray{"snapshot", "lyrics", "cursor"}},
                              {"snapshot", snap}});
                    } else if (r["method"] == "getLyricsSlice") {
                        if (holdPages)
                            held.append(r);
                        else
                            send(page(r, data.value(r["track"].toString()), snap));
                    }
                }
            });
        });
        valid = enable();
    }
    ~Export()
    {
        server.close();
        if (oldRuntime.isNull())
            qunsetenv("XDG_RUNTIME_DIR");
        else
            qputenv("XDG_RUNTIME_DIR", oldRuntime);
    }
    bool enable()
    {
        leaf = space + QStringLiteral("/i-") + instanceId;
        endpoint = leaf + QStringLiteral("/s");
        snap["instanceId"] = instanceId;
        if (!QDir().mkpath(leaf) ||
            !QFile::setPermissions(space, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) ||
            !QFile::setPermissions(leaf, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
            return false;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(endpoint) ||
            !QFile::setPermissions(endpoint, QFile::ReadOwner | QFile::WriteOwner))
            return false;
        QFile file(leaf + QStringLiteral("/observation-v1.json"));
        if (!file.open(QIODevice::WriteOnly))
            return false;
        const auto bytes = QJsonDocument(QJsonObject{{"schema", 1},
                                                     {"instanceId", instanceId},
                                                     {"endpoint", endpoint},
                                                     {"pid", qint64(::getpid())}})
                               .toJson(QJsonDocument::Compact);
        return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) && file.write(bytes) == bytes.size();
    }
    void disable()
    {
        if (peer)
            peer->abort();
        server.close();
        QFile::remove(leaf + QStringLiteral("/observation-v1.json"));
        QFile::remove(endpoint);
        QDir().rmdir(leaf);
        QDir().rmdir(space);
    }
    void send(const QJsonObject &object)
    {
        if (object["kind"] == "response")
            --outstanding;
        peer->write(P::frame(object));
    }
    void event() { send({{"v", 1}, {"kind", "event"}, {"topic", "snapshot"}, {"snapshot", snap}}); }
};
} // namespace

class LyricsTest : public QObject {
    Q_OBJECT
    static void attach(Lyrics &lyrics)
    {
        lyrics.setMetadata(cirrus, {{"xesam:asText", QStringLiteral("Never expose Cirrus metadata")}},
                           QStringLiteral("selected"));
        lyrics.m_owner = QStringLiteral(":unit.1");
        lyrics.m_ownerPid = ::getpid();
        lyrics.m_instance = instance;
    }
    static void load(Lyrics &lyrics, const QJsonObject &snap, const QMap<QString, QJsonArray> &data)
    {
        lyrics.acceptSnapshot(snap);
        for (const auto &name : P::trackNames()) {
            const auto lines = data.value(name);
            for (int from = 0; from < lines.size(); from += 128) {
                const Lyrics::Request r{QStringLiteral("getLyricsSlice"),
                                        lyrics.m_binding,
                                        lyrics.m_documentEpoch,
                                        name,
                                        from,
                                        0};
                lyrics.acceptPage(
                    r, page({{"track", name}, {"fromLine", from}, {"maxLines", 128}}, lines, snap));
            }
        }
    }
    static QMap<QString, QJsonArray> twoLines()
    {
        return {
            {QStringLiteral("original.standard"), QJsonArray{line(0, QStringLiteral("First"), 1000, 1200),
                                                             line(1, QStringLiteral("Second"), 1800, 1900)}}};
    }

  private slots:
    void standardMetadataRemainsPlainText()
    {
        Lyrics lyrics(nullptr, false);
        lyrics.setMetadata(
            QStringLiteral("other"),
            {{"xesam:asText", QStringLiteral("<b>Literal</b>\r\n\n[00:01.00]Not a clock\rThird")}});
        QCOMPARE(lyrics.status(), QStringLiteral("ready"));
        QCOMPARE(lyrics.lyrics().size(), 3);
        QCOMPARE(lyrics.currentText(), QStringLiteral("<b>Literal</b>"));
        QCOMPARE(lyrics.lyrics()[1].toMap()["text"].toString(), QStringLiteral("[00:01.00]Not a clock"));
        QVERIFY(!lyrics.isSynchronized());
        QVERIFY(lyrics.document().isEmpty());
        lyrics.setMetadata(QStringLiteral("other"), {});
        QVERIFY(!lyrics.hasLyrics());
        lyrics.clearTrack();
        QCOMPARE(lyrics.status(), QStringLiteral("idle"));
    }
    void invalidMetadata_data()
    {
        QTest::addColumn<QVariant>("value");
        QTest::newRow("number") << QVariant(12);
        QTest::newRow("array") << QVariant(QStringList{QStringLiteral("not a string")});
        QTest::newRow("nul") << QVariant(QString(QChar::Null));
        QTest::newRow("surrogate") << QVariant(QString(QChar(0xd800)));
        QTest::newRow("oversized") << QVariant(QString(512 * 1024 + 1, 'x'));
    }
    void invalidMetadata()
    {
        QFETCH(QVariant, value);
        Lyrics lyrics(nullptr, false);
        lyrics.setMetadata(QStringLiteral("other"), {{"xesam:asText", value}});
        QCOMPARE(lyrics.status(), QStringLiteral("error"));
        QVERIFY(!lyrics.hasLyrics());
    }
    void framingFragmentationAndDeadlines()
    {
        const auto framed = P::frame({{"v", 1}, {"kind", "event"}});
        P::Framer parser;
        for (int i = 0; i < framed.size(); ++i) {
            QVERIFY(parser.feed(QByteArrayView(framed).sliced(i, 1), i));
            QCOMPARE(parser.complete(), i == framed.size() - 1);
        }
        QCOMPARE(parser.take(), framed.mid(4));
        QVERIFY(!parser.expired(9000));
        QVERIFY(parser.feed(QByteArrayView(framed).first(1), 10000));
        QVERIFY(!parser.expired(11999));
        QVERIFY(parser.expired(12000));
        QVERIFY(!parser.feed(QByteArrayView(framed).sliced(1, 1), 12000));
        for (quint32 length : {quint32(0), quint32(P::FrameBytes + 1), quint32(0xffffffff)}) {
            P::Framer oversized;
            QByteArray prefix(4, '\0');
            qToBigEndian(length, prefix.data());
            QVERIFY(!oversized.feed(prefix, 0));
        }
    }
    void jsonRejectsAmbiguousOrMalformedObjects()
    {
        QJsonObject object;
        QVERIFY(P::parseObject(R"({"v":1,"extra":{"new":true}})", object));
        QVERIFY(!P::parseObject(R"({"v":1,"\u0076":2})", object));
        QVERIFY(!P::parseObject(R"({"outer":{"x":1,"x":2}})", object));
        QVERIFY(!P::parseObject("[]", object));
        QVERIFY(!P::parseObject("{", object));
        QVERIFY(!P::parseObject(QByteArray("{\"x\":\"") + char(0xff) + "\"}", object));
    }
    void snapshotsAndPagesValidateThePublicContract()
    {
        const auto data = twoLines();
        const auto s = snapshot(data);
        QVERIFY(P::snapshot(s));
        const auto sum =
            s["lyrics"].toObject()["tracks"].toObject()["original.standard"].toObject()["summary"].toObject();
        auto p = page({{"track", "original.standard"}, {"fromLine", 0}}, data.first(), s);
        QVERIFY(P::page(p, sum, 0));
        p["nextLine"] = 1;
        QVERIFY(!P::page(p, sum, 0));
        p["nextLine"] = QJsonValue{};
        p["lineCount"] = 900000;
        QVERIFY(!P::page(p, sum, 0));
        auto invalid = s;
        invalid["sequence"] = 1;
        QVERIFY(!P::snapshot(invalid));
        invalid = s;
        invalid["trackId"] = QStringLiteral("not-a-uuid");
        QVERIFY(!P::snapshot(invalid));
        quint64 count = 0;
        QVERIFY(P::counter(QStringLiteral("18446744073709551615"), &count));
        QCOMPARE(count, std::numeric_limits<quint64>::max());
        QVERIFY(!P::counter(QStringLiteral("18446744073709551616")));
    }
    void preservesSixTracksWordsAndTimestampJoins()
    {
        auto data = twoLines();
        const QJsonArray words{
            QJsonObject{{"index", 0}, {"startMs", 1000}, {"durationMs", 170}, {"text", "Real"}},
            QJsonObject{{"index", 1}, {"startMs", 1170}, {"durationMs", 230}, {"text", " words"}}};
        data["original.enhanced"] = {line(0, QStringLiteral("Enhanced"), 1000, 1400, words)};
        data["translation.standard"] = {line(0, QStringLiteral("T1"), 1000),
                                        line(1, QStringLiteral("T2"), 1000)};
        data["translation.enhanced"] = {};
        data["romanization.standard"] = {line(0, QStringLiteral("R standard"), 1000)};
        data["romanization.enhanced"] = {line(0, QStringLiteral("R enhanced"), 1000)};
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        load(lyrics, snapshot(data, 1100), data);
        QCOMPARE(lyrics.status(), QStringLiteral("ready"));
        QCOMPARE(lyrics.currentText(), QStringLiteral("Enhanced"));
        QCOMPARE(lyrics.document()["displayTrack"].toString(), QStringLiteral("original.enhanced"));
        QCOMPARE(lyrics.document()["tracks"].toMap().size(), 6);
        const auto display = lyrics.lyrics().first().toMap();
        QCOMPARE(display["translation"].toString(), QStringLiteral("T1\nT2"));
        QCOMPARE(display["romanization"].toString(), QStringLiteral("R enhanced"));
        QCOMPARE(QJsonArray::fromVariantList(display["words"].toList()), words);
        QCOMPARE(lyrics.state()["activeWords"].toList().size(), 1);
    }
    void localClockProgressionLeadInGapSeekAndStaleness()
    {
        const auto data = twoLines();
        auto s = snapshot(data, 900);
        s["playback"] = QJsonObject{{"state", "playing"}, {"intent", "play"}};
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        load(lyrics, s, data);
        QSignalSpy changed(&lyrics, &Lyrics::lyricsChanged);
        const auto document = lyrics.document();
        lyrics.updateCursor(0);
        QCOMPARE(lyrics.currentLine(), -1);
        QVERIFY(lyrics.currentText().isEmpty());
        lyrics.updateCursor(100);
        QCOMPARE(lyrics.currentLine(), 0);
        lyrics.updateCursor(600); // endMs was 1200; retain the last started line
        QCOMPARE(lyrics.currentText(), QStringLiteral("First"));
        lyrics.updateCursor(900);
        QCOMPARE(lyrics.currentText(), QStringLiteral("Second"));
        lyrics.updateCursor(9000);
        QCOMPARE(lyrics.state()["songPositionMs"].toLongLong(), 1900);
        QVERIFY(lyrics.state()["stale"].toBool());
        QCOMPARE(lyrics.document(), document);
        QCOMPARE(changed.count(), 0);
        auto seek = snapshot(data, 500, song, 2);
        lyrics.acceptSnapshot(seek);
        lyrics.updateCursor(1000); // paused clock does not advance
        QCOMPARE(lyrics.currentLine(), -1);
    }
    void previewAndOffsetsApplyExactlyOnce()
    {
        const auto data = twoLines();
        auto s = snapshot(data, 400);
        auto timing = s["timing"].toObject();
        timing["songPositionMs"] = 1400;
        timing["preview"] = QJsonObject{{"startMs", 1000}, {"endMs", 301000}};
        s["timing"] = timing;
        auto l = s["lyrics"].toObject();
        l["userOffsetMs"] = 150;
        auto tracks = l["tracks"].toObject();
        auto original = tracks["original.standard"].toObject();
        original["summary"] = summary(2, QStringLiteral("lines"), 250);
        tracks["original.standard"] = original;
        l["tracks"] = tracks;
        s["lyrics"] = l;
        QVERIFY(P::snapshot(s));
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        load(lyrics, s, data);
        lyrics.updateCursor(0);
        QCOMPARE(lyrics.currentText(), QStringLiteral("First")); // 1400 - 250 - 150 = 1000
        timing["songPositionMs"] = 1399;
        timing["positionMs"] = 399;
        s["timing"] = timing;
        s["sequence"] = "2";
        lyrics.acceptSnapshot(s);
        lyrics.updateCursor(0);
        QCOMPARE(lyrics.currentLine(), -1);
    }
    void plainTextShowsFirstNonemptyLine()
    {
        const QMap<QString, QJsonArray> data{
            {"original.standard", {line(0, QStringLiteral("  ")), line(1, QStringLiteral("Untimed"))}}};
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        load(lyrics, snapshot(data), data);
        QCOMPARE(lyrics.currentLine(), 1);
        QCOMPARE(lyrics.currentText(), QStringLiteral("Untimed"));
        QVERIFY(!lyrics.isSynchronized());
    }
    void stalePagesRevisionsAndPlayerCallbacksCannotReviveText()
    {
        const auto data = twoLines();
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        load(lyrics, snapshot(data, 1000), data);
        const auto oldContext = lyrics.context();
        const Lyrics::Request old{QStringLiteral("getLyricsSlice"),
                                  lyrics.m_binding,
                                  lyrics.m_documentEpoch,
                                  QStringLiteral("original.standard"),
                                  0,
                                  0};
        auto next = snapshot(data, 0, otherSong, 2, 2);
        lyrics.acceptSnapshot(next);
        QVERIFY(!lyrics.hasLyrics());
        lyrics.acceptPage(
            old, page({{"track", "original.standard"}, {"fromLine", 0}}, data.first(), snapshot(data)));
        QVERIFY(!lyrics.hasLyrics());
        load(lyrics, next, data);
        auto stale = next;
        stale["lyricsRevision"] = "1";
        stale["sequence"] = "3";
        lyrics.acceptSnapshot(stale);
        QCOMPARE(lyrics.document()["revision"].toString(), QStringLiteral("2"));
        lyrics.setMetadata(QStringLiteral("other"), {{"xesam:asText", QStringLiteral("Other player")}});
        lyrics.receive(oldContext, {{"v", 1}, {"kind", "event"}, {"topic", "snapshot"}, {"snapshot", next}});
        QCOMPARE(lyrics.currentText(), QStringLiteral("Other player"));
        lyrics.clearTrack();
        QVERIFY(!lyrics.hasLyrics());
    }
    void unavailableAndDisabledCirrusNeverUseMetadata()
    {
        const auto data = twoLines();
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        load(lyrics, snapshot(data, 1000), data);
        auto revoked = snapshot(data, 1000, song, 2);
        revoked["availability"] = "unavailable";
        lyrics.acceptSnapshot(revoked);
        QVERIFY(!lyrics.hasLyrics());
        QVERIFY(lyrics.document().isEmpty());
        lyrics.setMetadata(cirrus, {{"xesam:asText", QStringLiteral("Stale text")}},
                           QStringLiteral("selected"));
        QVERIFY(!lyrics.hasLyrics());
        lyrics.selectOwner({});
        QVERIFY(!lyrics.hasLyrics());
        QCOMPARE(lyrics.currentLine(), -1);
    }
    void badVersionUnknownIdAndMismatchedPageFailClosed()
    {
        const auto data = twoLines();
        for (int mode = 0; mode < 3; ++mode) {
            Lyrics lyrics(nullptr, false);
            attach(lyrics);
            load(lyrics, snapshot(data, 1000), data);
            if (mode == 0)
                lyrics.receive(lyrics.context(), {{"v", 2}, {"kind", "event"}});
            else if (mode == 1)
                lyrics.receive(lyrics.context(),
                               {{"v", 1}, {"kind", "response"}, {"id", "unknown"}, {"ok", true}});
            else {
                const Lyrics::Request r{QStringLiteral("getLyricsSlice"),
                                        lyrics.m_binding,
                                        lyrics.m_documentEpoch,
                                        QStringLiteral("original.standard"),
                                        0,
                                        0};
                auto p =
                    page({{"track", "original.standard"}, {"fromLine", 0}}, data.first(), snapshot(data));
                p["trackId"] = otherSong;
                lyrics.acceptPage(r, p);
            }
            QCOMPARE(lyrics.status(), QStringLiteral("error"));
            QVERIFY(!lyrics.hasLyrics());
        }
    }
    void socketHandshakePaginationAndLocalBoundaryTimer()
    {
        auto data = twoLines();
        QJsonArray many;
        for (int i = 0; i < 260; ++i)
            many.append(line(i, QStringLiteral("Auxiliary %1").arg(i), i * 1000));
        data["translation.standard"] = many;
        Export server(data);
        QVERIFY(server.valid);
        server.eventBeforeSubscribeReply = true;
        server.snap = snapshot(data, 0);
        server.snap["playback"] = QJsonObject{{"state", "playing"}, {"intent", "play"}};
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        lyrics.discover();
        QTRY_COMPARE(lyrics.status(), QStringLiteral("ready"));
        QCOMPARE(server.requests.first()["method"].toString(), QStringLiteral("hello"));
        QCOMPARE(server.requests[1]["method"].toString(), QStringLiteral("subscribe"));
        QList<int> starts;
        for (const auto &request : std::as_const(server.requests)) {
            if (request["track"] == "translation.standard")
                starts.append(request["fromLine"].toInt());
        }
        QCOMPARE(starts, (QList<int>{0, 128, 256}));
        // A fresh snapshot reaches just before a boundary. No more snapshots are
        // sent: the actual native single-shot timer advances the displayed line.
        server.snap = snapshot(data, 950, song, 2);
        server.snap["playback"] = QJsonObject{{"state", "playing"}, {"intent", "play"}};
        server.event();
        QTRY_COMPARE(lyrics.currentText(), QStringLiteral("First"));
        QCOMPARE(server.connections, 1);
    }
    void socketSwitchDropsHeldReplyAndEnforcesPendingLimit()
    {
        Export server(twoLines());
        QVERIFY(server.valid);
        server.holdPages = true;
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        lyrics.discover();
        QTRY_COMPARE(server.held.size(), 1);
        const auto oldRequest = server.held.takeFirst();
        const auto oldSnap = server.snap;
        server.snap = snapshot(server.data, 0, otherSong, 2, 2);
        server.event();
        QTRY_COMPARE(server.held.size(), 1);
        server.send(page(oldRequest, server.data["original.standard"], oldSnap));
        QVERIFY(!lyrics.hasLyrics());
        const auto currentRequest = server.held.takeFirst();
        server.send(page(currentRequest, server.data["original.standard"], server.snap));
        QTRY_COMPARE(lyrics.status(), QStringLiteral("ready"));
        QCOMPARE(lyrics.document()["trackId"].toString(), otherSong);
        QVERIFY(server.maximumOutstanding <= 4);
        lyrics.setMetadata(QStringLiteral("other"), {{"xesam:asText", QStringLiteral("Other")}});
        QCOMPARE(lyrics.currentText(), QStringLiteral("Other"));
    }
    void socketDisableAndDirectoryRecreationAreObserved()
    {
        Export server(twoLines());
        QVERIFY(server.valid);
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        lyrics.discover();
        QTRY_COMPARE(lyrics.status(), QStringLiteral("ready"));
        server.disable();
        QTRY_VERIFY(!lyrics.hasLyrics());
        QVERIFY(lyrics.document().isEmpty());
        lyrics.setMetadata(cirrus, {{"xesam:asText", QStringLiteral("Must stay cleared")}},
                           QStringLiteral("selected"));
        QVERIFY(!lyrics.hasLyrics());
        server.instanceId = QStringLiteral("55555555-5555-4555-8555-555555555555");
        QVERIFY(server.enable());
        QTRY_COMPARE(lyrics.status(), QStringLiteral("ready"));
        QCOMPARE(lyrics.document()["instanceId"].toString(), server.instanceId);
        QCOMPARE(server.connections, 2);
    }
    void discoveryRejectsWrongPermissionsSymlinksAndPeerPid()
    {
        Export server(twoLines());
        QVERIFY(server.valid);
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        QVERIFY(QFile::setPermissions(server.leaf, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                                       QFile::ReadOther));
        lyrics.discover();
        QVERIFY(!lyrics.m_socket);
        QVERIFY(QFile::setPermissions(server.leaf, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = server.leaf + QStringLiteral("/observation-v1.json");
        const auto renamed = server.runtime.path() + QStringLiteral("/private-manifest");
        QVERIFY(QFile::rename(path, renamed));
        QVERIFY(QFile::link(renamed, path));
        lyrics.discover();
        QVERIFY(!lyrics.m_socket);
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(renamed, path));
        // The manifest claims our PID, but peer authentication uses SO_PEERCRED
        // compared with the selected bus owner's PID, never this hint.
        lyrics.m_ownerPid = ::getpid() + 1;
        lyrics.discover();
        QTRY_COMPARE(server.connections, 1);
        QTRY_VERIFY(!lyrics.m_socket);
        QVERIFY(server.requests.isEmpty());
        QVERIFY(!lyrics.hasLyrics());
    }
    void manyPagesRespectTokenBucketWithoutReconnect()
    {
        QMap<QString, QJsonArray> data;
        for (const auto &name : P::trackNames()) {
            QJsonArray lines;
            for (int i = 0; i < 385; ++i)
                lines.append(line(i, QStringLiteral("Line %1").arg(i), i * 100));
            data[name] = lines;
        }
        Export server(data);
        QVERIFY(server.valid);
        Lyrics lyrics(nullptr, false);
        attach(lyrics);
        lyrics.discover();
        QTRY_COMPARE(lyrics.status(), QStringLiteral("ready"));
        QCOMPARE(server.requests.size(), 26); // hello, subscribe, 6 * 4 pages
        QCOMPARE(server.connections, 1);
        QVERIFY(server.maximumOutstanding <= 4);
        for (qsizetype i = 0; i < server.received.size(); ++i) {
            for (qsizetype j = i; j < server.received.size(); ++j) {
                const auto milliseconds = server.received[j] - server.received[i];
                // v1 permits a 16-frame burst plus 16 new tokens per second.
                QVERIFY(double(j - i + 1) <= 16.0 + double(milliseconds) * 0.016 + 1.0);
            }
        }
    }
};

QTEST_GUILESS_MAIN(LyricsTest)
#include "lyrics_test.moc"
