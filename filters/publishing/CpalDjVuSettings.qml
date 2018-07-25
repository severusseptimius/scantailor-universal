import QtQuick 2.4

CpalDjVuSettingsForm {
    property string type: "encoder"
    property string name: qsTr("cpaldjvu [DjVuLibre] for few colors")

    property string supportedInput: "jpeg;ppm;pgm;pbm"
    property string prefferedInput: "ppm"
    property string supportedOutputMode: "grayscale;color"
    property string description: qsTr("Encoder for images containing few colors. It performs best on images containing large solid color areas. This program works by first reducing the number of distinct colors to a small specified value using a simple color quantization algorithm. The dominant color is encoded into the background layer. The other colors are encoded into the foreground layer.")
    property int priority: 20
    property int _dpi_: 600



    property string current_platform: "linux"
    property string colors_val: "-colors"
    property string bgwhite_used: "-bgwhite"

    signal notify()

    onNotify: {
        mainApp.requestParamUpdate(type);
    }

    Component.onCompleted: {
        cbBgwhite.checkedChanged.connect(notify);
        sbColors.valueChanged.connect(notify);
        cbColors.checkedChanged.connect(notify);
    }

    function init(platform) {
        current_platform = platform;
        if ("linux" === platform) {
            return true;
        } else return false;
    }

    function getDependencies() {
        return {"app":"cpaldjvu", "check_cmd":"%1 --help", "search_params":"-dpi;"+colors_val+";"+bgwhite_used, "missing_app_hint": getMissingAppHint() }
    }

    function getMissingAppHint() {
        if (current_platform == "linux") {
            return qsTr("cpaldjvu is a part of djvulibre-bin package. Please install djvulibre-bin in case this appplication is missing in your system.");
        } else return "";
    }

    function getState() {
        var state = {"_dpi_": _dpi_};
        if (cbColors.checked) {
            state[colors_val] = sbColors.value;
        }
        state[bgwhite_used] = cbBgwhite.checked;

        return state;
    }

    function setState(state) {
        _dpi_ = state["_dpi_"];
        cbColors.checked = state.contains(colors_val);
        sbColors.value = (cbColors.checked ? state[colors_val] : 256);
        cbBgwhite.checked = state.contains(bgwhite_used);
    }

    function getCommandFromState(state) {
        var s = "cpaldjvu -dpi " + state["_dpi_"]
        if (state.contains(colors_val)) {
            s += " " + colors_val + " " + state[colors_val]
        }
        if (state.contains(bgwhite_used)) {
            s += " " + bgwhite_used
        }
        s += " %1 %2";
        return s
    }

    function getCommand() {
        return getCommandFromState(getState());
    }

    root.onHeightChanged: {
        mainApp.resizeQuickWidget(type);
    }

}
