#include "lyrics.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace P = CirrusObservation;
namespace {
// Discovery is confined to the runtime namespace. Open every path component
// without following links, then inspect the opened inode before reading it.
struct Fd {
    int value = -1;
    explicit Fd(int value = -1) : value(value) {}
    ~Fd()
    {
        if (value >= 0)
            ::close(value);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
bool privateNode(int fd, mode_t kind, mode_t mode)
{
    struct stat s{};
    return fd >= 0 && ::fstat(fd, &s) == 0 && (s.st_mode & S_IFMT) == kind && s.st_uid == ::getuid() &&
           (s.st_mode & 07777) == mode;
}
int runtimeDirectory(const QString &path)
{
    if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path)
        return -1;
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const auto &part : path.split('/', Qt::SkipEmptyParts)) {
        if (fd < 0)
            return -1;
        const int next = ::openat(fd, QFile::encodeName(part).constData(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd);
        fd = next;
    }
    if (!privateNode(fd, S_IFDIR, 0700)) {
        if (fd >= 0)
            ::close(fd);
        return -1;
    }
    return fd;
}
bool manifest(int space, const QString &leaf, const QString &endpoint, QString &instance)
{
    Fd dir(::openat(space, QFile::encodeName(leaf).constData(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!privateNode(dir.value, S_IFDIR, 0700))
        return false;
    Fd file(::openat(dir.value, "observation-v1.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat info{}, socket{};
    if (!privateNode(file.value, S_IFREG, 0600) || ::fstat(file.value, &info) != 0 || info.st_size <= 0 ||
        info.st_size > P::RequestBytes || ::fstatat(dir.value, "s", &socket, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISSOCK(socket.st_mode) || socket.st_uid != ::getuid() || (socket.st_mode & 07777) != 0600)
        return false;
    QByteArray bytes(P::RequestBytes + 1, '\0');
    const auto n = ::read(file.value, bytes.data(), size_t(bytes.size()));
    if (n <= 0 || n > P::RequestBytes || n != info.st_size)
        return false;
    bytes.resize(n);
    QJsonObject object;
    if (!P::parseObject(bytes, object) || !P::integer(object["schema"], 1, 1) ||
        !P::uuid(object["instanceId"]) || object["endpoint"] != endpoint ||
        !P::integer(object["pid"], 1, 2147483647) ||
        leaf != QStringLiteral("i-") + object["instanceId"].toString())
        return false;
    instance = object["instanceId"].toString();
    return true;
}
QString runtimePath() { return qEnvironmentVariable("XDG_RUNTIME_DIR"); }
QString namespacePath() { return runtimePath() + QStringLiteral("/com.guang233.Cirrus"); }
QString selectedService() { return QStringLiteral("org.mpris.MediaPlayer2.cirrus"); }
bool validPlayerText(const QVariant &value)
{
    const auto text = value.toString();
    return value.metaType().id() == QMetaType::QString && text.size() <= 512 * 1024 && text.isValidUtf16() &&
           !text.contains(QChar::Null);
}
QString responseId(const QJsonObject &value) { return value["id"].toString(); }
} // namespace

Lyrics::Lyrics(QObject *parent, bool useSessionBus) : QObject(parent), m_useSessionBus(useSessionBus)
{
    m_monotonic.start();
    m_discoveryTimer.setSingleShot(true);
    m_lineTimer.setSingleShot(true);
    m_lineTimer.setTimerType(Qt::PreciseTimer);
    m_maintenanceTimer.setInterval(100);
    connect(&m_discoveryTimer, &QTimer::timeout, this, &Lyrics::discover);
    connect(&m_maintenanceTimer, &QTimer::timeout, this, &Lyrics::maintain);
    connect(&m_lineTimer, &QTimer::timeout, this, [this] { updateCursor(); });
    const auto changed = [this] {
        if (!isCirrus())
            return;
        // Deletion of the selected export revokes it immediately, even if an
        // already queued socket callback still contains old lyric data.
        if (m_socket) {
            Fd runtime(runtimeDirectory(runtimePath()));
            Fd space(::openat(runtime.value, "com.guang233.Cirrus",
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            QString instance;
            if (!privateNode(space.value, S_IFDIR, 0700) ||
                !manifest(space.value, QStringLiteral("i-") + m_instance, m_endpoint, instance) ||
                instance != m_instance) {
                fail();
                return;
            }
        }
        if (!m_socket)
            m_discoveryTimer.start(50);
    };
    connect(&m_files, &QFileSystemWatcher::directoryChanged, this, changed);
    connect(&m_files, &QFileSystemWatcher::fileChanged, this, changed);
}
Lyrics::~Lyrics() { stopPlayer(); }
bool Lyrics::isCirrus() const
{
    return m_service == selectedService() || m_service.startsWith(selectedService() + '.');
}
bool Lyrics::isCurrent(Context c) const
{
    return c.player == m_playerEpoch && c.socket == m_socketEpoch && isCirrus();
}
QString Lyrics::currentText() const
{
    return m_currentLine >= 0 && m_currentLine < m_lyrics.size()
               ? m_lyrics[m_currentLine].toMap().value(QStringLiteral("text")).toString()
               : QString{};
}
void Lyrics::publish(const QString &status, QVariantList lines, const QString &error, QVariantMap document,
                     QVariantMap state, int currentLine, bool synchronized)
{
    const bool statusChangedNow = m_status != status;
    const bool lyricsChangedNow = m_lyrics != lines;
    const bool errorChangedNow = m_error != error;
    const bool syncChangedNow = m_document != document || m_state != state || m_currentLine != currentLine ||
                                m_synchronized != synchronized;
    m_status = status;
    m_lyrics = std::move(lines);
    m_error = error;
    m_document = std::move(document);
    m_state = std::move(state);
    m_currentLine = currentLine;
    m_synchronized = synchronized;
    if (statusChangedNow)
        emit statusChanged();
    if (lyricsChangedNow)
        emit lyricsChanged();
    if (errorChangedNow)
        emit errorChanged();
    if (syncChangedNow || lyricsChangedNow)
        emit synchronizationChanged();
}
void Lyrics::publishMetadata(const QVariantMap &metadata)
{
    const auto value = metadata.value(QStringLiteral("xesam:asText"));
    if (!value.isValid()) {
        publish(QStringLiteral("empty"), {});
        return;
    }
    if (!validPlayerText(value)) {
        publish(QStringLiteral("error"), {}, tr("Invalid player lyrics"));
        return;
    }
    QVariantList lines;
    static const QRegularExpression breaks(QStringLiteral("\\r\\n|[\\r\\n]"));
    for (const auto &line : value.toString().split(breaks)) {
        if (!line.trimmed().isEmpty())
            lines.append(QVariantMap{{QStringLiteral("text"), line}});
    }
    const auto status = lines.isEmpty() ? QStringLiteral("empty") : QStringLiteral("ready");
    const int current = lines.isEmpty() ? -1 : 0;
    publish(status, std::move(lines), {}, {}, {}, current);
}
void Lyrics::setMetadata(const QString &dbusName, const QVariantMap &metadata, const QString &playerIdentity)
{
    if (dbusName.isEmpty()) {
        clearTrack();
        return;
    }
    const bool changed = dbusName != m_service || playerIdentity != m_playerIdentity;
    if (changed) {
        stopPlayer();
        m_service = dbusName;
        m_playerIdentity = playerIdentity;
    }
    if (!isCirrus()) {
        publishMetadata(metadata);
        return;
    }
    // Cirrus metadata can outlive disabled sharing. Never consume its asText or
    // equate its MPRIS object-path track ID with an observation UUID.
    if (changed) {
        publish(QStringLiteral("idle"), {});
        startOwnerWatch();
    }
}
void Lyrics::resetDocument()
{
    ++m_documentEpoch;
    m_tracks.clear();
    m_documentBytes = 13; // {"tracks":{}} normalized document envelope
    m_textUnits = 0;
    m_displayIndices.clear();
    m_displayStarts.clear();
    m_documentReady = false;
    m_displayTrack.clear();
    m_lineTimer.stop();
}
void Lyrics::resetSocket()
{
    ++m_socketEpoch;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_maintenanceTimer.stop();
    m_framer = P::Framer{};
    m_readQueued = false;
    m_pending.clear();
    m_hello = false;
    m_subscribed = false;
    m_instance.clear();
    m_endpoint.clear();
    m_snapshot = {};
    m_snapshotVariant.clear();
    m_binding.clear();
    m_haveSequence = false;
    m_sequence = m_revision = 0;
    m_tokens = 16;
    m_tokenAt = m_monotonic.elapsed();
    m_requestsAfter = 0;
    resetDocument();
}
void Lyrics::stopPlayer()
{
    ++m_playerEpoch;
    resetSocket();
    m_discoveryTimer.stop();
    watchDiscovery({});
    delete m_serviceWatcher;
    m_serviceWatcher = nullptr;
    m_owner.clear();
    m_ownerPid = 0;
    m_pidPending = false;
    m_candidates.clear();
    m_retryMs = 250;
    m_service.clear();
    m_playerIdentity.clear();
}
void Lyrics::clearTrack()
{
    stopPlayer();
    publish(QStringLiteral("idle"), {});
}
void Lyrics::startOwnerWatch()
{
    if (!m_useSessionBus)
        return;
    if (!m_bus)
        m_bus = std::make_unique<QDBusConnection>(QDBusConnection::sessionBus());
    m_serviceWatcher =
        new QDBusServiceWatcher(m_service, *m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &service, const QString &, const QString &owner) {
                if (service == m_service)
                    selectOwner(owner);
            });
    const auto epoch = m_playerEpoch;
    auto call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    call << m_service;
    auto *watcher = new QDBusPendingCallWatcher(m_bus->asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, epoch] {
        const QDBusPendingReply<QString> reply = *watcher;
        watcher->deleteLater();
        if (epoch == m_playerEpoch)
            selectOwner(reply.isError() ? QString{} : reply.value());
    });
}
void Lyrics::selectOwner(const QString &owner)
{
    if (m_owner == owner && !owner.isEmpty())
        return;
    ++m_playerEpoch;
    resetSocket();
    m_owner = owner;
    m_ownerPid = 0;
    m_pidPending = false;
    m_candidates.clear();
    m_discoveryTimer.stop();
    publish(QStringLiteral("idle"), {});
    if (owner.isEmpty()) {
        watchDiscovery({});
        return;
    }
    resolveOwnerPid();
}
void Lyrics::resolveOwnerPid()
{
    if (!m_bus || m_owner.isEmpty() || m_pidPending)
        return;
    m_pidPending = true;
    const auto epoch = m_playerEpoch;
    auto call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixProcessID"));
    call << m_owner;
    auto *watcher = new QDBusPendingCallWatcher(m_bus->asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, epoch] {
        const QDBusPendingReply<uint> reply = *watcher;
        watcher->deleteLater();
        if (epoch != m_playerEpoch)
            return;
        m_pidPending = false;
        if (!reply.isError() && reply.value() > 0) {
            m_ownerPid = reply.value();
            discover();
        } else {
            retryDiscovery();
        }
    });
}
void Lyrics::watchDiscovery(const QStringList &paths)
{
    const auto old = m_files.directories() + m_files.files();
    if (!old.isEmpty())
        m_files.removePaths(old);
    if (!paths.isEmpty())
        m_files.addPaths(paths);
}
void Lyrics::retryDiscovery()
{
    if (!isCirrus() || m_owner.isEmpty())
        return;
    if (!m_discoveryTimer.isActive()) {
        m_discoveryTimer.start(m_retryMs);
        m_retryMs = std::min(m_retryMs * 2, 3000);
    }
}
void Lyrics::discover()
{
    if (!isCirrus() || m_owner.isEmpty() || m_socket)
        return;
    if (m_ownerPid <= 0) {
        resolveOwnerPid();
        return;
    }
    m_candidates.clear();
    QStringList watched;
    Fd runtime(runtimeDirectory(runtimePath()));
    if (runtime.value >= 0)
        watched.append(runtimePath());
    Fd space(::openat(runtime.value, "com.guang233.Cirrus", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (privateNode(space.value, S_IFDIR, 0700)) {
        watched.append(namespacePath());
        const auto leaves =
            QDir(namespacePath())
                .entryList({QStringLiteral("i-*")}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const auto &leaf : leaves) {
            const auto endpoint = namespacePath() + '/' + leaf + QStringLiteral("/s");
            QString instance;
            if (!manifest(space.value, leaf, endpoint, instance))
                continue;
            m_candidates.append({endpoint, instance});
            watched.append(namespacePath() + '/' + leaf);
            watched.append(namespacePath() + '/' + leaf + QStringLiteral("/observation-v1.json"));
        }
    }
    watchDiscovery(watched);
    connectNext();
}
void Lyrics::connectNext()
{
    resetSocket();
    if (!isCirrus() || m_ownerPid <= 0)
        return;
    if (m_candidates.isEmpty()) {
        retryDiscovery();
        return;
    }
    const auto endpoint = m_candidates.takeFirst();
    m_endpoint = endpoint.path;
    m_instance = endpoint.instance;
    m_socket = new QLocalSocket(this);
    m_socket->setReadBufferSize(P::FrameBytes + 4);
    m_connectedAt = m_monotonic.elapsed();
    const auto c = context();
    connect(m_socket, &QLocalSocket::connected, this, [this, c] {
        if (!isCurrent(c))
            return;
#ifdef Q_OS_LINUX
        struct ucred peer{};
        socklen_t length = sizeof(peer);
        if (::getsockopt(int(m_socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 ||
            length != sizeof(peer) || peer.uid != ::getuid() || peer.pid != m_ownerPid) {
            fail();
            return;
        }
#else
        fail();
        return;
#endif
        pump();
    });
    connect(m_socket, &QLocalSocket::readyRead, this, [this, c] { readSocket(c); });
    connect(m_socket, &QLocalSocket::disconnected, this, [this, c] {
        if (isCurrent(c))
            fail();
    });
    connect(m_socket, &QLocalSocket::errorOccurred, this, [this, c] {
        if (isCurrent(c))
            fail();
    });
    m_maintenanceTimer.start();
    m_socket->connectToServer(m_endpoint, QIODevice::ReadWrite);
}
void Lyrics::fail(bool invalid)
{
    resetSocket();
    publish(invalid ? QStringLiteral("error") : QStringLiteral("empty"), {},
            invalid ? tr("Invalid player lyrics") : QString{});
    const auto c = context();
    if (!m_candidates.isEmpty())
        QTimer::singleShot(0, this, [this, c] {
            if (isCurrent(c))
                connectNext();
        });
    else
        retryDiscovery();
}
void Lyrics::readSocket(Context c)
{
    if (!isCurrent(c) || !m_socket || m_readQueued)
        return;
    // At most one <=1MiB frame per event-loop turn. No blocking waits, readAll,
    // recursive draining or synchronous socket calls on the QML thread.
    if (m_socket->bytesAvailable() > 0) {
        const auto bytes = m_socket->read(std::min(m_socket->bytesAvailable(), qint64(m_framer.needed())));
        if (!m_framer.feed(bytes, m_monotonic.elapsed())) {
            fail(true);
            return;
        }
    }
    if (m_framer.complete()) {
        QJsonObject message;
        if (!P::parseObject(m_framer.take(), message)) {
            fail(true);
            return;
        }
        receive(c, message);
    }
    if (isCurrent(c) && m_socket && m_socket->bytesAvailable() > 0) {
        m_readQueued = true;
        QTimer::singleShot(0, this, [this, c] {
            if (!isCurrent(c))
                return;
            m_readQueued = false;
            readSocket(c);
        });
    }
}
QString Lyrics::binding(const QJsonObject &s) const
{
    return s["instanceId"].toString() + '/' + s["viewEpoch"].toString() + '/' + s["trackId"].toString() +
           '/' + s["lyricsRevision"].toString();
}
void Lyrics::request(const QString &method, const QString &track, int from)
{
    if (!m_socket || m_pending.size() >= 4 || m_tokens < 1)
        return;
    const auto id = QStringLiteral("r") + QString::number(++m_nextRequest);
    QJsonObject object{{"v", 1}, {"kind", "request"}, {"id", id}, {"method", method}};
    if (method == "subscribe")
        object["topics"] = QJsonArray{"snapshot", "lyrics", "cursor"};
    if (method == "getLyricsSlice") {
        for (const auto &key : {"instanceId", "viewEpoch", "trackId", "lyricsRevision"})
            object[key] = m_snapshot[key];
        object["track"] = track;
        object["fromLine"] = from;
        object["maxLines"] = 128;
    }
    const auto bytes = P::frame(object);
    if (bytes.isEmpty() || bytes.size() > P::RequestBytes + 4 || m_socket->bytesToWrite() > 8192) {
        fail(true);
        return;
    }
    m_tokens -= 1;
    m_pending.insert(id, {method, m_binding, m_documentEpoch, track, from, m_monotonic.elapsed()});
    if (m_socket->write(bytes) != bytes.size())
        fail();
}
void Lyrics::pump()
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState)
        return;
    const auto now = m_monotonic.elapsed();
    m_tokens = std::min(16.0, m_tokens + double(now - m_tokenAt) * 0.016);
    m_tokenAt = now;
    if (now < m_requestsAfter || m_tokens < 1 || m_pending.size() >= 4)
        return;
    // A single sequential page stream leaves room for late replies from retired
    // views, respects the four-outstanding limit and cannot flood the server.
    for (const auto &r : std::as_const(m_pending)) {
        if (r.method == "hello" || r.method == "subscribe" ||
            (r.method == "getLyricsSlice" && r.documentEpoch == m_documentEpoch))
            return;
    }
    if (!m_hello) {
        request(QStringLiteral("hello"));
        return;
    }
    if (!m_subscribed) {
        request(QStringLiteral("subscribe"));
        return;
    }
    for (auto it = m_tracks.cbegin(); it != m_tracks.cend(); ++it) {
        if (!it->complete) {
            request(QStringLiteral("getLyricsSlice"), it.key(), int(it->lines.size()));
            return;
        }
    }
}
void Lyrics::maintain()
{
    const auto now = m_monotonic.elapsed();
    if ((!m_hello && now - m_connectedAt >= 5000) || m_framer.expired(now)) {
        fail();
        return;
    }
    for (const auto &r : std::as_const(m_pending)) {
        if (now - r.sent >= 5000) {
            fail();
            return;
        }
    }
    pump();
}
void Lyrics::receive(Context c, const QJsonObject &message)
{
    if (!isCurrent(c))
        return;
    if (!P::integer(message["v"], 1, 1)) {
        fail(true);
        return;
    }
    if (message["kind"] == "event") {
        bool subscribing = false;
        for (const auto &pending : std::as_const(m_pending))
            subscribing = subscribing || pending.method == "subscribe";
        if (!m_hello || (!m_subscribed && !subscribing) ||
            !QStringList{"snapshot", "lyrics", "cursor"}.contains(message["topic"].toString()) ||
            !message["snapshot"].isObject()) {
            fail(true);
            return;
        }
        acceptSnapshot(message["snapshot"].toObject());
    } else if (message["kind"] == "response") {
        const auto id = responseId(message);
        if (!m_pending.contains(id) || !message["ok"].isBool()) {
            fail(true);
            return;
        }
        const auto r = m_pending.take(id);
        // Replies are correlated before being discarded, so an unknown ID never
        // silently becomes an event. Retired view replies cannot mutate new data.
        if (r.method == "getLyricsSlice" && (r.binding != m_binding || r.documentEpoch != m_documentEpoch)) {
            pump();
            return;
        }
        if (!message["ok"].toBool()) {
            const auto code = message["error"].toObject()["code"].toString();
            if (r.method == "getLyricsSlice" && (code == "StaleView" || code == "LyricsNotReady")) {
                resetDocument();
                publish(QStringLiteral("loading"), {});
            } else if (r.method == "subscribe" && code == "StaleView") {
                m_subscribed = false;
                m_requestsAfter = m_monotonic.elapsed() + 100;
            } else if (code == "Busy" || code == "RateLimited") {
                m_requestsAfter = m_monotonic.elapsed() + 1000;
            } else {
                fail(code != "NotReady");
                return;
            }
        } else if (r.method == "hello") {
            const auto limits = message["limits"].toObject();
            const auto methods = message["methods"].toArray();
            const auto tracks = message["tracks"].toArray();
            bool valid =
                message["instanceId"] == m_instance &&
                P::integer(limits["requestBytes"], P::RequestBytes, P::RequestBytes) &&
                P::integer(limits["frameBytes"], P::FrameBytes, P::FrameBytes) &&
                P::integer(limits["snapshotBytes"], P::SnapshotBytes, P::SnapshotBytes) &&
                P::integer(limits["documentBytes"], P::DocumentBytes, P::DocumentBytes) &&
                P::integer(limits["maxLines"], 128, 128) && P::integer(limits["pendingRequests"], 4, 4) &&
                P::integer(limits["framesPerSecond"], 16, 16) && P::integer(limits["burstFrames"], 16, 16);
            for (const auto &method : {"hello", "subscribe", "getLyricsSlice"})
                valid = valid && methods.contains(QString::fromLatin1(method));
            for (const auto &track : P::trackNames())
                valid = valid && tracks.contains(track);
            if (!valid) {
                fail(true);
                return;
            }
            m_hello = true;
        } else if (r.method == "subscribe") {
            const auto topics = message["topics"].toArray();
            if (!topics.contains("snapshot") || !topics.contains("lyrics") || !topics.contains("cursor")) {
                fail(true);
                return;
            }
            m_subscribed = true;
            m_retryMs = 250;
            acceptSnapshot(message["snapshot"].toObject());
        } else if (r.method == "getLyricsSlice") {
            acceptPage(r, message);
        }
    } else {
        fail(true);
        return;
    }
    if (isCurrent(c))
        pump();
}
void Lyrics::acceptSnapshot(const QJsonObject &s)
{
    if (QJsonDocument(s).toJson(QJsonDocument::Compact).size() > P::SnapshotBytes || !P::snapshot(s) ||
        s["instanceId"] != m_instance) {
        fail(true);
        return;
    }
    quint64 sequence = 0, revision = 0;
    P::counter(s["sequence"], &sequence);
    P::counter(s["lyricsRevision"], &revision);
    if (m_haveSequence && sequence <= m_sequence)
        return;
    if (s["viewEpoch"] == m_snapshot["viewEpoch"] && s["trackId"] == m_snapshot["trackId"] &&
        revision < m_revision)
        return;
    quint64 previousSample = 0, nextSample = 0;
    if (P::counter(m_snapshot["clock"].toObject()["sampleMonoMs"], &previousSample) &&
        P::counter(s["clock"].toObject()["sampleMonoMs"], &nextSample) && nextSample < previousSample) {
        fail(true);
        return;
    }
    m_haveSequence = true;
    m_sequence = sequence;
    m_revision = revision;
    const auto key = binding(s);
    const auto l = s["lyrics"].toObject();
    const bool usable = s["availability"] == "ready" && !s["trackId"].isNull() && l["state"] == "ready" &&
                        l["documentExportStatus"] == "ready";
    if (key != m_binding || !usable) {
        resetDocument();
        publish(QStringLiteral("idle"), {});
    }
    m_binding = key;
    m_snapshot = s;
    m_snapshotVariant = s.toVariantMap();
    m_receivedAt = m_monotonic.elapsed();
    if (!usable) {
        const bool loading = s["availability"] == "ready" &&
                             (l["state"] == "loading" || l["documentExportStatus"] == "preparing");
        const bool error = s["availability"] == "ready" &&
                           (l["state"] == "failed" || l["documentExportStatus"] == "invalidData" ||
                            l["documentExportStatus"] == "limitExceeded");
        publish(error     ? QStringLiteral("error")
                : loading ? QStringLiteral("loading")
                          : QStringLiteral("empty"),
                {}, error ? tr("Invalid player lyrics") : QString{});
        return;
    }
    const auto tracks = l["tracks"].toObject();
    if (m_tracks.isEmpty()) {
        for (const auto &name : P::trackNames()) {
            const auto summary = tracks[name].toObject()["summary"].toObject();
            m_documentBytes += (m_tracks.isEmpty() ? 0 : 1) + name.size() + 3 + 11 +
                               QJsonDocument(summary).toJson(QJsonDocument::Compact).size();
            m_tracks.insert(name, {summary, {}, summary["lineCount"].toInt() == 0, 0});
        }
        publish(QStringLiteral("loading"), {});
        assembleDocument();
    } else {
        for (const auto &name : P::trackNames()) {
            if (m_tracks[name].summary != tracks[name].toObject()["summary"].toObject()) {
                fail(true);
                return;
            }
        }
    }
    if (m_documentReady && validateCursors())
        updateCursor();
}
void Lyrics::acceptPage(const Request &r, const QJsonObject &p)
{
    if (r.binding != m_binding || r.documentEpoch != m_documentEpoch)
        return;
    if (binding(p) != m_binding || p["track"] != r.track || !m_tracks.contains(r.track) ||
        !P::page(p, m_tracks[r.track].summary, r.from)) {
        fail(true);
        return;
    }
    auto &track = m_tracks[r.track];
    const auto lines = p["lines"].toArray();
    if (track.lines.size() != r.from) {
        fail(true);
        return;
    }
    // Count normalized line records across all six source tracks, not a display
    // reconstruction or repeated page envelope. The v1 document cap is 8MiB.
    for (const auto &line : lines) {
        const auto object = line.toObject();
        m_documentBytes +=
            QJsonDocument(object).toJson(QJsonDocument::Compact).size() + (track.lines.isEmpty() ? 0 : 1);
        m_textUnits += object["text"].toString().size();
        const auto words = object["words"].toArray();
        track.wordCount += words.size();
        for (const auto &word : words)
            m_textUnits += word.toObject()["text"].toString().size();
        if (m_documentBytes > P::DocumentBytes || m_textUnits > 1024 * 1024 || track.wordCount > 30000) {
            fail(true);
            return;
        }
        track.lines.append(line);
    }
    track.complete = p["nextLine"].isNull();
    assembleDocument();
}
QString Lyrics::preferredTrack(const QString &kind) const
{
    const auto enhanced = kind + QStringLiteral(".enhanced");
    // Content preference, not error recovery: every source track was validated
    // and fetched; never substitute standard when an enhanced fetch is invalid.
    return m_tracks.value(enhanced).summary["availability"] == "present" ? enhanced
                                                                         : kind + QStringLiteral(".standard");
}
void Lyrics::assembleDocument()
{
    if (m_documentReady || m_tracks.size() != 6)
        return;
    for (const auto &track : std::as_const(m_tracks)) {
        if (!track.complete)
            return;
    }
    m_displayTrack = preferredTrack(QStringLiteral("original"));
    const auto original = m_tracks.value(m_displayTrack);
    QVariantList display;
    // Align independent source indexes by their effective song timestamps.
    // Equal timestamps may have multiple auxiliary lines, all of which survive.
    QMap<qint64, QStringList> translations, romanizations;
    const auto auxiliary = [this](const QString &kind, QMap<qint64, QStringList> &out) {
        const auto track = m_tracks.value(preferredTrack(kind));
        const auto offset = track.summary["providerOffsetMs"].toInteger();
        for (const auto &entry : track.lines) {
            const auto line = entry.toObject();
            if (!line["startMs"].isNull())
                out[line["startMs"].toInteger() + offset].append(line["text"].toString());
        }
    };
    auxiliary(QStringLiteral("translation"), translations);
    auxiliary(QStringLiteral("romanization"), romanizations);
    for (qsizetype i = 0; i < original.lines.size(); ++i)
        m_displayIndices.append(int(i));
    if (original.summary["timing"] != "plainText") {
        std::stable_sort(m_displayIndices.begin(), m_displayIndices.end(), [&original](int a, int b) {
            const auto x = original.lines[a].toObject()["startMs"].toInteger(P::MaximumMs + 1);
            const auto y = original.lines[b].toObject()["startMs"].toInteger(P::MaximumMs + 1);
            return x < y;
        });
    }
    for (const auto source : std::as_const(m_displayIndices)) {
        const auto line = original.lines[source].toObject();
        if (!line["startMs"].isNull())
            m_displayStarts.append(line["startMs"].toInteger());
        auto value = line.toVariantMap();
        const auto start = line["startMs"].toInteger(-1);
        const auto key = start + original.summary["providerOffsetMs"].toInteger();
        value["translation"] = start < 0 ? QString{} : translations.value(key).join('\n');
        value["romanization"] = start < 0 ? QString{} : romanizations.value(key).join('\n');
        display.append(value);
    }
    QVariantMap tracks;
    for (auto it = m_tracks.cbegin(); it != m_tracks.cend(); ++it)
        tracks[it.key()] =
            QVariantMap{{"summary", it->summary.toVariantMap()}, {"lines", it->lines.toVariantList()}};
    QVariantMap document{{"instanceId", m_snapshot["instanceId"].toString()},
                         {"viewEpoch", m_snapshot["viewEpoch"].toString()},
                         {"trackId", m_snapshot["trackId"].toString()},
                         {"revision", m_snapshot["lyricsRevision"].toString()},
                         {"tracks", tracks},
                         {"displayTrack", m_displayTrack},
                         {"timing", original.summary["timing"].toString()},
                         {"lines", display}};
    if (!validateCursors())
        return;
    m_documentReady = true;
    publish(display.isEmpty() ? QStringLiteral("empty") : QStringLiteral("ready"), display, {}, document);
    updateCursor();
}
bool Lyrics::validateCursors()
{
    const auto tracks = m_snapshot["lyrics"].toObject()["tracks"].toObject();
    for (const auto &name : P::trackNames()) {
        const auto words = tracks[name].toObject()["cursor"].toObject()["words"].toArray();
        const auto &lines = m_tracks[name].lines;
        for (const auto &entry : words) {
            const auto word = entry.toObject();
            const int line = word["line"].toInt(-1);
            if (line < 0 || line >= lines.size() || word["word"].toInt(-1) < 0 ||
                word["word"].toInt() >= lines[line].toObject()["words"].toArray().size()) {
                fail(true);
                return false;
            }
        }
    }
    return true;
}
void Lyrics::updateCursor(qint64 elapsed)
{
    m_lineTimer.stop();
    if (!m_documentReady)
        return;
    if (elapsed < 0)
        elapsed = std::max(qint64(0), m_monotonic.elapsed() - m_receivedAt);
    const auto timing = m_snapshot["timing"].toObject();
    const auto playback = m_snapshot["playback"].toObject();
    const auto lyric = m_snapshot["lyrics"].toObject();
    const bool playing = playback["state"] == "playing";
    const auto advance = playing ? std::min(elapsed, qint64(1000)) : 0;
    // songPositionMs already includes preview.startMs. Offsets are subtracted
    // exactly once below; snapshot cursors already include them and are not shifted.
    auto song = timing["songPositionMs"].toInteger(-1);
    if (song >= 0) {
        song += advance;
        if (!timing["durationMs"].isNull()) {
            const auto preview = timing["preview"].toObject();
            song = std::min(song, timing["durationMs"].toInteger() + preview["startMs"].toInteger());
        }
    }
    const auto &track = m_tracks[m_displayTrack];
    const bool timed = track.summary["timing"] != "plainText";
    const auto local =
        song - track.summary["providerOffsetMs"].toInteger() - lyric["userOffsetMs"].toInteger();
    int current = -1;
    qint64 next = 1001;
    QVariantList activeLines, activeWords;
    if (!timed) {
        for (qsizetype i = 0; i < m_lyrics.size(); ++i) {
            if (!m_lyrics[i].toMap().value(QStringLiteral("text")).toString().trimmed().isEmpty()) {
                current = int(i);
                break;
            }
        }
    } else if (song >= 0) {
        // Last-started line deliberately survives gaps and its end timestamp.
        const auto upper = std::upper_bound(m_displayStarts.cbegin(), m_displayStarts.cend(), local);
        const auto low = std::distance(m_displayStarts.cbegin(), upper);
        current = int(low) - 1;
        if (low < m_displayStarts.size())
            next = std::min(next, m_displayStarts[low] - local);
        if (current >= 0) {
            activeLines.append(current);
            const auto words = track.lines[m_displayIndices[current]].toObject()["words"].toArray();
            for (const auto &entry : words) {
                const auto word = entry.toObject();
                const auto start = word["startMs"].toInteger();
                const auto end = start + word["durationMs"].toInteger();
                if (start <= local && local < end)
                    activeWords.append(QVariantMap{{"line", current}, {"word", word["index"].toInt()}});
                if (start > local)
                    next = std::min(next, start - local);
                if (end > local)
                    next = std::min(next, end - local);
            }
        }
    }
    const QVariantMap state{{"trackId", m_snapshot["trackId"].toString()},
                            {"revision", m_snapshot["lyricsRevision"].toString()},
                            {"positionMs", timing["positionMs"].isNull()
                                               ? QVariant{}
                                               : QVariant(timing["positionMs"].toInteger() + advance)},
                            {"songPositionMs", song},
                            {"playbackStatus", playback["state"].toString()},
                            {"currentLine", current},
                            {"activeLines", activeLines},
                            {"activeWords", activeWords},
                            {"stale", elapsed >= 1000},
                            {"snapshot", m_snapshotVariant}};
    // No document/list reconstruction or lyricsChanged signal on clock ticks.
    if (m_state != state || m_currentLine != current || m_synchronized != (timed && !track.lines.isEmpty())) {
        m_state = state;
        m_currentLine = current;
        m_synchronized = timed && !track.lines.isEmpty();
        emit synchronizationChanged();
    }
    if (playing && song >= 0 && elapsed < 1000) {
        next = std::min(next, 1000 - elapsed);
        m_lineTimer.start(int(std::max(qint64(1), next)));
    }
}
