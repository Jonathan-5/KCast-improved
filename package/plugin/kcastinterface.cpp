/*
 * SPDX-FileCopyrightText: 2025 Agundur <info@agundur.de>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 *
 */

#include "kcastinterface.h"
#include <QDBusConnection>
#include <QDBusError>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <memory>

using namespace Qt::StringLiterals;

QList<KCastBridge *> KCastBridge::s_instances;
KCastBridge *KCastBridge::s_pollOwner = nullptr;
QString KCastBridge::s_sharedDefaultDevice;
QStringList KCastBridge::s_sharedDevices;
QString KCastBridge::s_sharedMediaUrl;
QHash<QString, KCastBridge::SessionState> KCastBridge::s_sharedSessions;

void customMessageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    static QFile logFile(QDir::homePath() + QStringLiteral("/.local/share/kcast.log"));
    if (!logFile.isOpen() && !logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QTextStream out(&logFile);

    QString prefix;
    switch (type) {
    case QtDebugMsg:
        prefix = QStringLiteral("[DEBUG]");
        break;
    case QtWarningMsg:
        prefix = QStringLiteral("[WARN] ");
        break;
    case QtCriticalMsg:
        prefix = QStringLiteral("[CRIT] ");
        break;
    case QtFatalMsg:
        prefix = QStringLiteral("[FATAL]");
        break;
    case QtInfoMsg:
        prefix = QStringLiteral("[INFO] ");
        break;
    }

    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")) << " " << prefix << " " << msg << '\n';
    out.flush();
}

KCastBridge::KCastBridge(QObject *parent)
    : QObject(parent)
{
    s_instances.append(this);
    ensurePollOwner();

    // qInstallMessageHandler(customMessageHandler);

    // kurze Bündel-Zeit: UI darf „ausrauschen“, dann 1x senden
    m_coalesceTimer.setSingleShot(true);
    m_coalesceTimer.setInterval(90); // 80–120 ms fühlt sich snappy an
    connect(&m_coalesceTimer, &QTimer::timeout, this, &KCastBridge::flushVolumeDesired);

    // Mindestabstand zwischen zwei catt-Spawns (Python-Interpreter-Overhead!)
    m_rateLimitTimer.setSingleShot(true);
    m_rateLimitTimer.setInterval(100);

    m_statusPollTimer.setInterval(1000);
    connect(&m_statusPollTimer, &QTimer::timeout, this, &KCastBridge::refreshPlaybackStatus);

    m_statusPollBoostTimer.setSingleShot(true);
    m_statusPollBoostTimer.setInterval(1600);
    connect(&m_statusPollBoostTimer, &QTimer::timeout, this, [this]() {
        m_statusPollTimer.setInterval(1000);
        if (m_statusPollTimer.isActive())
            m_statusPollTimer.start();
    });

    adoptSharedState();

    if (s_pollOwner == this && hasSharedActiveSessions())
        startPlaybackStatusPolling(false);
}

KCastBridge::~KCastBridge()
{
    m_statusPollBoostTimer.stop();
    m_statusPollTimer.stop();
    s_instances.removeAll(this);

    if (s_pollOwner == this) {
        s_pollOwner = nullptr;
        ensurePollOwner();
        if (s_pollOwner && hasSharedActiveSessions())
            s_pollOwner->startPlaybackStatusPolling(false);
    }
}

bool KCastBridge::applyDefaultDeviceLocal(const QString &name, bool emitSignal)
{
    if (m_defaultDevice == name)
        return false;

    m_defaultDevice = name;
    if (emitSignal)
        Q_EMIT defaultDeviceChanged(m_defaultDevice);
    return true;
}

bool KCastBridge::applyDevicesLocal(const QStringList &devices, bool emitSignal)
{
    if (m_devices == devices)
        return false;

    m_devices = devices;
    if (emitSignal)
        Q_EMIT devicesChanged(m_devices);
    return true;
}

bool KCastBridge::applyMediaUrlLocal(const QString &url, bool emitSignal)
{
    if (m_mediaUrl == url)
        return false;

    m_mediaUrl = url;
    if (emitSignal)
        Q_EMIT mediaUrlChanged();
    return true;
}

bool KCastBridge::applyPlayingLocal(bool on, bool emitSignal)
{
    if (m_playing == on)
        return false;

    m_playing = on;
    if (emitSignal)
        Q_EMIT playingChanged();
    return true;
}

