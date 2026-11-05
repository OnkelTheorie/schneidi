// SPDX-License-Identifier: AGPL-3.0-only
// Inspector: section "Placeholder" of a template clip (ClipSlot, core/TemplateFill.h). Only shown when the clip is a
// placeholder (Timeline › Clip › Mark as Placeholder…); every change is one Editor::setClipSlot (one undo step).
#include "ui/Inspector.h"

#include "core/TemplateFill.h"
#include "ui/SlotText.h"

#include "ui/InspectorDetail.h"

using namespace InspectorDetail;

void Inspector::buildSlotSection(QVBoxLayout* page, TrackKind kind)
{
    Section s = addSection(page, kind, T("Platzhalter"), {});
    s.body->setObjectName(kind == TrackKind::Video ? "SlotSectionVideo" : "SlotSectionAudio");
    const QString prefix = kind == TrackKind::Video ? "slotV_" : "slotA_";

    // The clip whose placeholder is shown (the first selected one of this kind) and its track kind
    auto shown = [this, kind]() -> const Clip* {
        const Clip* c = primary(kind);
        return c && c->slot.isSet() ? c : nullptr;
    };
    auto* error = new QLabel;
    error->setWordWrap(true);
    error->setStyleSheet(QString("color: %1;").arg(Theme::warning.name()));
    error->hide();
    // Change the shown clip's placeholder: one undo step
    auto change = [this, shown, error](const std::function<void(ClipSlot&)>& fn) {
        const Clip* c = shown();
        if (!c) return;
        ClipSlot slot = c->slot;
        fn(slot);
        const QString why = m_editor->setClipSlot(c->id, slot);
        error->setVisible(!why.isEmpty());
        error->setText(T("Nicht möglich: Die ID gehört schon zu einem Platzhalter anderer Art, oder die Spur ist gesperrt."));
        error->setToolTip(why);
    };

    // Remove the mark (trash in the header, like an effect)
    auto* trash = new QToolButton;
    trash->setObjectName(prefix + "remove");
    trash->setIcon(trashIcon());
    trash->setIconSize(QSize(14, 14));
    trash->setAutoRaise(true);
    trash->setFocusPolicy(Qt::NoFocus);
    trash->setToolTip(T("Platzhalter entfernen"));
    trash->setStyleSheet("QToolButton { border: none; background: transparent; }");
    connect(trash, &QToolButton::clicked, this, [this, shown] {
        if (const Clip* c = shown()) m_editor->setClipSlot(c->id, {});
    });
    s.headerLayout->addWidget(trash);

    struct Row { QLabel* label; QWidget* widget; };
    auto addRow = [this, &s](const QString& text, QWidget* w) {
        Row r{rowLabel(text), w};
        s.grid->addWidget(r.label, s.rows, 0);
        s.grid->addWidget(w, s.rows, 1, 1, 2);
        ++s.rows;
        return r;
    };
    auto lineEdit = [&](const char* name, const QString& tip, const std::function<void(ClipSlot&, const QString&)>& set) {
        auto* e = new QLineEdit;
        e->setObjectName(prefix + name);
        e->setToolTip(tip);
        connect(e, &QLineEdit::editingFinished, this, [e, change, set] {
            if (!e->isModified()) return;
            e->setModified(false);
            const QString text = e->text();
            change([&](ClipSlot& sl) { set(sl, text); });
        });
        return e;
    };

    auto* label = lineEdit("label", T("Angezeigter Name des Platzhalters"), [](ClipSlot& sl, const QString& t) { sl.label = t.trimmed(); });
    auto* id = lineEdit("id", T("Fester Schlüssel für das Befüllen per Liste (schneidi-cli template fill), z. B. „clip1“ oder „hook_text“"),
                        [](ClipSlot& sl, const QString& t) {
                            if (!t.trimmed().isEmpty()) sl.id = t.trimmed();
                        });
    auto* hint = lineEdit("hint", T("Was hier hineingehört, für Menschen und KI: „bester Kill“, „Produkt von vorne“"),
                          [](ClipSlot& sl, const QString& t) { sl.hint = t.trimmed(); });
    addRow(T("Name"), label);
    addRow(T("ID"), id);

    auto combo = [&](const char* name) {
        auto* b = new QComboBox;
        b->setObjectName(prefix + name);
        b->setFocusPolicy(Qt::NoFocus);
        b->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        return b;
    };
    QComboBox* kindBox = combo("kind");
    connect(kindBox, &QComboBox::activated, this, [kindBox, change](int i) {
        change([&](ClipSlot& sl) { sl.kind = SlotKind(kindBox->itemData(i).toInt()); });
    });
    addRow(T("Art"), kindBox);
    addRow(T("Hinweis"), hint);

    // Length (not for text: a title keeps its time)
    QComboBox* lengthBox = combo("length");
    lengthBox->addItem(T("Fest"), int(SlotLength::Fixed));
    lengthBox->addItem(T("Flexibel"), int(SlotLength::Flexible));
    lengthBox->setToolTip(T("Fest: der Platzhalter behält seine Länge. Flexibel: er nimmt die Länge des Inhalts "
                            "(zwischen Min. und Max.), alles danach rückt nach."));
    connect(lengthBox, &QComboBox::activated, this, [lengthBox, change](int i) {
        change([&](ClipSlot& sl) { sl.length = SlotLength(lengthBox->itemData(i).toInt()); });
    });
    const Row lengthRow = addRow(T("Länge"), lengthBox);
    auto secondsField = [&](const char* name, const std::function<int&(ClipSlot&)>& field) {
        auto* f = new ScrubField(0, 3600, 0.1, 1);
        f->setObjectName(prefix + name);
        f->setSuffix(T(" s"));
        f->setMinimumText("–");
        f->setToolTip(T("0 = keine Grenze"));
        f->onFinish = [this, f, change, field] {
            const double fps = std::max(1.0, m_editor->project()->format().rate.fps());
            const int frames = int(std::lround(f->value() * fps));
            change([&](ClipSlot& sl) { field(sl) = frames; });
        };
        return f;
    };
    ScrubField* minLen = secondsField("min", [](ClipSlot& sl) -> int& { return sl.minLen; });
    ScrubField* maxLen = secondsField("max", [](ClipSlot& sl) -> int& { return sl.maxLen; });
    const Row minRow = addRow(T("Min."), minLen);
    const Row maxRow = addRow(T("Max."), maxLen);

    // Clip: what a too short clip does, reframing
    QComboBox* fitBox = combo("fit");
    for (SlotFit f : {SlotFit::Trim, SlotFit::Speed, SlotFit::Freeze, SlotFit::Loop}) fitBox->addItem(SlotText::fitName(f), int(f));
    fitBox->setToolTip(T("Was ein zu kurzer Clip tut (zu lange werden gekürzt)"));
    connect(fitBox, &QComboBox::activated, this, [fitBox, change](int i) {
        change([&](ClipSlot& sl) { sl.fit = SlotFit(fitBox->itemData(i).toInt()); });
    });
    const Row fitRow = addRow(T("Zu kurz"), fitBox);
    auto check = [&](const char* name, const QString& text, const QString& tip, const std::function<bool&(ClipSlot&)>& field) {
        auto* b = new QCheckBox(text);
        b->setObjectName(prefix + name);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QCheckBox::clicked, this, [change, field](bool on) { change([&](ClipSlot& sl) { field(sl) = on; }); });
        return b;
    };
    QCheckBox* reframe = check("reframe", T("Andere Bildform anpassen"),
                               T("Ein Bild anderer Form als die Timeline wird auf den Ausschnitt gebracht (Reframe)"),
                               [](ClipSlot& sl) -> bool& { return sl.reframe; });
    const Row reframeRow = addRow({}, reframe);

    // Text: maximum characters, shrinking
    auto* maxChars = new ScrubField(0, 10000, 1, 0);
    maxChars->setObjectName(prefix + "maxChars");
    maxChars->setMinimumText("–");
    maxChars->setToolTip(T("0 = keine Grenze"));
    maxChars->onFinish = [maxChars, change] {
        const int n = int(std::lround(maxChars->value()));
        change([&](ClipSlot& sl) { sl.maxChars = n; });
    };
    const Row charsRow = addRow(T("Max. Zeichen"), maxChars);
    QCheckBox* shrink = check("shrink", T("Längeren Text verkleinern"),
                              T("Ein breiterer Text bekommt eine kleinere Schrift, damit er passt"),
                              [](ClipSlot& sl) -> bool& { return sl.shrink; });
    const Row shrinkRow = addRow({}, shrink);

    // Music: cut the song to the length on its phrases
    QCheckBox* fitMusic = check("fitMusic", T("Musik an Phrasen auf Länge schneiden"),
                                T("Der Song wird an seinen Phrasen auf die Länge des Platzhalters geschnitten (sonst gekürzt mit Ausblenden)"),
                                [](ClipSlot& sl) -> bool& { return sl.fitMusic; });
    const Row musicRow = addRow({}, fitMusic);
    s.grid->addWidget(error, s.rows++, 0, 1, 3);

    m_refreshers << [=, this] {
        const Clip* c = shown();
        s.header->setVisible(c);
        s.body->setVisible(c && !s.body->property("collapsed").toBool());
        if (!c) {
            error->hide();
            return;
        }
        const ClipSlot& sl = c->slot;
        TrackRef ref;
        TimelineOps::findClip(m_editor->project()->timeline(), c->id, &ref);
        const bool locked = m_editor->isTrackLocked(ref);
        s.body->setEnabled(!locked);
        trash->setEnabled(!locked);
        for (QLineEdit* e : {label, id, hint})
            if (!e->hasFocus()) e->setModified(false);
        if (!label->hasFocus()) label->setText(sl.label);
        if (!id->hasFocus()) id->setText(sl.id);
        if (!hint->hasFocus()) hint->setText(sl.hint);
        {
            const QSignalBlocker b(kindBox);
            kindBox->clear();
            // Only the kinds this clip can hold (TemplateFill::canHold)
            for (SlotKind k : {SlotKind::Clip, SlotKind::Logo, SlotKind::Text, SlotKind::Music})
                if (TemplateFill::canHold(*c, ref.kind, k).isEmpty() || k == sl.kind) kindBox->addItem(SlotText::kindName(k), int(k));
            kindBox->setCurrentIndex(kindBox->findData(int(sl.kind)));
        }
        const QSignalBlocker b1(lengthBox), b2(fitBox);
        lengthBox->setCurrentIndex(lengthBox->findData(int(sl.length)));
        fitBox->setCurrentIndex(fitBox->findData(int(sl.fit)));
        const double fps = std::max(1.0, m_editor->project()->format().rate.fps());
        minLen->setValue(sl.minLen / fps);
        maxLen->setValue(sl.maxLen / fps);
        maxChars->setValue(sl.maxChars);
        reframe->setChecked(sl.reframe);
        shrink->setChecked(sl.shrink);
        fitMusic->setChecked(sl.fitMusic);
        const bool text = sl.kind == SlotKind::Text;
        const bool flexible = !text && sl.length == SlotLength::Flexible;
        auto show = [](const Row& r, bool on) {
            r.label->setVisible(on);
            r.widget->setVisible(on);
        };
        show(lengthRow, !text);
        show(minRow, flexible);
        show(maxRow, flexible);
        show(fitRow, sl.kind == SlotKind::Clip);
        show(reframeRow, sl.kind == SlotKind::Clip);
        show(charsRow, text);
        show(shrinkRow, text);
        show(musicRow, sl.kind == SlotKind::Music);
    };
}
