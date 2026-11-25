import QtQuick
import Clavis.Lyrics
import qs.Services

Item {
    id: root

    required property var player
    property bool vertical: false
    property string edge: "top"
    // Native observation timing supplies -1 before the first timed line and
    // holds the last started line across gaps; this presenter is display-only.
    readonly property var lyricsModel: Lyrics.lyrics
    readonly property string artUrl: player ? player.trackArtUrl || "" : ""
    readonly property int currentLineIndex: Lyrics.currentLine
    readonly property string currentLyric: Lyrics.currentText

    implicitWidth: presenter.item ? presenter.item.implicitWidth : 0
    implicitHeight: presenter.item ? presenter.item.implicitHeight : 0
    Loader {
        id: presenter

        anchors.fill: parent
        sourceComponent: root.vertical ? verticalComponent : horizontalComponent
    }

    Component {
        id: horizontalComponent

        HorizontalLyricsLayout {
            // StrictlyEnforceRange follows the native index without pointer input.
            // Hiding the presenter also prevents a first-line flash at index -1.
            visible: root.currentLineIndex >= 0 || Lyrics.status !== "ready"
            lyricsModel: root.currentLineIndex >= 0 ? root.lyricsModel : []
            currentLineIndex: root.currentLineIndex
            artUrl: root.artUrl
            player: root.player
            status: Lyrics.status
            errorText: Lyrics.error
        }
    }

    Component {
        id: verticalComponent

        VerticalLyricsLayout {
            lyric: root.currentLyric
            artUrl: root.artUrl
            player: root.player
            edge: root.edge
            status: Lyrics.status
            errorText: Lyrics.error
        }
    }
}
