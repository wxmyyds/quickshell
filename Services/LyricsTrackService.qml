pragma Singleton

import QtQuick
import Quickshell
import Clavis.Lyrics

Singleton {
    id: root

    readonly property var player: MediaManager.active

    function sync() {
        const current = root.player;
        if (!current) {
            Lyrics.clearTrack();
            return;
        }
        // MPRIS selects the player; uniqueId invalidates a replaced instance.
        // The native consumer binds Cirrus observation sockets to its bus owner.
        // Other players expose only their ordinary xesam:asText metadata.
        Lyrics.setMetadata(String(current.dbusName || ""), current.metadata || {}, String(current.uniqueId
                                                                                          || ""));
    }

    function initialize() {
        root.sync();
    }

    Component.onCompleted: root.sync()

    Connections {
        target: MediaManager

        function onActiveChanged() {
            root.sync();
        }
    }

    Connections {
        target: root.player
        ignoreUnknownSignals: true

        function onMetadataChanged() {
            root.sync();
        }
        function onDbusNameChanged() {
            root.sync();
        }
        function onUniqueIdChanged() {
            root.sync();
        }
    }
}