bool KCastBridge::applySessionActiveLocal(bool on, bool emitSignal)
{
    if (m_sessionActive == on)
        return false;

    m_sessionActive = on;
    if (emitSignal)
        Q_EMIT sessionActiveChanged();
    return true;
}

bool KCastBridge::applyMediaPositionLocal(int seconds, bool emitSignal)
{
    seconds = std::max(seconds, 0);
    if (m_mediaDuration > 0)
        seconds = std::min(seconds, m_mediaDuration);
    if (m_mediaPosition == seconds)
        return false;

    m_mediaPosition = seconds;
    if (emitSignal)
        Q_EMIT mediaPositionChanged();
    return true;
}

bool KCastBridge::applyMediaDurationLocal(int seconds, bool emitSignal)
{
    seconds = std::max(seconds, 0);
    if (m_mediaDuration == seconds)
        return false;

    m_mediaDuration = seconds;
    const bool positionChanged = m_mediaDuration > 0 && m_mediaPosition > m_mediaDuration;
    if (positionChanged)
        m_mediaPosition = m_mediaDuration;

    if (emitSignal) {
        Q_EMIT mediaDurationChanged();
        if (positionChanged)
            Q_EMIT mediaPositionChanged();
    }
    return true;
}

bool KCastBridge::applyMediaSeekableLocal(bool on, bool emitSignal)
{
    if (m_mediaSeekable == on)
        return false;

    m_mediaSeekable = on;
    if (emitSignal)
        Q_EMIT mediaSeekableChanged();
    return true;
}

void KCastBridge::adoptSharedState()
{
    applyDevicesLocal(s_sharedDevices, false);
    applyDefaultDeviceLocal(s_sharedDefaultDevice, false);
    applyMediaUrlLocal(s_sharedMediaUrl, false);
    syncLegacyPlaybackState();
}

void KCastBridge::syncLegacyPlaybackState()
{
    const SessionState *session = preferredSession();
    if (!session) {
        applyPlayingLocal(false, true);
        applySessionActiveLocal(false, true);
        applyMediaPositionLocal(0, true);
        applyMediaDurationLocal(0, true);
        applyMediaSeekableLocal(false, true);
        return;
    }

    applyPlayingLocal(session->playing, true);
    applySessionActiveLocal(session->sessionActive, true);
    applyMediaDurationLocal(session->mediaDuration, true);
    applyMediaPositionLocal(session->mediaPosition, true);
    applyMediaSeekableLocal(session->mediaSeekable, true);
}

const KCastBridge::SessionState *KCastBridge::preferredSession() const
{
    if (!m_defaultDevice.isEmpty()) {
        if (const SessionState *session = sharedSessionConst(m_defaultDevice))
            return session;
    }

    const QString fallbackDevice = firstSharedSessionDevice();
    if (!fallbackDevice.isEmpty())
        return sharedSessionConst(fallbackDevice);

    return nullptr;
}

QVariantMap KCastBridge::sessionToVariantMap(const SessionState &session)
{
    QVariantMap map;
    map.insert(u"device"_s, session.device);
    map.insert(u"mediaUrl"_s, session.mediaUrl);
    map.insert(u"playing"_s, session.playing);
    map.insert(u"sessionActive"_s, session.sessionActive);
    map.insert(u"mediaPosition"_s, session.mediaPosition);
    map.insert(u"mediaDuration"_s, session.mediaDuration);
    map.insert(u"mediaSeekable"_s, session.mediaSeekable);
    map.insert(u"volume"_s, session.volume);
    map.insert(u"muted"_s, session.muted);
    return map;
}

KCastBridge::SessionState *KCastBridge::sharedSession(const QString &device)
{
    auto it = s_sharedSessions.find(device);
    if (it == s_sharedSessions.end())
        return nullptr;
    return &it.value();
}

const KCastBridge::SessionState *KCastBridge::sharedSessionConst(const QString &device)
{
    auto it = s_sharedSessions.constFind(device);
    if (it == s_sharedSessions.cend())
        return nullptr;
    return &it.value();
}

QStringList KCastBridge::activeSessionDevices()
{
    QStringList devices;
    for (auto it = s_sharedSessions.cbegin(); it != s_sharedSessions.cend(); ++it) {
        if (it.value().sessionActive)
            devices.append(it.key());
    }
    return devices;
}

QString KCastBridge::firstSharedSessionDevice()
{
    if (s_sharedSessions.isEmpty())
        return {};

    QStringList keys = s_sharedSessions.keys();
    std::sort(keys.begin(), keys.end());
    return keys.constFirst();
}

