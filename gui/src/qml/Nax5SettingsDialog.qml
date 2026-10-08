import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

import "controls" as C

DialogView {
    id: dialog
    title: qsTr("Настройки")
    buttonVisible: false

    Item {
        // Keep the focused control on screen while navigating with the D-pad.
        Window.onActiveFocusItemChanged: {
            const item = Window.activeFocusItem;
            if (!item || !flick.contentItem)
                return;
            // Scroll to the whole row (label + control), not just the focused control.
            let row = item;
            while (row && row.parent !== layout)
                row = row.parent;
            if (!row)
                return;
            // Also show a heading or hint (a plain Label: has text, takes no focus) right above the row.
            const rows = layout.children;
            let index = -1;
            for (let i = 0; i < rows.length; ++i)
                if (rows[i] === row)
                    index = i;
            let first = row;
            while (index > 0 && rows[index - 1].visible && rows[index - 1].text !== undefined
                   && !rows[index - 1].activeFocusOnTab) {
                first = rows[index - 1];
                index--;
            }
            const maxY = Math.max(0, flick.contentHeight - flick.height);
            const top = index <= 0 ? 0 : first.mapToItem(flick.contentItem, 0, 0).y - 12;
            const bottom = row.mapToItem(flick.contentItem, 0, 0).y + row.height + 12;
            if (top < flick.contentY)
                flick.contentY = Math.max(0, top);
            else if (bottom > flick.contentY + flick.height)
                flick.contentY = Math.min(maxY, bottom - flick.height);
        }

        Flickable {
            id: flick
            anchors.fill: parent
            clip: true
            contentWidth: width
            contentHeight: layout.implicitHeight + 40
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AlwaysOn
                visible: flick.contentHeight > flick.height
            }

        ColumnLayout {
            id: layout
            width: Math.max(0, flick.width - 48)
            x: 24
            y: 20
            spacing: 16

            Label {
                Layout.fillWidth: true
                text: qsTr("Качество Remote Play")
                font.pixelSize: 18
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Устройство") }
                C.ComboBox {
                    Layout.fillWidth: true
                    firstInFocusChain: true
                    model: [qsTr("Steam Deck OLED"), qsTr("Steam Deck (LCD)")]
                    currentIndex: Chiaki.settings.deviceProfile === "lcd" ? 1 : 0
                    onActivated: function(index) { Chiaki.settings.deviceProfile = index === 1 ? "lcd" : "oled" }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#c5c5c5"
                text: Chiaki.settings.deviceProfile === "lcd"
                      ? qsTr("Обычная картинка без HDR — для Steam Deck с LCD-экраном.")
                      : qsTr("HDR-картинка с глубоким чёрным — для Steam Deck OLED.")
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Разрешение") }
                C.ComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("360p"), qsTr("540p"), qsTr("720p"), qsTr("1080p")]
                    currentIndex: Chiaki.settings.resolutionRemotePS5 - 1
                    onActivated: function(index) {
                        Chiaki.settings.resolutionRemotePS5 = index + 1
                        Chiaki.settings.bitrateRemotePS5 = 0
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Частота кадров") }
                C.ComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("30 FPS"), qsTr("60 FPS")]
                    currentIndex: (Chiaki.settings.fpsRemotePS5 / 30) - 1
                    onActivated: function(index) { Chiaki.settings.fpsRemotePS5 = (index + 1) * 30 }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Кодек") }
                C.ComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("H.264"), qsTr("H.265"), qsTr("H.265 HDR")]
                    currentIndex: Chiaki.settings.codecRemotePS5
                    onActivated: function(index) { Chiaki.settings.codecRemotePS5 = index }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Декодер") }
                C.ComboBox {
                    Layout.fillWidth: true
                    model: Chiaki.settings.availableDecoders
                    currentIndex: Math.max(0, model.indexOf(Chiaki.settings.decoder))
                    onActivated: function(index) { Chiaki.settings.decoder = index ? model[index] : "" }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Битрейт: %1 Мбит/с").arg(bitrate.value) }
                C.Slider {
                    id: bitrate
                    Layout.fillWidth: true
                    from: 2
                    to: 100
                    stepSize: 1
                    value: Chiaki.settings.bitrateRemotePS5 > 0 ? Chiaki.settings.bitrateRemotePS5 / 1000 : 10
                    onMoved: Chiaki.settings.bitrateRemotePS5 = value * 1000
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Громкость: %1%").arg(volume.value) }
                C.Slider {
                    id: volume
                    Layout.fillWidth: true
                    from: 0
                    to: 100
                    stepSize: 1
                    value: Chiaki.settings.audioVolume
                    onMoved: Chiaki.settings.audioVolume = value
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Диагностика для beta-тестеров")
                font.pixelSize: 18
                font.bold: true
            }

            C.CheckBox {
                id: verboseLogs
                Layout.fillWidth: true
                text: qsTr("Подробные логи (для beta)")
                checked: Chiaki.settings.logVerbose
                onToggled: Chiaki.settings.logVerbose = checked
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#ffcc80"
                text: qsTr("Увеличивает размер логов, включайте только по просьбе поддержки.")
            }

            C.CheckBox {
                id: streamStats
                Layout.fillWidth: true
                text: qsTr("Показывать статистику стрима")
                checked: Chiaki.settings.showStreamStats
                lastInFocusChain: !Chiaki.operatorMode
                onToggled: Chiaki.settings.showStreamStats = checked
            }

            Button {
                visible: Chiaki.operatorMode
                text: qsTr("Полные настройки Chiaki")
                Material.roundedScale: Material.SmallScale
                onClicked: root.showChiakiSettingsDialog()
            }
        }
        }
    }
}
