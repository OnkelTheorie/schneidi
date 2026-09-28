#pragma once
#include <QColor>

class QApplication;

// Farben an DaVinci Resolve angelehnt. Alle Widgets holen ihre Farben von hier.
namespace Theme {

inline const QColor window{0x1f, 0x1f, 0x23};
inline const QColor panel{0x28, 0x28, 0x2e};
inline const QColor panelHeader{0x2f, 0x2f, 0x35};
inline const QColor border{0x14, 0x14, 0x17};
inline const QColor text{0xd2, 0xd2, 0xd6};
inline const QColor textDim{0x8c, 0x8c, 0x94};
inline const QColor accent{0xe8, 0x7a, 0x3a};      // Orange (Auswahl, aktive Buttons)

inline const QColor timelineBg{0x1a, 0x1a, 0x1d};
inline const QColor trackBg{0x22, 0x22, 0x27};
inline const QColor trackBgAlt{0x1f, 0x1f, 0x24};
inline const QColor trackHeader{0x2a, 0x2a, 0x30};
inline const QColor ruler{0x26, 0x26, 0x2b};
inline const QColor playhead{0xe8, 0x41, 0x4a};
inline const QColor videoClip{0x3b, 0x6a, 0xa0};
inline const QColor audioClip{0x3c, 0x86, 0x4c};
inline const QColor titleClip{0x86, 0x5c, 0xa8};   // Titel lila wie Resolve
inline const QColor clipSelected{0xf0, 0x8a, 0x3c};

void apply(QApplication& app);

} // namespace Theme