bool KCastBridge::hasSharedActiveSessions()
{
    for (auto it = s_sharedSessions.cbegin(); it != s_sharedSessions.cend(); ++it) {
        if (it.value().sessionActive)
            return true;
    }
    return false;
}

void KCastBridge::removeSharedSession(const QString &device)
{
    s_sharedSessions.remove(device);
}

void KCastBridge::broadcastSessionsChanged()
{
    for (KCastBridge *instance : s_instances) {
        if (!instance)
            continue;
        instance->syncLegacyPlaybackState();
        Q_EMIT instance->sessionsChanged();
    }
}

void KCastBridge::broadcastDefaultDeviceChanged()
{
    for (KCastBridge *instance : s_instances) {
        if (!instance)
            continue;
        instance->applyDefaultDeviceLocal(s_sharedDefaultDevice, true);
        instance->syncLegacyPlaybackState();
    }
}

void KCastBridge::broadcastDevicesChanged()
{
    for (KCastBridge *instance : s_instances) {
        if (instance)
            instance->applyDevicesLocal(s_sharedDevices, true);
    }
}

void KCastBridge::ensurePollOwner()
{
    if (s_pollOwner && s_instances.contains(s_pollOwner))
        return;

    s_pollOwner = s_instances.isEmpty() ? nullptr : s_instances.constFirst();
}

QString KCastBridge::cattExecutable() const
{
    const QString resolved = QStandardPaths::findExecutable(u"catt"_s);
    if (!resolved.isEmpty())
        return resolved;

    return u"catt"_s;
}

void KCastBridge::playMedia(const QString &device, const QString &url)
{
    const bool ok = QProcess::startDetached(cattExecutable(), QStringList() << QString::fromUtf8("-d") << device << QString::fromUtf8("cast") << url);
    if (!ok) {
        qWarning() << QString::fromUtf8("Failed to start catt cast");
        return;
    }

    SessionState &session = s_sharedSessions[device];
    session.device = device;
    session.mediaUrl = url;
    session.playing = true;
    session.sessionActive = true;
    session.mediaPosition = 0;
    session.mediaDuration = 0;
    session.mediaSeekable = false;
    session.statusInFlight = false;
    ++session.statusGeneration;

    setMediaUrl(url);
    broadcastSessionsChanged();
    boostPlaybackStatusPolling();
    startPlaybackStatusPolling(false);
}

void KCastBridge::pauseMedia(const QString &device)
{
    const bool ok = QProcess::startDetached(cattExecutable(), QStringList{u"-d"_s, device, u"pause"_s});
    if (!ok) {
        qWarning() << u"[KCast] Failed to start catt pause"_s;
        return;
    }

    SessionState &session = s_sharedSessions[device];
    session.device = device;
    session.playing = false;
    session.sessionActive = true;
    session.statusInFlight = false;
    ++session.statusGeneration;
    broadcastSessionsChanged();
    startPlaybackStatusPolling(false);
}

void KCastBridge::resumeMedia(const QString &device)
{
    using namespace Qt::StringLiterals;

    const bool ok = QProcess::startDetached(cattExecutable(), QStringList{u"-d"_s, device, u"play"_s});

    if (!ok) {
        qWarning() << u"[KCast] Failed to start catt play (resume)"_s;
        return;
    }

    SessionState &session = s_sharedSessions[device];
    session.device = device;
    session.playing = true;
    session.sessionActive = true;
    session.statusInFlight = false;
    ++session.statusGeneration;
    broadcastSessionsChanged();
    startPlaybackStatusPolling(false);
}

void KCastBridge::stopMedia(const QString &device)
{
    const bool ok = QProcess::startDetached(cattExecutable(), QStringList() << QString::fromUtf8("-d") << device << QString::fromUtf8("stop"));
    if (!ok) {
        qWarning() << QString::fromUtf8("Failed to start catt stop");
        return;
    }

    stopPlaybackStatusPolling(device, true);
}

bool KCastBridge::seekTo(int seconds)
{
    const QString device = pickDefaultDevice();
    return seekOnDevice(device, seconds);
}

