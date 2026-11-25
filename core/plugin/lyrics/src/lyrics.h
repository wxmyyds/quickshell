#pragma once

#include "observationprotocol.h"
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>
#include <memory>

class QDBusConnection;
class QDBusServiceWatcher;
class QLocalSocket;

// Read-only MPRIS selection; Cirrus observation v1 is the sole timed lyric source.
class Lyrics : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Lyrics)
    QML_SINGLETON

    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool hasLyrics READ hasLyrics NOTIFY lyricsChanged)
    Q_PROPERTY(QVariantList lyrics READ lyrics NOTIFY lyricsChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QVariantMap document READ document NOTIFY synchronizationChanged)
    Q_PROPERTY(QVariantMap state READ state NOTIFY synchronizationChanged)
    Q_PROPERTY(int currentLine READ currentLine NOTIFY synchronizationChanged)
    Q_PROPERTY(QString currentText READ currentText NOTIFY synchronizationChanged)
    Q_PROPERTY(bool synchronized READ isSynchronized NOTIFY synchronizationChanged)
    Q_PROPERTY(bool isSynchronized READ isSynchronized NOTIFY synchronizationChanged)

  public:
    explicit Lyrics(QObject *parent = nullptr, bool useSessionBus = true);
    ~Lyrics() override;
    QString status() const { return m_status; }
    bool hasLyrics() const { return !m_lyrics.isEmpty(); }
    QVariantList lyrics() const { return m_lyrics; }
    QString error() const { return m_error; }
    QVariantMap document() const { return m_document; }
    QVariantMap state() const { return m_state; }
    int currentLine() const { return m_currentLine; }
    QString currentText() const;
    bool isSynchronized() const { return m_synchronized; }
    Q_INVOKABLE void setMetadata(const QString &dbusName, const QVariantMap &metadata,
                                 const QString &playerIdentity = {});
    Q_INVOKABLE void clearTrack();

  signals:
    void statusChanged();
    void lyricsChanged();
    void errorChanged();
    void synchronizationChanged();

  private:
    friend class LyricsTest;
    struct Context {
        quint64 player = 0;
        quint64 socket = 0;
    };
    struct Endpoint {
        QString path;
        QString instance;
    };
    struct Request {
        QString method;
        QString binding;
        quint64 documentEpoch = 0;
        QString track;
        int from = 0;
        qint64 sent = 0;
    };
    struct Track {
        QJsonObject summary;
        QJsonArray lines;
        bool complete = false;
        qsizetype wordCount = 0;
    };
    Context context() const { return {m_playerEpoch, m_socketEpoch}; }
    bool isCurrent(Context context) const;
    bool isCirrus() const;
    void publish(const QString &status, QVariantList lines, const QString &error = {},
                 QVariantMap document = {}, QVariantMap state = {}, int currentLine = -1,
                 bool synchronized = false);
    void publishMetadata(const QVariantMap &metadata);
    void startOwnerWatch();
    void selectOwner(const QString &owner);
    void resolveOwnerPid();
    void stopPlayer();
    void resetSocket();
    void resetDocument();
    void discover();
    void watchDiscovery(const QStringList &paths);
    void retryDiscovery();
    void connectNext();
    void readSocket(Context context);
    void fail(bool invalid = false);
    void receive(Context context, const QJsonObject &message);
    void acceptSnapshot(const QJsonObject &snapshot);
    void acceptPage(const Request &request, const QJsonObject &page);
    void request(const QString &method, const QString &track = {}, int from = 0);
    void pump();
    void maintain();
    void assembleDocument();
    bool validateCursors();
    void updateCursor(qint64 elapsed = -1);
    QString preferredTrack(const QString &kind) const;
    QString binding(const QJsonObject &snapshot) const;

    bool m_useSessionBus;
    std::unique_ptr<QDBusConnection> m_bus;
    QDBusServiceWatcher *m_serviceWatcher = nullptr;
    QString m_service;
    QString m_playerIdentity;
    QString m_owner;
    qint64 m_ownerPid = 0;
    bool m_pidPending = false;
    quint64 m_playerEpoch = 0;
    quint64 m_socketEpoch = 0;
    quint64 m_documentEpoch = 0;
    QLocalSocket *m_socket = nullptr;
    QString m_endpoint;
    QString m_instance;
    QList<Endpoint> m_candidates;
    QFileSystemWatcher m_files;
    QTimer m_discoveryTimer;
    QTimer m_maintenanceTimer;
    QTimer m_lineTimer;
    QElapsedTimer m_monotonic;
    qint64 m_connectedAt = 0;
    int m_retryMs = 250;
    CirrusObservation::Framer m_framer;
    bool m_readQueued = false;
    bool m_hello = false;
    bool m_subscribed = false;
    QMap<QString, Request> m_pending;
    quint64 m_nextRequest = 0;
    double m_tokens = 16;
    qint64 m_tokenAt = 0;
    qint64 m_requestsAfter = 0;
    QJsonObject m_snapshot;
    bool m_haveSequence = false;
    quint64 m_sequence = 0;
    quint64 m_revision = 0;
    QString m_binding;
    QMap<QString, Track> m_tracks;
    qsizetype m_documentBytes = 0;
    qsizetype m_textUnits = 0;
    QVector<int> m_displayIndices;
    QVector<qint64> m_displayStarts;
    bool m_documentReady = false;
    QString m_displayTrack;
    QVariantMap m_snapshotVariant;
    qint64 m_receivedAt = 0;

    QString m_status = QStringLiteral("idle");
    QVariantList m_lyrics;
    QString m_error;
    QVariantMap m_document;
    QVariantMap m_state;
    int m_currentLine = -1;
    bool m_synchronized = false;
};
