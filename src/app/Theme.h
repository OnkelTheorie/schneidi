#pragma once
#include <QColor>
#include <QHash>
#include <QList>
#include <QString>

class QApplication;

// Farben der Oberfläche. Alle Widgets holen ihre Farben von hier (nie fest eintragen).
// Designs liegen als Daten vor (assets/themes/*.json, eingebaut als Ressource). Beim Start setzt load() die Werte
// unten aus dem gewählten Design + Nutzerwahl (QSettings); eine Änderung wirkt nach dem Neustart (wie die Sprache).
// Die Vorgabewerte hier entsprechen dem Design „Dark Orange“ (gilt auch, wenn load() nie läuft, z. B. in Tests).
namespace Theme {

// ---- Design-Farben (ändern sich mit dem Design) ----
inline QColor window{0x1f, 0x1f, 0x23};
inline QColor panel{0x28, 0x28, 0x2e};
inline QColor panelHeader{0x2f, 0x2f, 0x35};
inline QColor topBar{0x2a, 0x2a, 0x30};       // obere Leiste mit Panel-Schaltern
inline QColor border{0x14, 0x14, 0x17};
inline QColor well{0x14, 0x14, 0x17};         // eingelassene Felder (Pegel, Fortschritt, Timecode)
inline QColor field{0x1b, 0x1b, 0x1f};        // Eingabefelder
inline QColor control{0x3a, 0x3a, 0x42};      // Schaltflächen, Hover
inline QColor controlLight{0x45, 0x45, 0x4d}; // Scrollbalken, Trenner
inline QColor controlOff{0x55, 0x55, 0x5c};   // aus / deaktiviert (auch deaktivierte Clips)
inline QColor text{0xd2, 0xd2, 0xd6};
inline QColor textDim{0x8c, 0x8c, 0x94};
inline QColor textFaint{0x5a, 0x5a, 0x62};    // abgeschaltete Schrift, feine Skalen
inline QColor timelineBg{0x1a, 0x1a, 0x1d};
inline QColor trackBg{0x22, 0x22, 0x27};
inline QColor trackBgAlt{0x1f, 0x1f, 0x24};
inline QColor trackHeader{0x2a, 0x2a, 0x30};
inline QColor ruler{0x26, 0x26, 0x2b};
inline QColor lane{0x1d, 0x1d, 0x21};         // Keyframe-Spur, Kurven-Editor
inline QColor viewerBg{0x10, 0x10, 0x12};
inline QColor thumbBg{0x18, 0x18, 0x1b};      // Vorschaubilder ohne Bild
inline QColor primary{0xe8, 0x7a, 0x3a};      // aktive Schalter, Auswahl, Regler-Füllung, Fortschritt
inline QColor secondary{0xa8, 0xc8, 0xf0};    // Keyframes, Marker, In/Out, Hover beim Ziehen
// abgeleitet (nicht einstellbar)
inline QColor onPrimary{Qt::black};           // Schrift/Symbole auf der Primärfarbe
inline QColor clipSelected{0xeb, 0x8a, 0x50}; // Rand ausgewählter Clips (aus der Primärfarbe)

// ---- Signalfarben (bleiben bei jedem Design gleich, einzeln einstellbar) ----
inline QColor playhead{0xe8, 0x41, 0x4a};
inline QColor videoClip{0x3b, 0x6a, 0xa0};
inline QColor audioClip{0x3c, 0x86, 0x4c};
inline QColor titleClip{0x86, 0x5c, 0xa8};    // Titel lila
inline QColor compoundClip{0x9a, 0x7b, 0x3c}; // Compound Clips / verschachtelte Timelines (ocker)
inline QColor subtitleClip{0x4d, 0x7d, 0x84}; // Untertitel (graublau)
inline QColor meterLow{0x3c, 0xc0, 0x5a};     // Pegel grün
inline QColor meterMid{0xe0, 0xc0, 0x3a};     // Pegel gelb (auch Solo)
inline QColor meterHigh{0xe8, 0x41, 0x4a};    // Pegel rot
inline QColor warning{0xe8, 0x41, 0x4a};      // Übersteuerung, Fehler, Stumm, Keyframe am Playhead
inline QColor quiet{0x3a, 0x8c, 0xc8};        // Lautheit unter dem Ziel
inline QColor cacheReady{0x3d, 0x8b, 0xe8};   // Render-Cache fertig
inline QColor cacheMissing{0xd2, 0x3c, 0x3c}; // Render-Cache fehlt

// ---- Scopes of the Color page (fixed like video signal colours, not part of the designs) ----
inline QColor scopeTrace{0xdc, 0xe8, 0xdc}; // Waveform/Vectorscope trace, Histogram luma
inline QColor scopeRed{0xff, 0x4c, 0x4c};
inline QColor scopeGreen{0x4c, 0xe0, 0x5c};
inline QColor scopeBlue{0x50, 0x84, 0xff};
inline QColor scopeSkin{0xe8, 0xa8, 0x78};  // skin tone line in the vectorscope

using Colors = QHash<QString, QColor>; // Schlüssel wie in den JSON-Dateien (z. B. "panel", "primary", "playhead")

struct Design {
    QString id;   // Dateiname ohne .json
    QString name;        // Anzeigename (übersetzt)
    QString description; // eine Zeile für den Dialog (übersetzt)
};
QList<Design> designs();           // eingebaute Designs in fester Reihenfolge
QString defaultDesign();           // "dark-orange"
QString savedDesign();             // gewähltes Design (QSettings), unbekannt -> defaultDesign()
QStringList signalKeys();          // Reihenfolge für den Dialog
QString signalLabel(const QString& key);

Colors defaults(const QString& design); // Vorgabe des Designs + Vorgabe-Signalfarben
Colors saved(const QString& design);    // wie defaults(), plus gespeicherte Primär-/Sekundär- und Signalfarben
void save(const QString& design, const Colors& chosen); // speichert Design + Abweichungen von der Vorgabe

void activate(const Colors& colors); // setzt die Farben oben (fehlende Schlüssel bleiben)
void load(const QString& overrideDesign = {}); // beim Start vor apply(); override nur für diesen Lauf (--design)
bool restartNeeded(); // gespeicherte Wahl weicht von den beim Start geladenen Farben ab
void apply(QApplication& app);

// Hilfen
QColor mix(const QColor& a, const QColor& b, double t); // t = 0 -> a, 1 -> b
QColor readableOn(const QColor& background);             // schwarz oder weiß
QColor selectionFrom(const QColor& primary);             // Rand ausgewählter Clips zur Primärfarbe
QColor alpha(const QColor& c, int a);
QString primaryButtonStyle(); // Stylesheet für die große Haupt-Schaltfläche (z. B. „Rendern“)

} // namespace Theme