bool KCastBridge::seekOnDevice(const QString &device, int seconds)
{
    if (device.isEmpty()) {
        qWarning() << u"[KCast] seekTo: default device not set."_s;
        return false;
    }

    seconds = std::max(seconds, 0);
    const bool ok = QProcess::startDetached(cattExecutable(), QStringList{u"-d"_s, device, u"seek"_s, QString::number(seconds)});
    if (!ok) {
        qWarning() << u"[KCast] Failed to start catt seek"_s;
        return false;
    }

    SessionState &session = s_sharedSessions[device];
    session.device = device;
    session.sessionActive = true;
    session.mediaPosition = seconds;
    if (session.mediaDuration > 0)
        session.mediaPosition = std::min(session.mediaPosition, session.mediaDuration);
    session.statusInFlight = false;
    ++session.statusGeneration;
    broadcastSessionsChanged();
    boostPlaybackStatusPolling();
    startPlaybackStatusPolling(false);
    QTimer::singleShot(160, this, &KCastBridge::refreshPlaybackStatus);
    QTimer::singleShot(420, this, &KCastBridge::refreshPlaybackStatus);
    return true;
}

bool KCastBridge::isCattInstalled() const
{
    // QStandardPaths::findExecutable sucht in den PATH-Umgebungsvariablen
    // und gibt den absoluten Pfad zurück, oder einen leeren QString, wenn es nicht gefunden wurde.
    QString exePath = QStandardPaths::findExecutable(QLatin1String("catt"));
    if (exePath.isEmpty()) {
        qWarning() << QStringLiteral("catt executable not found)");
        return false;
    } else {
        qDebug() << QStringLiteral("catt found:") << exePath;
        return true;
    }
}

void KCastBridge::scanDevicesAsync()
{
    auto *p = new QProcess(this);
    p->setProgram(cattExecutable());
    p->setArguments({QStringLiteral("scan")});
    p->setProcessChannelMode(QProcess::MergedChannels);

    auto *buf = new QString; // Puffer für ggf. unvollständige Zeilen
    auto *acc = new QStringList; // gesammelte Gerätenamen

    // Sicherheitsnetz: Scan nach 30s abbrechen (keine UI-Blockade)
    auto *kill = new QTimer(this);
    kill->setSingleShot(true);
    kill->setInterval(30000);
    connect(kill, &QTimer::timeout, this, [p, kill]() {
        if (p->state() != QProcess::NotRunning)
            p->kill();
        kill->deleteLater();
    });
    kill->start();

    // Inkrementell lesen und Zeilen verarbeiten
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p, acc, buf]() {
        *buf += QString::fromUtf8(p->readAllStandardOutput());

        qsizetype nl;
        while ((nl = buf->indexOf(QLatin1Char('\n'))) >= 0) {
            const QString line = buf->left(nl);
            buf->remove(0, nl + 1);

            const auto parts = line.split(QStringLiteral(" - "));
            if (parts.size() >= 2) {
                const QString name = parts.at(1).trimmed();
                if (!name.isEmpty() && !acc->contains(name)) {
                    acc->append(name);
                    Q_EMIT deviceFound(name); // Live-Update fürs UI

                    // Properties live synchron halten
                    if (!s_sharedDevices.contains(name)) {
                        s_sharedDevices.append(name);
                        broadcastDevicesChanged();
                    }
                    if (s_sharedDefaultDevice.isEmpty()) {
                        s_sharedDefaultDevice = name;
                        broadcastDefaultDeviceChanged();
                    }
                }
            }
        }
    });

    // Abschluss: Restzeile (ohne \n) verarbeiten + final emitten
    connect(p, &QProcess::finished, this, [this, p, acc, buf, kill](int, QProcess::ExitStatus) {
        if (!buf->isEmpty()) {
            const QString line = *buf;
            const auto parts = line.split(QStringLiteral(" - "));
            if (parts.size() >= 2) {
                const QString name = parts.at(1).trimmed();
                if (!name.isEmpty() && !acc->contains(name)) {
                    acc->append(name);
                    Q_EMIT deviceFound(name);
                    if (!s_sharedDevices.contains(name)) {
                        s_sharedDevices.append(name);
                        broadcastDevicesChanged();
                    }
                    if (s_sharedDefaultDevice.isEmpty()) {
                        s_sharedDefaultDevice = name;
                        broadcastDefaultDeviceChanged();
                    }
                }
            }
        }

        if (s_sharedDevices != *acc) {
            s_sharedDevices = *acc;
            broadcastDevicesChanged();
        }
        Q_EMIT devicesScanned(*acc);

        delete buf;
        delete acc;
        p->deleteLater();
        if (kill)
            kill->deleteLater();
    });

    p->start();
}

static QString toLocalMediaPath(const QString &in)
{
    const QUrl u(in);
    if (u.isLocalFile() || in.startsWith(u"file://"_s))
        return u.toLocalFile();
    return in;
}

