// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectMaterial.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "ui/ElidedLabel.h"
#include "ui/MediaPool.h"
#include "ui/direct/DirectText.h"

#include <QAction>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

DirectMaterial::DirectMaterial(QWidget* parent) : QWidget(parent)
{
    setObjectName("direct_material");
    setAcceptDrops(true);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 4, 6, 6);
    lay->setSpacing(4);
    auto* title = new ElidedLabel;
    title->setFullText(T("Material"));
    title->setObjectName("PanelTitle");
    lay->addWidget(title);
    m_hint = new QLabel(T("Clips aus dem Medienpool oder einem Dateimanager hierher ziehen."));
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(m_hint);
    m_list = new QListWidget;
    m_list->setObjectName("direct_material_list");
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setWordWrap(true);
    m_list->setTextElideMode(Qt::ElideMiddle);
    m_list->setAcceptDrops(false); // the drop goes to the whole column
    m_list->installEventFilter(this);
    lay->addWidget(m_list, 1);
    m_useTimeline = new QPushButton(T("Aktuelle Timeline als Material"));
    m_useTimeline->setObjectName("direct_use_timeline");
    m_useTimeline->setCheckable(true);
    m_useTimeline->setToolTip(T("Die offene Timeline ist das Material (zum Beispiel ein Gespräch ohne Pausen)"));
    connect(m_useTimeline, &QPushButton::toggled, this, &DirectMaterial::setUseTimeline);
    m_clear = new QPushButton(T("Leeren"));
    m_clear->setObjectName("direct_material_clear");
    connect(m_clear, &QPushButton::clicked, this, &DirectMaterial::clear);
    lay->addWidget(m_useTimeline);
    lay->addWidget(m_clear);
    // Delete / Backspace: out of this list (InputBindings::registerPanelAction, MainWindowDirect.cpp)
    m_remove = new QAction(T("Aus dem Material nehmen"), this);
    m_remove->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    m_remove->setShortcutContext(Qt::WidgetShortcut);
    connect(m_remove, &QAction::triggered, this, &DirectMaterial::removeSelected);
    rebuild();
}

QStringList DirectMaterial::files() const { return m_files; }

void DirectMaterial::setUseTimeline(bool on)
{
    if (m_useTimeline->isChecked() != on) {
        m_useTimeline->setChecked(on); // comes back here through toggled
        return;
    }
    if (m_timeline == on) return;
    m_timeline = on;
    rebuild();
    emit changed();
}

int DirectMaterial::addFiles(const QStringList& files)
{
    int added = 0;
    for (const QString& f : files) {
        const QFileInfo fi(f);
        if (!fi.isFile()) continue;
        const QString path = fi.absoluteFilePath();
        if (m_files.contains(path)) continue;
        m_files << path;
        ++added;
    }
    if (!added) return 0;
    if (m_timeline) setUseTimeline(false); // files dropped: they are the material now
    rebuild();
    emit changed();
    return added;
}

void DirectMaterial::removeSelected()
{
    if (m_timeline) return;
    QStringList gone;
    for (QListWidgetItem* item : m_list->selectedItems()) gone << item->data(Qt::UserRole).toString();
    if (gone.isEmpty()) return;
    for (const QString& f : gone) m_files.removeAll(f);
    rebuild();
    emit changed();
}

void DirectMaterial::clear()
{
    if (m_files.isEmpty() && !m_timeline) return;
    m_files.clear();
    m_state.clear();
    m_timeline = false;
    {
        const QSignalBlocker b(m_useTimeline);
        m_useTimeline->setChecked(false);
    }
    rebuild();
    emit changed();
}

QJsonObject DirectMaterial::materialJson() const
{
    if (m_timeline) return {{"timeline", "@current"}};
    return {{"files", QJsonArray::fromStringList(m_files)}};
}

void DirectMaterial::setMaterialState(const QJsonArray& material)
{
    for (const QJsonValue& v : material) {
        const QJsonObject m = v.toObject();
        m_state.insert(m.value("file").toString(), m);
    }
    rebuild();
}

QStringList DirectMaterial::filesOf(const QMimeData* mime)
{
    QStringList out;
    if (!mime) return out;
    if (mime->hasFormat(MediaPool::ListMimeType)) {
        for (const QString& p : QString::fromUtf8(mime->data(MediaPool::ListMimeType)).split('\n', Qt::SkipEmptyParts))
            if (QFileInfo(p).isFile()) out << p; // timelines of the pool are no files
    }
    if (out.isEmpty() && mime->hasUrls())
        for (const QUrl& u : mime->urls())
            if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isFile()) out << u.toLocalFile();
    return out;
}

void DirectMaterial::dragEnterEvent(QDragEnterEvent* e)
{
    if (!filesOf(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

void DirectMaterial::dragMoveEvent(QDragMoveEvent* e)
{
    if (!filesOf(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

void DirectMaterial::dropEvent(QDropEvent* e)
{
    const QStringList files = filesOf(e->mimeData());
    if (files.isEmpty()) return;
    e->acceptProposedAction();
    addFiles(files);
}

bool DirectMaterial::eventFilter(QObject* obj, QEvent* e)
{
    // Delete removes from the list and never reaches the window (timeline clips)
    if (obj == m_list && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && InputBindings::matches(m_remove, static_cast<QKeyEvent*>(e))) {
        e->accept();
        if (e->type() == QEvent::KeyPress) m_remove->trigger();
        return e->type() == QEvent::KeyPress;
    }
    return QWidget::eventFilter(obj, e);
}

void DirectMaterial::rebuild()
{
    m_list->clear();
    if (m_timeline) {
        auto* item = new QListWidgetItem(T("Aktuelle Timeline"), m_list);
        item->setData(Qt::UserRole, "@current");
        if (const QJsonObject st = m_state.value("@current"); !st.isEmpty())
            item->setText(item->text() + "\n" + DirectText::clock(st.value("length_s").toDouble()) + " · "
                          + DirectText::analysis(st.value("analysis").toObject()));
    }
    for (const QString& f : m_files) {
        QString text = QFileInfo(f).fileName();
        if (const QJsonObject st = m_state.value(f); !st.isEmpty())
            text += "\n" + DirectText::clock(st.value("length_s").toDouble()) + " · "
                    + DirectText::analysis(st.value("analysis").toObject());
        auto* item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, f);
        item->setToolTip(QDir::toNativeSeparators(f));
        if (m_timeline) item->setFlags(item->flags() & ~Qt::ItemIsEnabled); // kept for later, not the material now
    }
    m_hint->setVisible(m_files.isEmpty() && !m_timeline);
    m_clear->setEnabled(!m_files.isEmpty() || m_timeline);
}
