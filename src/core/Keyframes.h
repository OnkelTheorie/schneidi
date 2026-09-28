#pragma once
// Keyframes (wie DaVinci): reine Funktionen auf Clip::keys, ohne Undo/Signale (das macht Project::edit).
// Zeiten sind hier Clip-Frames t (0 = erstes Frame des Clips), gespeichert wird der Quell-Frame (Clip::in + t).
// Zwischen zwei Keyframes wird interpoliert (linear bzw. Ease-Kurve), vor dem ersten/nach dem letzten gilt
// dessen Wert. Ohne Keyframes gilt der statische Wert im Clip.

#include "core/Types.h"

#include <QColor>

namespace Keys {

struct ParamInfo {
    AnimParam param;
    const char* id; // Schlüssel in der Projektdatei
    bool color;     // Wert = ARGB-Zahl, Kanäle einzeln interpolieren
};
const ParamInfo& info(AnimParam p);
bool fromId(const QString& id, AnimParam* p);

// Farbe <-> Keyframe-Wert (ARGB als Zahl, in double exakt darstellbar)
inline double fromColor(const QColor& c) { return double(c.rgba()); }
inline QColor toColor(double v) { return QColor::fromRgba(QRgb(quint32(v))); }

double staticValue(const Clip& c, AnimParam p);
void setStaticValue(Clip& c, AnimParam p, double v);

bool animated(const Clip& c, AnimParam p);
bool hasKeys(const Clip& c);
// Wert an Clip-Frame t (auch Bruchteile)
double valueAt(const Clip& c, AnimParam p, double t);
// Keyframe genau an Clip-Frame t (nullptr = keiner)
const Keyframe* keyAt(const Clip& c, AnimParam p, int t);

void setKey(Clip& c, AnimParam p, int t, double v);  // setzt oder ersetzt (Verlauf bleibt)
void removeKey(Clip& c, AnimParam p, int t);         // letzter weg -> Wert bleibt als statischer Wert
// Wert ändern wie im DaVinci-Inspector: animiert -> Keyframe an t (neu oder ersetzt), sonst statischer Wert
void setValue(Clip& c, AnimParam p, int t, double v);
void clear(Clip& c, AnimParam p);                    // alle Keyframes weg, statischer Wert bleibt

// Clip-Frames aller Keyframes (sortiert, ohne Doppelte); params leer = alle Parameter
QVector<int> keyTimes(const Clip& c, const QVector<AnimParam>& params = {});
// Verläuft der Parameter zwischen t0 und t1 (benachbarte Keyframe-Zeiten) linear bzw. konstant?
bool linearBetween(const Clip& c, AnimParam p, int t0, int t1);

// Timeline-Keyframe-Spur: alle Parameter an den Zeiten gemeinsam
void setEase(Clip& c, const QVector<int>& times, KeyEase ease, const QVector<AnimParam>& params = {});
void removeAt(Clip& c, const QVector<int>& times, const QVector<AnimParam>& params = {});
void move(Clip& c, const QVector<int>& times, int delta);

// Nach dem Teilen (left/right = Kopien mit neuem In/Out): Keyframes aufteilen, an der Schnittkante
// den interpolierten Wert als Keyframe setzen, damit beide Teile genau wie vorher aussehen.
void split(const Clip& original, Clip& left, Clip& right);

// Bereich aktiv und (statisch oder animiert) nicht neutral
bool hasTransform(const Clip& c);
bool hasCrop(const Clip& c);
bool hasOpacity(const Clip& c);

} // namespace Keys