static QString decodePercentEncodedLocalPath(const QString &in)
{
    if (!in.contains(u'%'))
        return in;

    const QString decoded = QUrl::fromPercentEncoding(in.toUtf8());
    if (decoded == in)
        return in;

    const QFileInfo originalInfo(in);
    const QFileInfo decodedInfo(decoded);
    if (!originalInfo.exists() && decodedInfo.exists())
        return decodedInfo.absoluteFilePath();

    return in;
}

void KCastBridge::probeReceiver(const QString &assetUrl)
{
    const QString dev = pickDefaultDevice();
    if (dev.isEmpty())
        return;

    // 1) Prefer: expliziten Pfad/URL aus QML verwenden (funktioniert im Dev-Tree)
    QString asset = assetUrl;

    // 2) Fallback: im installierten System suchen
    if (asset.isEmpty()) {
        asset = QStandardPaths::locate(QStandardPaths::GenericDataLocation, u"plasma/plasmoids/de.agundur.kcast/contents/ui/250-milliseconds-of-silence.mp3"_s);
        if (asset.isEmpty())
            return; // kein Outbound, einfach aufgeben
    }

    const QString local = toLocalMediaPath(asset);

    QProcess::startDetached(cattExecutable(), {u"-d"_s, dev, u"stop"_s});
    QProcess::startDetached(cattExecutable(), {u"-d"_s, dev, u"quit"_s});

    QTimer::singleShot(350, this, [dev, local]() {
        auto *p = new QProcess();
        p->setProgram(QStandardPaths::findExecutable(u"catt"_s));
        p->setArguments({u"-d"_s, dev, u"cast"_s, local});
        p->setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(p, &QProcess::finished, p, [dev, p] {
            const QString out = QString::fromUtf8(p->readAll());
            p->deleteLater();
            QProcess::startDetached(QStandardPaths::findExecutable(u"catt"_s), {u"-d"_s, dev, u"quit"_s});
            // Optional: out auf CC1AD845 prüfen und ein Signal emittieren
        });
        p->start();
    });
}

// ---- DBUS Helper ----

bool KCastBridge::registerDBus()
{
    auto bus = QDBusConnection::sessionBus();

    const bool okObj = bus.registerObject(u"/de/agundur/kcast"_s, this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
    if (!okObj) {
        qWarning() << "[KCast] DBus: registerObject failed:" << bus.lastError().message();
        setDbusReady(false);
        // Retry – manchmal ist der Bus/Objektbaum noch nicht so weit
        scheduleDbusRetry();
        return false;
    }

    if (!bus.registerService(u"de.agundur.kcast"_s)) {
        // Kann beim Plasma-Start vorkommen (Race mit zweiter Instanz / Bus init)
        qWarning() << "[KCast] DBus: registerService failed:" << bus.lastError().message();
        setDbusReady(false);
        scheduleDbusRetry();
        return false;
    }

    qInfo() << "[KCast] DBus ready on de.agundur.kcast /de/agundur/kcast";
    setDbusReady(true);
    return true;
}

void KCastBridge::setMediaUrl(const QString &url)
{
    s_sharedMediaUrl = url;
    for (KCastBridge *instance : s_instances) {
        if (instance)
            instance->applyMediaUrlLocal(url, true);
    }
}

QVariantList KCastBridge::sessions() const
{
    QVariantList list;
    QStringList keys = s_sharedSessions.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &key : keys)
        list.push_back(sessionToVariantMap(s_sharedSessions.value(key)));
    return list;
}

QVariantMap KCastBridge::sessionForDevice(const QString &device) const
{
    if (const SessionState *session = sharedSessionConst(device))
        return sessionToVariantMap(*session);
    return {};
}

bool KCastBridge::hasSessionForDevice(const QString &device) const
{
    return sharedSessionConst(device) != nullptr;
}

QString KCastBridge::firstActiveSessionDevice() const
{
    for (const QString &device : activeSessionDevices())
        return device;
    return firstSharedSessionDevice();
}

QString KCastBridge::pickDefaultDevice() const
{
    return m_defaultDevice;
}

QString KCastBridge::normalizeMediaInput(const QString &input) const
{
    return normalizeUrlForCasting(input);
}

