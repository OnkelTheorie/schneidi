#include "app/DuckingDialog.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
// Number in the field: decimal point of the UI language; both '.' and ',' are read
QString shown(double v)
{
    QString s = QString::number(v, 'g', 6);
    return s.replace('.', QLocale().decimalPoint());
}

double parsed(const QLineEdit* e, double fallback, double lo, double hi)
{
    bool ok = false;
    const double x = QString(e->text()).trimmed().replace(',', '.').toDouble(&ok);
    return ok ? std::clamp(x, lo, hi) : fallback;
}
} // namespace

DuckingDialog::DuckingDialog(Editor* editor, const QVector<int>& musicIds, QWidget* parent)
    : QDialog(parent), m_editor(editor)
{
    setWindowTitle(T("Ducking"));
    const Project* p = editor->project();
    const Timeline& tl = p->timeline();
    m_fps = p->format().rate.fps();
    QVector<int> musicTracks;
    for (int id : musicIds) {
        TrackRef ref;
        if (TimelineOps::findClip(tl, id, &ref) && ref.kind == TrackKind::Audio && !TimelineOps::isLocked(tl, id)
            && !m_music.contains(id)) {
            m_music << id;
            if (!musicTracks.contains(ref.index)) musicTracks << ref.index;
        }
    }

    QSettings settings;
    const QStringList remembered = settings.value("ducking/under").toStringList();
    auto* root = new QVBoxLayout(this);
    auto* intro = new QLabel(m_music.isEmpty()
                                 ? T("Keine Musik ausgewählt: zuerst die Audioclips der Musik in der Timeline auswählen.")
                                 : T("Die ausgewählte Musik (%1 Clips) wird leiser, solange auf diesen Spuren Ton ist "
                                     "(Lautstärke-Keyframes, danach in der Lautstärkelinie änderbar):")
                                       .arg(m_music.size()));
    intro->setWordWrap(true);
    root->addWidget(intro);
    auto* box = new QGroupBox(T("Absenken bei Ton auf"));
    auto* boxLayout = new QVBoxLayout(box);
    for (int i = 0; i < tl.audio.size(); ++i) {
        if (musicTracks.contains(i)) continue;
        const TrackRef ref{TrackKind::Audio, i};
        const QString name = trackDisplayName(tl.audio[i], ref);
        auto* check = new QCheckBox(QString("%1 – %2").arg(trackShortName(ref), name));
        check->setObjectName("under_" + trackShortName(ref));
        check->setChecked(!m_music.isEmpty() && remembered.contains(name));
        check->setEnabled(!m_music.isEmpty());
        boxLayout->addWidget(check);
        m_tracks << qMakePair(i, check);
        connect(check, &QCheckBox::toggled, this, &DuckingDialog::updateOk);
    }
    if (m_tracks.isEmpty())
        boxLayout->addWidget(new QLabel(T("Keine anderen Audiospuren (die Musik liegt auf allen).")));
    root->addWidget(box);

    const Ducking::Params def = Ducking::defaults(m_fps);
    auto* form = new QFormLayout;
    auto field = [&](const char* name, const QString& label, const QString& unit, double value, const QString& tip) {
        auto* e = new QLineEdit(shown(value));
        e->setObjectName(name);
        e->setToolTip(tip);
        e->setMaximumWidth(90);
        auto* row = new QHBoxLayout;
        row->addWidget(e);
        row->addWidget(new QLabel(unit));
        row->addStretch(1);
        form->addRow(label, row);
        return e;
    };
    m_amount = field("amount", T("Absenken um"), "dB", settings.value("ducking/amount", def.amountDb).toDouble(),
                     T("Wie weit die Musik leiser wird"));
    m_threshold = field("threshold", T("Schwelle"), "dBFS", settings.value("ducking/threshold", def.thresholdDb).toDouble(),
                        T("Ab diesem Pegel zählt es auf den Spuren als Ton"));
    m_attack = field("attack", T("Attack"), "s", settings.value("ducking/attack", 0.15).toDouble(),
                     T("Rampe nach unten vor dem Ton"));
    m_release = field("release", T("Release"), "s", settings.value("ducking/release", 0.4).toDouble(),
                      T("Rampe nach oben nach dem Ton"));
    m_hold = field("hold", T("Halten"), "s", settings.value("ducking/hold", 0.5).toDouble(),
                   T("Kürzere Pausen bleiben abgesenkt"));
    root->addLayout(form);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(m_buttons);
    updateOk();
}

QVector<int> DuckingDialog::underTracks() const
{
    QVector<int> out;
    for (const auto& [index, check] : m_tracks)
        if (check->isChecked()) out << index;
    return out;
}

Ducking::Params DuckingDialog::params() const
{
    const Ducking::Params def = Ducking::defaults(m_fps);
    Ducking::Params p;
    p.amountDb = parsed(m_amount, def.amountDb, -60, 0);
    p.thresholdDb = parsed(m_threshold, def.thresholdDb, -80, 0);
    const auto frames = [&](const QLineEdit* e, int fallback) {
        const double s = parsed(e, -1, 0, 60);
        return s < 0 ? fallback : int(std::lround(s * m_fps));
    };
    p.attack = frames(m_attack, def.attack);
    p.release = frames(m_release, def.release);
    p.hold = frames(m_hold, def.hold);
    return p;
}

void DuckingDialog::updateOk()
{
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!m_music.isEmpty() && !underTracks().isEmpty());
}

void DuckingDialog::accept()
{
    if (m_music.isEmpty() || underTracks().isEmpty()) return;
    QSettings settings;
    const Timeline& tl = m_editor->project()->timeline();
    QStringList names;
    for (int i : underTracks()) names << trackDisplayName(tl.audio[i], {TrackKind::Audio, i});
    settings.setValue("ducking/under", names);
    const Ducking::Params p = params();
    settings.setValue("ducking/amount", p.amountDb);
    settings.setValue("ducking/threshold", p.thresholdDb);
    settings.setValue("ducking/attack", parsed(m_attack, 0.15, 0, 60));
    settings.setValue("ducking/release", parsed(m_release, 0.4, 0, 60));
    settings.setValue("ducking/hold", parsed(m_hold, 0.5, 0, 60));
    QDialog::accept();
}
