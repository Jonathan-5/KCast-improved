/*
 * SPDX-FileCopyrightText: 2025 Agundur <info@agundur.de>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 *
 */

import Qt.labs.platform as Platform
import QtQuick 6.5
import QtQuick.Controls 6.7
import QtQuick.Layouts
import de.agundur.kcast 1.0
import org.kde.kirigami 2.20 as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid

Item {
    id: fullRoot

    property var devices: []
    property bool isScanning: false
    property string activeSessionDevice: ""
    property int volumeStepBig: 5
    property int volumeStepSmall: 1
    property int seekJumpSeconds: 10
    property int currentVolume: 50
    property int seekPreviewPosition: 0
    property int pendingSeekPosition: -1
    property string pendingSeekDevice: ""
    property string pendingSessionDevice: ""
    property string pendingVolumeDevice: ""
    property int pendingVolumeValue: -1
    property int seekSettleTolerance: 2
    property bool muted: false
    readonly property int pageMargin: Kirigami.Units.largeSpacing
    readonly property int sectionSpacing: Kirigami.Units.mediumSpacing
    readonly property int controlSpacing: Kirigami.Units.smallSpacing
    readonly property string selectedDevice: typeof deviceSelector.currentText === "string" ? deviceSelector.currentText : ""
    readonly property string targetDevice: currentSessionDevice.length > 0 ? currentSessionDevice : selectedDevice
    readonly property bool deviceReady: selectedDevice.length > 0
    readonly property bool controlsEnabled: deviceReady
    readonly property bool hasMedia: typeof mediaUrl.text === "string" && mediaUrl.text.trim().length > 0
    readonly property var sessionList: (kcast && kcast.sessions !== undefined && kcast.sessions !== null) ? kcast.sessions : []
    readonly property int sessionCount: (sessionList && sessionList.length !== undefined) ? sessionList.length : 0
    readonly property bool hasSession: sessionCount > 0
    readonly property int activeSessionIndex: sessionIndexForDevice(activeSessionDevice)
    readonly property var currentSession: hasSession ? (activeSessionIndex >= 0 && activeSessionIndex < sessionCount ? sessionList[activeSessionIndex] : sessionList[0]) : null
    readonly property string currentSessionDevice: currentSession && currentSession.device ? String(currentSession.device) : ""
    readonly property int currentSessionPosition: currentSession && currentSession.mediaPosition !== undefined ? Number(currentSession.mediaPosition) : 0
    readonly property int currentSessionDuration: currentSession && currentSession.mediaDuration !== undefined ? Number(currentSession.mediaDuration) : 0
    readonly property bool currentSessionPlaying: !!(currentSession && currentSession.playing)
    readonly property bool hasSessionForSelectedDevice: !!(kcast && typeof deviceSelector.currentText === "string" && deviceSelector.currentText.length > 0 && kcast.hasSessionForDevice(deviceSelector.currentText))
    readonly property bool seekPending: pendingSeekPosition >= 0
    readonly property bool canSeek: !!(currentSession && currentSession.mediaSeekable && currentSessionDuration > 0)
    readonly property bool canPlay: controlsEnabled && hasMedia

    function refreshDevices() {
        startScan();
    }

    function devs() {
        return (kcast && kcast.devices !== undefined && kcast.devices !== null) ? kcast.devices : [];
    }

    function deviceOptions() {
        const sharedDevices = devs();
        if (sharedDevices && sharedDevices.length !== undefined && sharedDevices.length > 0)
            return sharedDevices;

        if (kcast && kcast.defaultDevice && kcast.defaultDevice.length > 0)
            return [kcast.defaultDevice];

        return [];
    }

    function indexOfDevice(deviceName) {
        const options = deviceOptions();
        for (let i = 0; i < options.length; ++i) {
            if (String(options[i]) === deviceName)
                return i;
        }

        return -1;
    }

    function sessionIndexForDevice(deviceName) {
        if (!deviceName)
            return -1;

        for (let i = 0; i < sessionCount; ++i) {
            const session = sessionList[i];
            if (session && String(session.device || "") === deviceName)
                return i;
        }

        return -1;
    }

    function startScan() {
        devices = [];
        isScanning = true;
        kcast.scanDevicesAsync(); // asynchron, UI bleibt frei
    }

    function _play() {
        const targetDevice = selectedDevice || kcast.defaultDevice || currentSessionDevice || kcast.firstActiveSessionDevice() || "";
        const normalized = updateMediaInput(mediaUrl.text || "");
        if (!targetDevice || !normalized)
            return;

        pendingSessionDevice = targetDevice;
        if (kcast.defaultDevice !== targetDevice)
            kcast.setDefaultDevice(targetDevice);

        kcast.CastFile(normalized);
    }

    function updateMediaInput(input) {
        const normalized = kcast.normalizeMediaInput(String(input || ""));
        if (!normalized)
            return "";

        mediaUrl.text = normalized;
        kcast.mediaUrl = normalized;
        return normalized;
    }

    function formatMediaTime(totalSeconds) {
        const safeSeconds = Math.max(0, Math.floor(Number(totalSeconds) || 0));
        const hours = Math.floor(safeSeconds / 3600);
        const minutes = Math.floor((safeSeconds % 3600) / 60);
        const seconds = safeSeconds % 60;

        function pad(value) {
            return value < 10 ? "0" + value : String(value);
        }

        return pad(hours) + ":" + pad(minutes) + ":" + pad(seconds);
    }

    function clearPendingSeek(resetToCurrentSession) {
        seekSettleTimer.stop();
        pendingSeekPosition = -1;
        pendingSeekDevice = "";

        if (resetToCurrentSession)
            seekPreviewPosition = currentSessionPosition;
    }

    function queueVolumeChange() {
        pendingVolumeDevice = targetDevice;
        pendingVolumeValue = currentVolume;
        if (pendingVolumeDevice.length > 0)
            volumeDebounce.restart();
    }

    function sendVolumeImmediately() {
        const device = pendingVolumeDevice.length > 0 ? pendingVolumeDevice : targetDevice;
        const value = pendingVolumeValue >= 0 ? pendingVolumeValue : currentVolume;
        pendingVolumeDevice = "";
        pendingVolumeValue = -1;

        if (!device || !kcast || !kcast.setVolumeForDevice)
            return false;

        return kcast.setVolumeForDevice(device, value);
    }

    function requestSeekPosition(seconds) {
        if (!canSeek || !currentSessionDevice)
            return false;

        const maxPosition = currentSessionDuration > 0 ? currentSessionDuration : seconds;
        const targetPosition = Math.max(0, Math.min(Math.round(seconds), maxPosition));

        seekPreviewPosition = targetPosition;
        pendingSeekPosition = targetPosition;
        pendingSeekDevice = currentSessionDevice;
        seekSettleTimer.restart();

        if (!kcast.seekOnDevice(currentSessionDevice, targetPosition)) {
            clearPendingSeek(false);
            seekPreviewPosition = currentSessionPosition;
            return false;
        }

        return true;
    }

    function jumpSeek(deltaSeconds) {
        if (!canSeek)
            return;

        const basePosition = seekPending ? pendingSeekPosition : currentSessionPosition;
        requestSeekPosition(basePosition + deltaSeconds);
    }

    function syncCurrentSessionUi() {
        if (!currentSession) {
            clearPendingSeek(false);
            seekPreviewPosition = 0;
            if (!volumeSlider.pressed)
                currentVolume = 50;
            muted = false;
            return;
        }

        if (seekPending && pendingSeekDevice !== currentSessionDevice)
            clearPendingSeek(false);

        if (!seekSlider.pressed) {
            if (seekPending) {
                if (Math.abs(currentSessionPosition - pendingSeekPosition) <= seekSettleTolerance) {
                    clearPendingSeek(false);
                    seekPreviewPosition = currentSessionPosition;
                } else {
                    seekPreviewPosition = pendingSeekPosition;
                }
            } else if (currentSessionDuration > 0) {
                seekPreviewPosition = Math.min(currentSessionPosition, currentSessionDuration);
            } else {
                seekPreviewPosition = currentSessionPosition;
            }
        }

        if (!volumeSlider.pressed)
            currentVolume = currentSession.volume !== undefined ? Number(currentSession.volume) : 50;
        muted = !!currentSession.muted;
    }

    function selectSessionDevice(deviceName) {
        if (!deviceName || sessionIndexForDevice(deviceName) < 0)
            return;

        activeSessionDevice = deviceName;
    }

    function _pause() {
        if (currentSessionDevice)
            kcast.pauseMedia(currentSessionDevice);
    }

    function _resume() {
        if (currentSessionDevice)
            kcast.resumeMedia(currentSessionDevice);
    }

    function _stop() {
        if (currentSessionDevice)
            kcast.stopMedia(currentSessionDevice);
    }

    Component.onCompleted: {
        mediaUrl.text = kcast.mediaUrl;
        syncCurrentSessionUi();
        if (!kcast) {
            console.warn(i18n("Plugin not available!"));
            return ;
        }
        if (!kcast.isCattInstalled()) {
            console.warn(i18n("You need to install 'catt' first!"));
            return ;
        }
        console.log("[KCast] DBus registration started");
        const ok = kcast.registerDBus();
        if (!ok)
            console.warn("[KCast] DBus registration failed");

        if (Plasmoid.configuration.defaultDevice && Plasmoid.configuration.defaultDevice.length > 0)
            kcast.setDefaultDevice(Plasmoid.configuration.defaultDevice);

        if (!kcast.defaultDevice || kcast.defaultDevice.length === 0)
            startScan();

    }
    onCurrentSessionChanged: syncCurrentSessionUi()
    onCurrentSessionDeviceChanged: syncCurrentSessionUi()
    onSessionListChanged: {
        if (!hasSession) {
            activeSessionDevice = "";
            pendingSessionDevice = "";
            syncCurrentSessionUi();
            return ;
        }

        if (pendingSessionDevice.length > 0) {
            selectSessionDevice(pendingSessionDevice);
            pendingSessionDevice = "";
        }

        if (!activeSessionDevice || sessionIndexForDevice(activeSessionDevice) < 0) {
            const fallbackSession = sessionList[0];
            activeSessionDevice = fallbackSession && fallbackSession.device ? String(fallbackSession.device) : "";
        }

        syncCurrentSessionUi();
    }
    Layout.minimumWidth: Math.max(320, contentColumn.implicitWidth + (pageMargin * 2))
    Layout.minimumHeight: implicitHeight
    implicitWidth: Math.max(320, contentColumn.implicitWidth + (pageMargin * 2))
    implicitHeight: contentColumn.implicitHeight + (pageMargin * 2)

    Timer {
        id: volumeDebounce

        interval: 80
        repeat: false
        onTriggered: {
            sendVolumeImmediately();
        }
    }

    Timer {
        id: seekSettleTimer

        interval: 1200
        repeat: false
        onTriggered: {
            clearPendingSeek(true);
        }
    }

    KCastBridge {
        id: kcast
    }

    DropArea {
        // Optional: Timeout oder sofort schließen

        anchors.fill: parent
        onDropped: function(drop) {
            var url = "";
            if (drop.hasUrls && drop.urls.length > 0)
                url = drop.urls[0];
            else if (drop.hasText)
                url = drop.text;
            if (url !== "") {
                console.log(i18n("URL detected: %1").arg(url));
                updateMediaInput(url);
            } else {
                console.log(i18n("Not a valid url"));
                drop.accept(Qt.IgnoreAction);
            }
        }
        onExited: {
            if (root.keepOpenDuringDrop)
                Qt.callLater(() => {
                root.plasmoidItem.expanded = false;
            });

        }
    }

    ColumnLayout {
        id: contentColumn

        // Platzhalter

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: pageMargin
        spacing: sectionSpacing

        RowLayout {
            Item {
                id: logoWrapper

                width: 64
                height: 64
                // ToolTip.visible: kcastIcon.containsMouse
                ToolTip.delay: 500
                ToolTip.text: "KCast"

                Image {
                    id: kcastIcon

                    anchors.centerIn: parent
                    source: Qt.resolvedUrl("../icons/kcast_icon_64x64.png")
                    width: 64
                    height: 64
                    fillMode: Image.PreserveAspectFit
                }

            }

            Kirigami.Heading {
                text: i18n("KCast")
                level: 2
                Layout.fillWidth: true
            }

        }

        PlasmaComponents.Label {
            text: deviceOptions().length > 0 ? i18n("Select device:") : (isScanning ? i18n("Searching for devices...") : i18n("No device found"))
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }

        // 1) Device-Liste (ComboBox)
        RowLayout {
            id: deviceList
            Layout.fillWidth: true

            PlasmaComponents.ComboBox {
                id: deviceSelector

                Layout.fillWidth: true
                model: deviceOptions()
                Component.onCompleted: {
                    const deviceIndex = indexOfDevice(kcast.defaultDevice);
                    if (deviceIndex >= 0)
                        currentIndex = deviceIndex;
                    else if (deviceOptions().length > 0)
                        currentIndex = 0;
                }
                onActivated: (i) => {
                    const options = deviceOptions();
                    if (i >= 0 && i < options.length)
                        kcast.setDefaultDevice(options[i]);

                }
            }

            PlasmaComponents.Button {
                text: i18n("search devices")
                icon.name: "view-refresh"
                Layout.alignment: Qt.AlignRight
                onClicked: {
                    refreshDevices();
                }
            }

        }

        RowLayout {
            TextField {
                id: mediaUrl

                Layout.fillWidth: true
                placeholderText: i18n("http://... or /path/to/file.mp4")
                // 1) UI initial mit Bridge befüllen
                Component.onCompleted: mediaUrl.text = kcast.mediaUrl
                // 3) Wenn der Nutzer tippt → zurück in die Bridge spiegeln
                onTextEdited: kcast.mediaUrl = text

                // 2) Wenn die Bridge (z.B. via D-Bus) mediaUrl ändert → UI nachziehen
                Connections {
                    function onMediaUrlChanged() {
                        mediaUrl.text = kcast.mediaUrl;
                    }

                    target: kcast
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.RightButton
                    onPressed: {
                        if (mouse.button === Qt.RightButton)
                            menu.popup();

                    }

                    Menu {
                        id: menu

                        MenuItem {
                            text: i18n("copy")
                            enabled: mediaUrl.selectedText.length > 0
                            onTriggered: mediaUrl.copy()
                        }

                        MenuItem {
                            text: i18n("paste")
                            onTriggered: mediaUrl.paste()
                        }

                        MenuItem {
                            text: i18n("cut")
                            enabled: mediaUrl.selectedText.length > 0
                            onTriggered: mediaUrl.cut()
                        }

                        MenuItem {
                            text: i18n("select all")
                            onTriggered: mediaUrl.selectAll()
                        }

                    }

                }

            }

            PlasmaComponents.Button {
                text: i18n("open")
                icon.name: "folder-video"
                Layout.alignment: Qt.AlignRight
                onClicked: {
                    fileDialog.open();
                }
            }

        }

        RowLayout {
            id: mediaControls

            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            spacing: controlSpacing

            PlasmaComponents.Button {
                id: playBtn

                text: hasSessionForSelectedDevice ? i18n("Replace Cast") : i18n("Cast")
                icon.name: "media-playback-start"
                enabled: canPlay
                onClicked: {
                    _play();
                }
            }

            PlasmaComponents.Button {
                id: pauseBtn

                text: currentSessionPlaying ? i18n("Pause") : i18n("Resume")
                icon.name: currentSessionPlaying ? "media-playback-pause" : "media-playback-start"
                enabled: currentSessionDevice.length > 0
                onClicked: currentSessionPlaying ? _pause() : _resume()
            }

            PlasmaComponents.Button {
                text: "Stop"
                icon.name: "media-playback-stop"
                enabled: currentSessionDevice.length > 0
                onClicked: _stop()
            }

        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: hasSession
            spacing: controlSpacing

            PlasmaComponents.Label {
                Layout.fillWidth: true
                text: sessionCount === 1 ? i18n("Active session") : i18n("Active sessions")
            }

            TabBar {
                id: sessionTabs

                Layout.fillWidth: true
                currentIndex: activeSessionIndex >= 0 ? activeSessionIndex : 0
                onCurrentIndexChanged: {
                    if (currentIndex < 0 || currentIndex >= sessionCount)
                        return ;

                    const session = sessionList[currentIndex];
                    const deviceName = session && session.device ? String(session.device) : "";
                    if (deviceName.length > 0 && activeSessionDevice !== deviceName)
                        activeSessionDevice = deviceName;
                }

                Repeater {
                    model: sessionList

                    TabButton {
                        text: modelData && modelData.device ? String(modelData.device) : i18n("Unknown device")
                    }
                }
            }

            PlasmaComponents.Label {
                Layout.fillWidth: true
                text: currentSessionPlaying ? i18n("Playing on %1").arg(currentSessionDevice) : i18n("Paused on %1").arg(currentSessionDevice)
                visible: currentSessionDevice.length > 0
                elide: Text.ElideRight
            }
        }

        RowLayout {
            id: seekControls

            Layout.fillWidth: true
            visible: hasSession
            spacing: controlSpacing

            PlasmaComponents.Label {
                text: formatMediaTime((seekSlider.pressed || seekPending) ? seekPreviewPosition : currentSessionPosition)
                Layout.alignment: Qt.AlignVCenter
            }

            PlasmaComponents.Button {
                text: i18n("-%1s").arg(seekJumpSeconds)
                enabled: canSeek
                onClicked: jumpSeek(-seekJumpSeconds)
            }

            PlasmaComponents.Slider {
                id: seekSlider

                Layout.fillWidth: true
                from: 0
                to: Math.max(1, currentSessionDuration)
                stepSize: 1
                live: true
                enabled: canSeek
                value: seekPreviewPosition
                onValueChanged: {
                    if (pressed)
                        seekPreviewPosition = Math.round(value);
                }
                onPressedChanged: {
                    if (pressed) {
                        seekPreviewPosition = Math.round(value);
                        return ;
                    }

                    if (!canSeek)
                        return ;

                    requestSeekPosition(value);
                }
            }

            PlasmaComponents.Button {
                text: i18n("+%1s").arg(seekJumpSeconds)
                enabled: canSeek
                onClicked: jumpSeek(seekJumpSeconds)
            }

            PlasmaComponents.Label {
                text: currentSessionDuration > 0 ? formatMediaTime(currentSessionDuration) : "--:--:--"
                Layout.alignment: Qt.AlignVCenter
            }
        }

        RowLayout {
            id: volumeControls

            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            spacing: controlSpacing

            PlasmaComponents.Button {
                id: muteBtn

                enabled: targetDevice.length > 0
                checkable: true
                checked: muted
                icon.name: muted ? "audio-volume-muted" : "audio-volume-high"
                // text: muted ? i18n("Unmute") : i18n("Mute")
                Accessible.name: checked ? "Unmute" : "Mute"
                onClicked: {
                    muted = muteBtn.checked;
                    if (targetDevice.length > 0)
                        kcast.setMutedForDevice(targetDevice, muteBtn.checked);
                }
            }

            PlasmaComponents.Button {
                // icon.name: "media-volume-down"
                text: i18n("-")
                enabled: targetDevice.length > 0
                onClicked: {
                    currentVolume = Math.max(0, currentVolume - volumeStepBig);
                    queueVolumeChange();
                }
            }

            PlasmaComponents.Slider {
                id: volumeSlider

                Layout.fillWidth: true
                from: 0
                to: 100
                stepSize: volumeStepSmall
                live: true
                value: currentVolume
                enabled: targetDevice.length > 0
                // Beim Ziehen: nur throttled (Debounce) senden
                onValueChanged: {
                    if (!pressed)
                        return ;

                    // nur wenn der User wirklich schiebt
                    currentVolume = Math.round(value);
                    queueVolumeChange();
                }
                // „Loslassen“-Moment: final commit (ersetzt onReleased)
                onPressedChanged: {
                    if (pressed)
                        return ;

                    // wird false => Finger/Maus losgelassen
                    if (!kcast || !kcast.setVolumeForDevice)
                        return ;

                    currentVolume = Math.round(value);
                    pendingVolumeDevice = targetDevice;
                    pendingVolumeValue = currentVolume;
                    sendVolumeImmediately();
                }
                Keys.onPressed: (ev) => {
                    if (ev.key === Qt.Key_Left) {
                        currentVolume = Math.max(0, currentVolume - volumeStepSmall);
                        queueVolumeChange();
                        ev.accepted = true;
                    }
                    if (ev.key === Qt.Key_Right) {
                        currentVolume = Math.min(100, currentVolume + volumeStepSmall);
                        queueVolumeChange();
                        ev.accepted = true;
                    }
                }

                WheelHandler {
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (ev) => {
                        const d = ev.angleDelta.y > 0 ? volumeStepSmall : -volumeStepSmall;
                        currentVolume = Math.max(0, Math.min(100, currentVolume + d));
                        queueVolumeChange();
                        ev.accepted = true;
                    }
                }

            }

            PlasmaComponents.Label {
                // minimumWidth: implicitWidth

                text: currentVolume + "%"
                Accessible.name: i18n("Volume in %")
                Layout.alignment: Qt.AlignVCenter
            }

            PlasmaComponents.Button {
                // icon.name: "media-volume-up"
                text: i18n("+")
                enabled: targetDevice.length > 0
                onClicked: {
                    currentVolume = Math.min(100, currentVolume + volumeStepBig); // sofort im UI
                    queueVolumeChange(); // nach kurzer Zeit >= setVolume()
                }
            }

        }

        Platform.FileDialog {
            id: fileDialog

            title: i18n("Open file")
            nameFilters: ["Media (*.mp4 *.mkv *.webm *.mp3)", "Alle Dateien (*)"]
            onAccepted: {
                updateMediaInput(file);
            }
        }

        Connections {
            // erstes gefundenes nehmen
            // z.B. eine Fehlermeldung sichtbar schalten

            function onDefaultDeviceChanged(name) {
                const deviceIndex = indexOfDevice(name);
                if (deviceIndex >= 0 && deviceSelector.currentIndex !== deviceIndex)
                    deviceSelector.currentIndex = deviceIndex;
            }

            function onDevicesChanged() {
                if (deviceSelector.currentIndex >= 0)
                    return ;

                const deviceIndex = indexOfDevice(kcast.defaultDevice);
                if (deviceIndex >= 0)
                    deviceSelector.currentIndex = deviceIndex;
                else if (deviceOptions().length > 0)
                    deviceSelector.currentIndex = 0;
            }

            function onDeviceFound(name) {
                if (devices.indexOf(name) === -1)
                    devices = devices.concat([name]);

                // trigger Bindings
                if (!kcast.defaultDevice || kcast.defaultDevice.length === 0)
                    kcast.setDefaultDevice(name);

            }

            function onDevicesScanned(list) {
                devices = list ? list : [];
                isScanning = false;
                // Optional: InlineMessage zeigen, falls leer
                if (devices.length === 0) {
                }
            }

            target: kcast
        }

    }

}