QString KCastBridge::normalizeUrlForCasting(const QString &in) const
{
    const QString trimmed = in.trimmed();
    if (trimmed.isEmpty())
        return {};

    const bool hasScheme = trimmed.contains(u"://"_s);
    const QString candidate = hasScheme ? trimmed : decodePercentEncodedLocalPath(trimmed);
    QUrl u = QUrl::fromUserInput(candidate);
    if (u.isLocalFile()) {
        return u.toLocalFile(); // kein file:// → vermeidet yt-dlp-Block
    }
    if (u.isRelative() && QFileInfo(candidate).exists()) {
        return QFileInfo(candidate).absoluteFilePath();
    }
    return u.toString(); // http/https o.ä.
}

std::optional<int> KCastBridge::parseClockTime(const QString &value)
{
    const QStringList parts = value.split(u':');
    if (parts.size() != 3)
        return std::nullopt;

    bool okHours = false;
    bool okMinutes = false;
    bool okSeconds = false;
    const int hours = parts.at(0).toInt(&okHours);
    const int minutes = parts.at(1).toInt(&okMinutes);
    const int seconds = parts.at(2).toInt(&okSeconds);
    if (!okHours || !okMinutes || !okSeconds)
        return std::nullopt;

    return (hours * 3600) + (minutes * 60) + seconds;
}

void KCastBridge::resetPlaybackMetrics()
{
    setMediaPosition(0);
    setMediaDuration(0);
    setMediaSeekable(false);
}

void KCastBridge::boostPlaybackStatusPolling()
{
    ensurePollOwner();
    if (s_pollOwner && s_pollOwner != this) {
        s_pollOwner->boostPlaybackStatusPolling();
        return;
    }

    m_statusPollTimer.setInterval(250);
    if (m_statusPollTimer.isActive())
        m_statusPollTimer.start();
    m_statusPollBoostTimer.start();
}

void KCastBridge::startPlaybackStatusPolling(bool immediate)
{
    ensurePollOwner();
    if (s_pollOwner && s_pollOwner != this) {
        s_pollOwner->startPlaybackStatusPolling(immediate);
        return;
    }

    if (!hasSharedActiveSessions())
        return;

    if (!m_statusPollTimer.isActive())
        m_statusPollTimer.start();

    if (immediate)
        refreshPlaybackStatus();
}

void KCastBridge::stopPlaybackStatusPolling(const QString &device, bool removeSession)
{
    if (SessionState *session = sharedSession(device)) {
        session->statusInFlight = false;
        ++session->statusGeneration;

        if (removeSession) {
            removeSharedSession(device);
        } else {
            session->playing = false;
            session->sessionActive = false;
            session->mediaPosition = 0;
            session->mediaDuration = 0;
            session->mediaSeekable = false;
        }
    }

    if (s_pollOwner) {
        if (!hasSharedActiveSessions()) {
            s_pollOwner->m_statusPollTimer.stop();
            s_pollOwner->m_statusPollBoostTimer.stop();
            s_pollOwner->m_statusPollTimer.setInterval(1000);
        } else if (s_pollOwner != this) {
            s_pollOwner->startPlaybackStatusPolling(false);
        }
    }

    broadcastSessionsChanged();
}

void KCastBridge::refreshPlaybackStatus()
{
    ensurePollOwner();
    if (s_pollOwner && s_pollOwner != this) {
        s_pollOwner->refreshPlaybackStatus();
        return;
    }

    for (const QString &device : activeSessionDevices()) {
        SessionState *session = sharedSession(device);
        if (!session || session->statusInFlight)
            continue;

        auto *p = new QProcess(this);
        const auto handled = std::make_shared<bool>(false);
        const quint64 generation = session->statusGeneration;

        session->statusInFlight = true;
        p->setProgram(cattExecutable());
        p->setArguments({u"-d"_s, device, u"status"_s});
        p->setProcessChannelMode(QProcess::MergedChannels);

        const auto finalize = [this, p, handled, device, generation](const QString &output, bool shouldParse) {
            if (*handled)
                return;

            *handled = true;

            if (SessionState *session = sharedSession(device)) {
                if (session->statusGeneration == generation) {
                    session->statusInFlight = false;
                    if (shouldParse)
                        applyPlaybackStatus(device, output);
                }
            }

            p->deleteLater();
        };

        connect(p, &QProcess::finished, this, [finalize, p](int, QProcess::ExitStatus) {
            finalize(QString::fromUtf8(p->readAll()), true);
        });
        connect(p, &QProcess::errorOccurred, this, [finalize, p](QProcess::ProcessError) {
            finalize(QString::fromUtf8(p->readAll()), false);
        });
        p->start();
    }
}

