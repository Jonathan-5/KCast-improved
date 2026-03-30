#ifndef KCASTINTERFACE_H
#define KCASTINTERFACE_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <algorithm>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

class KCastBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_CLASSINFO("D-Bus Interface", "de.agundur.kcast")

    Q_PROPERTY(QString mediaUrl READ mediaUrl WRITE setMediaUrl NOTIFY mediaUrlChanged FINAL)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged FINAL)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionActiveChanged FINAL)
    Q_PROPERTY(int mediaPosition READ mediaPosition NOTIFY mediaPositionChanged FINAL)
    Q_PROPERTY(int mediaDuration READ mediaDuration NOTIFY mediaDurationChanged FINAL)
    Q_PROPERTY(bool mediaSeekable READ mediaSeekable NOTIFY mediaSeekableChanged FINAL)
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged FINAL)

    Q_PROPERTY(QString defaultDevice READ defaultDevice NOTIFY defaultDeviceChanged)
    Q_PROPERTY(QStringList devices READ devices NOTIFY devicesChanged)

    QString defaultDevice() const
    {
        return m_defaultDevice;
    }

    QStringList devices() const
    {
        return m_devices;
    }

public:
    explicit KCastBridge(QObject *parent = nullptr);
    ~KCastBridge() override;

    Q_INVOKABLE void scanDevicesAsync();
    Q_INVOKABLE QString normalizeMediaInput(const QString &input) const;
    Q_INVOKABLE bool seekTo(int seconds);
    Q_INVOKABLE bool seekOnDevice(const QString &device, int seconds);
    Q_INVOKABLE void playMedia(const QString &device, const QString &url);
    Q_INVOKABLE void pauseMedia(const QString &device);
    Q_INVOKABLE void resumeMedia(const QString &device);
    Q_INVOKABLE void stopMedia(const QString &device);
    Q_INVOKABLE bool isCattInstalled() const;
    Q_INVOKABLE void setDefaultDevice(const QString &name);
    Q_INVOKABLE bool registerDBus();
    Q_INVOKABLE void probeReceiver(const QString &assetUrl = QString());
    Q_INVOKABLE bool setVolume(int level); // 0..100
    Q_INVOKABLE bool volumeUp(int delta = 5);
    Q_INVOKABLE bool volumeDown(int delta = 5);
    Q_INVOKABLE bool setMuted(bool on);
    Q_INVOKABLE bool setVolumeForDevice(const QString &device, int level);
    Q_INVOKABLE bool setMutedForDevice(const QString &device, bool on);
    Q_INVOKABLE QVariantMap sessionForDevice(const QString &device) const;
    Q_INVOKABLE bool hasSessionForDevice(const QString &device) const;
    Q_INVOKABLE QString firstActiveSessionDevice() const;

    bool dbusReady() const
    {
        return m_dbusReady;
    }
    bool sessionActive() const
    {
        return m_sessionActive;
    }
    int mediaPosition() const
    {
        return m_mediaPosition;
    }
    int mediaDuration() const
    {
        return m_mediaDuration;
    }
    bool mediaSeekable() const
    {
        return m_mediaSeekable;
    }
    QVariantList sessions() const;
    // Property
    QString mediaUrl() const
    {
        return m_mediaUrl;
    }
    void setMediaUrl(const QString &url);
    bool playing() const
    {
        return m_playing;
    }

public Q_SLOTS: // —> per D-Bus aufrufbar
    void CastFile(const QString &url);
    void CastFiles(const QStringList &urls);

Q_SIGNALS:
    void deviceFound(QString name); // <— neu
    void devicesScanned(QStringList names); // falls noch nicht vorhanden
    void devicesChanged(QStringList); // existiert?
    void defaultDeviceChanged(QString);

    void mediaUrlChanged();
    void playingChanged();
    void sessionActiveChanged();
    void mediaPositionChanged();
    void mediaDurationChanged();
    void mediaSeekableChanged();
    void sessionsChanged();
    void dbusReadyChanged();
    void volumeCommandSent(QString command, int value);
    void muteCommandSent(bool muted);

