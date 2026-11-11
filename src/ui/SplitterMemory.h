// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Splitters inside panels (Media Pool bins | clips, Media Storage folders | files, Effects categories | list, ...)
// remember what the user dragged: remember() restores the sizes saved under "layout/<name>" (else the defaults) and
// saves them on every drag (splitterMoved: only the user's handle moves, never setSizes). The main window's own
// splitters go through MainWindow::watchSizes; "Reset panel sizes" sets these back to their defaults (resetAll).

#include <QSettings>
#include <QSplitter>
#include <QVariantList>
#include <QWidget>
#include <algorithm>

namespace SplitterMemory {

inline void remember(QSplitter* s, const QString& name, const QList<int>& defaults)
{
    s->setObjectName(name);
    QVariantList d;
    for (int v : defaults) d << v;
    s->setProperty("schneidi_default_sizes", d);
    const QVariantList saved = QSettings().value("layout/" + name).toList();
    QList<int> sizes;
    if (saved.size() == s->count())
        for (const QVariant& v : saved) sizes << std::max(0, v.toInt());
    s->setSizes(sizes.isEmpty() ? defaults : sizes);
    QObject::connect(s, &QSplitter::splitterMoved, s, [s] {
        // Hidden parts report 0: keep their last size so they come back as they were
        const QString key = "layout/" + s->objectName();
        const QVariantList before = QSettings().value(key).toList();
        const QVariantList d = s->property("schneidi_default_sizes").toList();
        const QList<int> sizes = s->sizes();
        QVariantList out;
        for (int i = 0; i < sizes.size(); ++i)
            out << (!s->widget(i)->isHidden() ? QVariant(sizes[i]) : i < before.size() ? before[i] : d.value(i, sizes[i]));
        QSettings().setValue(key, out);
    });
}

// Every remembered splitter below `root` back to its defaults (the saved sizes are removed by the caller)
inline void resetAll(QWidget* root)
{
    for (QSplitter* s : root->findChildren<QSplitter*>()) {
        const QVariantList d = s->property("schneidi_default_sizes").toList();
        if (d.size() != s->count()) continue;
        QList<int> sizes;
        for (const QVariant& v : d) sizes << v.toInt();
        s->setSizes(sizes);
    }
}

} // namespace SplitterMemory