void KCastBridge::applyPlaybackStatus(const QString &device, const QString &output)
{
    const QString trimmedOutput = output.trimmed();
    if (trimmedOutput.isEmpty())
        return;

    static const QRegularExpression timePattern(QStringLiteral(R"(^Time:\s*([0-9]{2}:[0-9]{2}:[0-9]{2})(?:\s*/\s*([0-9]{2}:[0-9]{2}:[0-9]{2})\s*\(([\d.]+)%\))?\s*$)"));
    static const QRegularExpression statePattern(QStringLiteral(R"(^State:\s*(\S+)\s*$)"));

    std::optional<int> current;
    std::optional<int> duration;
    QString state;

    const QStringList lines = trimmedOutput.split(u'\n', Qt::SkipEmptyParts);
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();

        const QRegularExpressionMatch timeMatch = timePattern.match(line);
        if (timeMatch.hasMatch()) {
            current = parseClockTime(timeMatch.captured(1));
            if (!timeMatch.captured(2).isEmpty())
                duration = parseClockTime(timeMatch.captured(2));
            continue;
        }

        const QRegularExpressionMatch stateMatch = statePattern.match(line);
        if (stateMatch.hasMatch()) {
            state = stateMatch.captured(1).trimmed().toUpper();
            continue;
        }
    }

    SessionState *session = sharedSession(device);
    if (!session)
        return;

    if (current.has_value())
        session->mediaPosition = *current;

    if (duration.has_value()) {
        session->mediaDuration = *duration;
        session->mediaSeekable = *duration > 0;
    } else if (session->mediaDuration <= 0) {
        // Some catt status responses omit total duration mid-playback.
        // Keep the last known duration so the seek slider does not collapse to zero.
        session->mediaSeekable = false;
    }

    if (state == u"PLAYING"_s || state == u"BUFFERING"_s) {
        session->sessionActive = true;
        session->playing = true;
        broadcastSessionsChanged();
        return;
    }

    if (state == u"PAUSED"_s) {
        session->sessionActive = true;
        session->playing = false;
        broadcastSessionsChanged();
        return;
    }

    if (state == u"IDLE"_s || state == u"UNKNOWN"_s) {
        stopPlaybackStatusPolling(device, true);
        return;
    }

    if (current.has_value())
        session->sessionActive = true;

    broadcastSessionsChanged();
}

void KCastBridge::setSessionActive(bool on)
{
    applySessionActiveLocal(on, true);
}

void KCastBridge::setMediaPosition(int seconds)
{
    applyMediaPositionLocal(seconds, true);
}

void KCastBridge::setMediaDuration(int seconds)
{
    applyMediaDurationLocal(seconds, true);
}

void KCastBridge::setMediaSeekable(bool on)
{
    applyMediaSeekableLocal(on, true);
}

void KCastBridge::setPlaying(bool on)
{
    applyPlayingLocal(on, true);
}

// ---- QML-Setter ----
void KCastBridge::setDefaultDevice(const QString &name)
{
    if (s_sharedDefaultDevice == name)
        return;

    s_sharedDefaultDevice = name;
    if (!s_sharedDevices.contains(name)) {
        s_sharedDevices.append(name);
        broadcastDevicesChanged();
    }
    broadcastDefaultDeviceChanged();
}

// ---- D-Bus Slots ----
void KCastBridge::CastFile(const QString &url)
{
    const QString device = pickDefaultDevice();
    if (device.isEmpty()) {
        qWarning() << u"[KCast] No device available for CastFile"_s;
        return;
    }
    const QString norm = normalizeUrlForCasting(url);

    // GUI synchronisieren:
    setMediaUrl(norm);
    setPlaying(true);

    qInfo() << u"[KCast] CastFile →"_s << device << norm;
    playMedia(device, norm);
}

void KCastBridge::CastFiles(const QStringList &urls)
{
    const QString device = pickDefaultDevice();
    if (device.isEmpty()) {
        qWarning() << u"[KCast] No device available for CastFiles"_s;
        return;
    }
    bool first = true;
    for (const QString &u : urls) {
        const QString norm = normalizeUrlForCasting(u);
        if (first) {
            setMediaUrl(norm);
            setPlaying(true);
            first = false;
        } // ← AN
        playMedia(device, norm);
    }
}

void KCastBridge::scheduleDbusRetry()
{
    static int tries = 0;
    if (tries >= 5)
        return;
    ++tries;

    QTimer::singleShot(1000, this, [this]() {
        qInfo() << "[KCast] DBus retry…";
        registerDBus();
    });
}

// ---- Volume ----