private:
    struct SessionState {
        QString device;
        QString mediaUrl;
        bool playing = false;
        bool sessionActive = false;
        int mediaPosition = 0;
        int mediaDuration = 0;
        bool mediaSeekable = false;
        int volume = 50;
        bool muted = false;
        bool statusInFlight = false;
        quint64 statusGeneration = 0;
    };

    bool applyDefaultDeviceLocal(const QString &name, bool emitSignal);
    bool applyDevicesLocal(const QStringList &devices, bool emitSignal);
    bool applyMediaUrlLocal(const QString &url, bool emitSignal);
    bool applyPlayingLocal(bool on, bool emitSignal);
    bool applySessionActiveLocal(bool on, bool emitSignal);
    bool applyMediaPositionLocal(int seconds, bool emitSignal);
    bool applyMediaDurationLocal(int seconds, bool emitSignal);
    bool applyMediaSeekableLocal(bool on, bool emitSignal);
    void adoptSharedState();
    void syncLegacyPlaybackState();
    const SessionState *preferredSession() const;
    static QVariantMap sessionToVariantMap(const SessionState &session);
    static SessionState *sharedSession(const QString &device);
    static const SessionState *sharedSessionConst(const QString &device);
    static QStringList activeSessionDevices();
    static QString firstSharedSessionDevice();
    static bool hasSharedActiveSessions();
    static void removeSharedSession(const QString &device);
    static void broadcastSessionsChanged();
    static void broadcastDefaultDeviceChanged();
    static void broadcastDevicesChanged();
    static void ensurePollOwner();
    QString m_defaultDevice;
    QStringList m_devices;

    QString m_mediaUrl;
    QString cattExecutable() const;
    QString pickDefaultDevice() const;
    QString normalizeUrlForCasting(const QString &in) const;
    void startPlaybackStatusPolling(bool immediate = true);
    void stopPlaybackStatusPolling(const QString &device, bool removeSession);
    void refreshPlaybackStatus();
    void applyPlaybackStatus(const QString &device, const QString &output);
    static std::optional<int> parseClockTime(const QString &value);
    void resetPlaybackMetrics();
    void boostPlaybackStatusPolling();
    void setSessionActive(bool on);
    void setMediaPosition(int seconds);
    void setMediaDuration(int seconds);
    void setMediaSeekable(bool on);
    bool m_playing = false; // ← NEU
    void setPlaying(bool on); // ← NEU

    bool m_dbusReady = false;
    bool m_sessionActive = false;
    int m_mediaPosition = 0;
    int m_mediaDuration = 0;
    bool m_mediaSeekable = false;
    bool m_statusPollInFlight = false;
    quint64 m_statusPollGeneration = 0;
    void setDbusReady(bool on)
    {
        if (m_dbusReady == on)
            return;
        m_dbusReady = on;
        Q_EMIT dbusReadyChanged();
    }

    void scheduleDbusRetry();

    // ---- Coalescer ----
    void requestVolumeAbsolute(int level);
    void flushVolumeDesired();
    static int clampVolume(int v)
    {
        return std::clamp(v, 0, 100);
    }

    bool spawnCattSetVolume(int level);
    bool spawnCattMute(bool on);

    // State
    std::optional<int> m_desiredVolume; // letzter gewünschter Zielwert (last-wins)
    int m_lastSentVolume = -1; // unbekannt am Start
    QTimer m_coalesceTimer; // bündelt schnelle Änderungen
    QTimer m_rateLimitTimer; // Mindestabstand zwischen Spawns
    QTimer m_statusPollTimer; // leichter Poll für Seek-Status
    QTimer m_statusPollBoostTimer; // kurz schneller nach Seek/Start

    static QList<KCastBridge *> s_instances;
    static KCastBridge *s_pollOwner;
    static QString s_sharedDefaultDevice;
    static QStringList s_sharedDevices;
    static QString s_sharedMediaUrl;
    static QHash<QString, SessionState> s_sharedSessions;
};

#endif // KCASTINTERFACE_H
