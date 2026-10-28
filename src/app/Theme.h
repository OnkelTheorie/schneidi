// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QColor>
#include <QHash>
#include <QList>
#include <QString>

class QApplication;

// Colors of the UI. All widgets get their colors from here (never hardcode).
// Designs exist as data (assets/themes/*.json, built in as a resource). At startup load() sets the values
// below from the chosen design + user choice (QSettings); a change takes effect after restart (like the language).
// The default values here correspond to the "Dark Orange" design (also apply if load() never runs, e.g. in tests).
namespace Theme {

// ---- Design colors (change with the design) ----
inline QColor window{0x1f, 0x1f, 0x23};
inline QColor panel{0x28, 0x28, 0x2e};
inline QColor panelHeader{0x2f, 0x2f, 0x35};
inline QColor topBar{0x2a, 0x2a, 0x30};       // top bar with panel switches
inline QColor border{0x14, 0x14, 0x17};
inline QColor well{0x14, 0x14, 0x17};         // recessed fields (level, progress, timecode)
inline QColor field{0x1b, 0x1b, 0x1f};        // input fields
inline QColor control{0x3a, 0x3a, 0x42};      // buttons, hover
inline QColor controlLight{0x45, 0x45, 0x4d}; // scrollbars, separators
inline QColor controlOff{0x55, 0x55, 0x5c};   // off / disabled (also disabled clips)
inline QColor text{0xd2, 0xd2, 0xd6};
inline QColor textDim{0x8c, 0x8c, 0x94};
inline QColor textFaint{0x5a, 0x5a, 0x62};    // disabled text, fine scales
inline QColor timelineBg{0x1a, 0x1a, 0x1d};
inline QColor trackBg{0x22, 0x22, 0x27};
inline QColor trackBgAlt{0x1f, 0x1f, 0x24};
inline QColor trackHeader{0x2a, 0x2a, 0x30};
inline QColor ruler{0x26, 0x26, 0x2b};
inline QColor lane{0x1d, 0x1d, 0x21};         // keyframe lane, curve editor
inline QColor viewerBg{0x10, 0x10, 0x12};
inline QColor thumbBg{0x18, 0x18, 0x1b};      // thumbnails without image
inline QColor primary{0xe8, 0x7a, 0x3a};      // active switches, selection, slider fill, progress
inline QColor secondary{0xa8, 0xc8, 0xf0};    // keyframes, markers, In/Out, hover while dragging
// derived (not adjustable)
inline QColor onPrimary{Qt::black};           // text/icons on the primary color
inline QColor clipSelected{0xeb, 0x8a, 0x50}; // border of selected clips (from the primary color)

// ---- Signal colors (stay the same for every design, individually adjustable) ----
inline QColor playhead{0xe8, 0x41, 0x4a};
inline QColor videoClip{0x3b, 0x6a, 0xa0};
inline QColor audioClip{0x3c, 0x86, 0x4c};
inline QColor titleClip{0x86, 0x5c, 0xa8};    // title purple
inline QColor compoundClip{0x9a, 0x7b, 0x3c}; // Compound clips / nested timelines (ocher)
inline QColor subtitleClip{0x4d, 0x7d, 0x84}; // subtitle (gray-blue)
inline QColor meterLow{0x3c, 0xc0, 0x5a};     // level green
inline QColor meterMid{0xe0, 0xc0, 0x3a};     // level yellow (also solo)
inline QColor meterHigh{0xe8, 0x41, 0x4a};    // level red
inline QColor warning{0xe8, 0x41, 0x4a};      // overload, error, mute, keyframe at the playhead
inline QColor quiet{0x3a, 0x8c, 0xc8};        // loudness below the target
inline QColor cacheReady{0x3d, 0x8b, 0xe8};   // render cache done
inline QColor cacheMissing{0xd2, 0x3c, 0x3c}; // render cache missing
inline QColor multicamActive{0xe8, 0x2a, 0x2a}; // frame around the active angle in the multicam grid

// ---- Scopes of the Color page (fixed like video signal colours, not part of the designs) ----
inline QColor scopeTrace{0xdc, 0xe8, 0xdc}; // Waveform/Vectorscope trace, Histogram luma
inline QColor scopeRed{0xff, 0x4c, 0x4c};
inline QColor scopeGreen{0x4c, 0xe0, 0x5c};
inline QColor scopeBlue{0x50, 0x84, 0xff};
inline QColor scopeSkin{0xe8, 0xa8, 0x78};  // skin tone line in the vectorscope

using Colors = QHash<QString, QColor>; // keys as in the JSON files (e.g. "panel", "primary", "playhead")

struct Design {
    QString id;   // file name without .json
    QString name;        // display name (translated)
    QString description; // one line for the dialog (translated)
};
QList<Design> designs();           // built-in designs in fixed order
QString defaultDesign();           // "dark-orange"
QString savedDesign();             // chosen design (QSettings), unknown -> defaultDesign()
QStringList signalKeys();          // order for the dialog
QString signalLabel(const QString& key);

Colors defaults(const QString& design); // default of the design + default signal colors
Colors saved(const QString& design);    // like defaults(), plus saved primary/secondary and signal colors
void save(const QString& design, const Colors& chosen); // saves design + deviations from the default

void activate(const Colors& colors); // sets the colors above (missing keys stay)
void load(const QString& overrideDesign = {}); // at startup before apply(); override only for this run (--design)
bool restartNeeded(); // saved choice differs from the colors loaded at startup
void apply(QApplication& app);

// helpers
QColor mix(const QColor& a, const QColor& b, double t); // t = 0 -> a, 1 -> b
QColor readableOn(const QColor& background);             // black or white
QColor selectionFrom(const QColor& primary);             // border of selected clips for the primary color
QColor alpha(const QColor& c, int a);
QString primaryButtonStyle(); // stylesheet for the big main button (e.g. "Rendern")

} // namespace Theme