bool KCastBridge::setVolume(int level)
{
    return setVolumeForDevice(pickDefaultDevice(), level);
}

bool KCastBridge::volumeUp(int delta)
{
    if (delta <= 0)
        delta = 5;
    const QString device = pickDefaultDevice();
    const SessionState *session = sharedSessionConst(device);
    const int base = session ? session->volume : 50;
    return setVolumeForDevice(device, base + delta);
}

bool KCastBridge::volumeDown(int delta)
{
    if (delta <= 0)
        delta = 5;
    const QString device = pickDefaultDevice();
    const SessionState *session = sharedSessionConst(device);
    const int base = session ? session->volume : 50;
    return setVolumeForDevice(device, base - delta);
}

bool KCastBridge::setMuted(bool on)
{
    return setMutedForDevice(pickDefaultDevice(), on);
}

bool KCastBridge::setVolumeForDevice(const QString &device, int level)
{
    if (device.isEmpty()) {
        qWarning() << u"[KCast] setVolumeForDevice: default device not set."_s;
        return false;
    }

    level = clampVolume(level);
    const QStringList args{u"-d"_s, device, u"volume"_s, QString::number(level)};
    const bool ok = QProcess::startDetached(cattExecutable(), args);
    if (!ok) {
        qWarning() << u"[KCast] Failed to start catt volume."_s;
        return false;
    }

    if (SessionState *session = sharedSession(device)) {
        session->volume = level;
        broadcastSessionsChanged();
    }
    Q_EMIT volumeCommandSent(u"set"_s, level);
    return true;
}

bool KCastBridge::setMutedForDevice(const QString &device, bool on)
{
    if (device.isEmpty()) {
        qWarning() << u"[KCast] setMutedForDevice: no Chromecast device available."_s;
        return false;
    }

    const QStringList args{u"-d"_s, device, u"volumemute"_s, on ? u"true"_s : u"false"_s};
    const bool ok = QProcess::startDetached(cattExecutable(), args);
    if (!ok) {
        qWarning() << u"[KCast] Failed to start catt volumemute."_s;
        return false;
    }

    if (SessionState *session = sharedSession(device)) {
        session->muted = on;
        broadcastSessionsChanged();
    }
    Q_EMIT muteCommandSent(on);
    return true;
}

// ---- Coalescer ----

void KCastBridge::requestVolumeAbsolute(int level)
{
    m_desiredVolume = clampVolume(level);
    // jeder neue Wunsch startet die Bündelung neu (last-wins)
    m_coalesceTimer.start();
}

void KCastBridge::flushVolumeDesired()
{
    if (!m_desiredVolume.has_value())
        return;

    // Rate-Limit noch aktiv? Danach erneut versuchen.
    if (m_rateLimitTimer.isActive()) {
        m_coalesceTimer.start(m_rateLimitTimer.remainingTime() + 10);
        return;
    }

    const int target = clampVolume(*m_desiredVolume);

    if (m_lastSentVolume == target) {
        m_desiredVolume.reset();
        return; // schon dort
    }

    const bool ok = spawnCattSetVolume(target);
    if (!ok) {
        qWarning() << u"[KCast] Failed to start catt volume."_s;
        // nicht aufgeben – in 200 ms nochmal probieren
        m_coalesceTimer.start(200);
        return;
    }

    m_lastSentVolume = target;
    m_desiredVolume.reset();
    m_rateLimitTimer.start();

    // falls während des Spawns neue Wünsche kamen, direkt wieder bündeln
    if (m_desiredVolume.has_value())
        m_coalesceTimer.start();
}

// ---- Helpers: tatsächlich catt starten (absolut!) ----

bool KCastBridge::spawnCattSetVolume(int level)
{
    const QString device = pickDefaultDevice();
    if (device.isEmpty()) {
        qWarning() << u"[KCast] setVolume: default device not set."_s;
        return false; // NICHT scannen!
    }
    const QStringList args{u"-d"_s, device, u"volume"_s, QString::number(level)};
    return QProcess::startDetached(cattExecutable(), args);
}

bool KCastBridge::spawnCattMute(bool on)
{
    const QString device = pickDefaultDevice();
    if (device.isEmpty()) {
        qWarning() << u"[KCast] setMuted: no Chromecast device available."_s;
        return false;
    }
    // catt --help: volumemute  Enable or disable mute on supported devices.
    const QStringList args{u"-d"_s, device, u"volumemute"_s, on ? u"true"_s : u"false"_s};
    return QProcess::startDetached(cattExecutable(), args);
}
