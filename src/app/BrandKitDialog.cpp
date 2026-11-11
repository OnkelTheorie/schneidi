// SPDX-License-Identifier: AGPL-3.0-only
#include "app/BrandKitDialog.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

struct CornerName { int corner; const char* name; };
const CornerName kCornerNames[] = {{1, N_("Oben rechts")}, {2, N_("Oben links")}, {3, N_("Unten rechts")},
                                   {4, N_("Unten links")}, {0, N_("Mitte")}};

// A font this computer has: an installed family or a generic name the system resolves ("Serif", "Monospace")
bool installed(const QString& family)
{
    static const QStringList generic{"serif", "sans", "sans-serif", "sans serif", "monospace", "mono", "cursive",
                                     "fantasy", "system-ui"};
    return QFontDatabase::hasFamily(family) || generic.contains(family.toLower());
}

} // namespace

BrandKitDialog::BrandKitDialog(const BrandKit& global, const std::optional<BrandKit>& project, QWidget* parent)
    : QDialog(parent), m_global(global.clamped()), m_project(project ? project->clamped() : global.clamped()),
      m_projectOnly(project.has_value()), m_hadOwn(project.has_value())
{
    setWindowTitle(T("Branding"));
    auto* layout = new QVBoxLayout(this);

    // Which kit: the global one (all projects) or this project's own
    auto* scope = new QHBoxLayout;
    m_globalButton = new QRadioButton(T("Global (alle Projekte)"));
    m_projectButton = new QRadioButton(T("Nur dieses Projekt"));
    auto* group = new QButtonGroup(this);
    group->addButton(m_globalButton);
    group->addButton(m_projectButton);
    scope->addWidget(m_globalButton);
    scope->addWidget(m_projectButton);
    scope->addStretch(1);
    layout->addLayout(scope);
    m_scopeHint = new QLabel;
    m_scopeHint->setWordWrap(true);
    m_scopeHint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    layout->addWidget(m_scopeHint);
    connect(m_globalButton, &QRadioButton::toggled, this, [this](bool on) {
        if (!m_loading) setProjectOnly(!on);
    });

    auto fileRow = [this](QGridLayout* grid, int row, const QString& which, const QString& label) {
        grid->addWidget(new QLabel(label), row, 0);
        auto* name = new QLabel;
        name->setMinimumWidth(220);
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_files[which] = name;
        auto* choose = new QPushButton(T("Wählen…"));
        auto* clear = new QToolButton;
        clear->setText(QStringLiteral("×"));
        clear->setToolTip(T("Entfernen"));
        connect(choose, &QPushButton::clicked, this, [this, which] { pickFile(which); });
        connect(clear, &QToolButton::clicked, this, [this, which] { setFile(which, {}); });
        grid->addWidget(name, row, 1);
        grid->addWidget(choose, row, 2);
        grid->addWidget(clear, row, 3);
    };

    // Logo with its place
    auto* logoBox = new QGroupBox(T("Logo"));
    auto* logo = new QGridLayout(logoBox);
    fileRow(logo, 0, "logo", T("Datei"));
    logo->addWidget(new QLabel(T("Position")), 1, 0);
    m_corner = new QComboBox;
    for (const CornerName& c : kCornerNames) m_corner->addItem(T(c.name), c.corner);
    logo->addWidget(m_corner, 1, 1);
    logo->addWidget(new QLabel(T("Breite")), 2, 0);
    m_size = new QSpinBox;
    m_size->setRange(1, 100);
    m_size->setSuffix(QStringLiteral(" %"));
    m_size->setToolTip(T("Breite des Logos im Verhältnis zur Bildbreite"));
    logo->addWidget(m_size, 2, 1);
    connect(m_corner, &QComboBox::currentIndexChanged, this, [this] { store(); });
    connect(m_size, &QSpinBox::valueChanged, this, [this] { store(); });
    layout->addWidget(logoBox);

    // Colours: brand colour 1..4 (click = change; the last one can be removed so the numbers never shift)
    auto* colorBox = new QGroupBox(T("Farben"));
    auto* colors = new QHBoxLayout(colorBox);
    for (int i = 0; i < BrandKit::kMaxColors; ++i) {
        auto* b = new QToolButton;
        b->setFixedSize(48, 28);
        connect(b, &QToolButton::clicked, this, [this, i] { pickColor(i); });
        colors->addWidget(b);
        m_colors << b;
    }
    auto* removeColor = new QToolButton;
    removeColor->setText(QStringLiteral("−"));
    removeColor->setToolTip(T("Letzte Farbe entfernen"));
    connect(removeColor, &QToolButton::clicked, this, [this] {
        if (!kit().colors.isEmpty()) setColor(int(kit().colors.size()) - 1, {});
    });
    colors->addWidget(removeColor);
    colors->addStretch(1);
    layout->addWidget(colorBox);

    // Fonts: heading and body
    auto* fontBox = new QGroupBox(T("Schriften"));
    auto* fonts = new QGridLayout(fontBox);
    const QString fontNames[] = {T("Schrift 1 (Überschrift)"), T("Schrift 2 (Fließtext)")};
    for (int i = 0; i < BrandKit::kMaxFonts; ++i) {
        auto* on = new QCheckBox(fontNames[i]);
        auto* box = new QFontComboBox;
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(14);
        connect(on, &QCheckBox::toggled, this, [this, i](bool checked) {
            // Switched off: the boxes from here on stand for no font any more (on again = the font each box shows)
            if (!checked && !m_loading)
                for (int j = i; j < m_shownFonts.size(); ++j) m_shownFonts[j].clear();
            store();
        });
        // Only a font the user picks replaces the kit's (a missing one stays as it is, see load())
        connect(box, &QFontComboBox::currentFontChanged, this, [this, i](const QFont& f) {
            if (m_loading) return;
            m_shownFonts[i] = f.family();
            store();
        });
        fonts->addWidget(on, i, 0);
        fonts->addWidget(box, i, 1);
        m_fontOn << on;
        m_fonts << box;
        m_shownFonts << QString();
    }
    layout->addWidget(fontBox);

    // Intro / outro clips
    auto* clipBox = new QGroupBox(T("Intro und Outro"));
    auto* clips = new QGridLayout(clipBox);
    fileRow(clips, 0, "intro", T("Intro"));
    fileRow(clips, 1, "outro", T("Outro"));
    layout->addWidget(clipBox);

    auto* note = new QLabel(T("Titel, deren Farbe oder Schrift auf das Branding verweist (Inspektor), übernehmen "
                              "Änderungen. Logo und Intro/Outro fügt Arbeitsbereich › Branding in die Timeline ein."));
    note->setWordWrap(true);
    note->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    layout->addWidget(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    load();
}

void BrandKitDialog::setProjectOnly(bool on)
{
    // The project's own starts as a copy of the global kit (one it had before comes back as it was)
    if (on && !m_projectOnly && !m_hadOwn) m_project = m_global;
    m_projectOnly = on;
    load();
}

void BrandKitDialog::setColor(int index, const QColor& c)
{
    QVector<QColor>& colors = kit().colors;
    if (index < 0 || index >= BrandKit::kMaxColors) return;
    if (!c.isValid()) {
        if (index < colors.size()) colors.resize(index); // removing one removes the ones after it too
    } else if (index < colors.size()) {
        colors[index] = c;
    } else {
        colors << c; // the next free one
    }
    load();
}

void BrandKitDialog::setFont(int index, const QString& family)
{
    QStringList& fonts = kit().fonts;
    if (index < 0 || index >= BrandKit::kMaxFonts) return;
    if (family.isEmpty()) {
        while (fonts.size() > index) fonts.removeLast();
    } else if (index < fonts.size()) {
        fonts[index] = family;
    } else {
        fonts << family;
    }
    load();
}

void BrandKitDialog::setFile(const QString& which, const QString& path)
{
    if (which == "logo") kit().logo = path;
    if (which == "intro") kit().intro = path;
    if (which == "outro") kit().outro = path;
    load();
}

void BrandKitDialog::pickColor(int index)
{
    const QColor cur = index < kit().colors.size() ? kit().colors[index] : QColor(Qt::white);
    const QColor c = QColorDialog::getColor(cur, this, T("Brand-Farbe %1").arg(index + 1));
    if (c.isValid()) setColor(index, c);
}

void BrandKitDialog::pickFile(const QString& which)
{
    const QString filter = which == "logo" ? T("Bilder und Videos (*.png *.jpg *.jpeg *.webp *.svg *.mp4 *.mov *.webm *.mkv)")
                                           : T("Videos (*.mp4 *.mov *.webm *.mkv *.avi)");
    QString cur = which == "logo" ? kit().logo : which == "intro" ? kit().intro : kit().outro;
    const QString path = QFileDialog::getOpenFileName(this, T("Datei wählen"), QFileInfo(cur).absolutePath(), filter);
    if (!path.isEmpty()) setFile(which, path);
}

void BrandKitDialog::load()
{
    m_loading = true;
    const BrandKit& k = kit();
    m_globalButton->setChecked(!m_projectOnly);
    m_projectButton->setChecked(m_projectOnly);
    m_scopeHint->setText(m_projectOnly ? T("Dieses Projekt hat ein eigenes Branding. „Global“ verwendet wieder das "
                                           "globale.")
                                       : T("Gilt für alle Projekte ohne eigenes Branding."));
    for (const auto& [which, path] : {std::pair{QString("logo"), k.logo}, std::pair{QString("intro"), k.intro},
                                      std::pair{QString("outro"), k.outro}}) {
        QLabel* l = m_files.value(which);
        l->setText(path.isEmpty() ? T("(keine)") : QFileInfo(path).fileName());
        l->setToolTip(path);
    }
    m_corner->setCurrentIndex(std::max(0, m_corner->findData(k.logoCorner)));
    m_size->setValue(int(std::lround(k.logoSize * 100)));
    for (int i = 0; i < m_colors.size(); ++i) {
        QToolButton* b = m_colors[i];
        const bool set = i < k.colors.size();
        b->setVisible(i <= k.colors.size()); // the set ones and one to add
        b->setText(set ? QString() : QStringLiteral("+"));
        b->setToolTip(set ? T("Brand-Farbe %1").arg(i + 1) : T("Farbe hinzufügen"));
        b->setStyleSheet(set ? QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 3px; }")
                                   .arg(k.colors[i].name(), Theme::border.name())
                             : QString());
    }
    for (int i = 0; i < m_fonts.size(); ++i) {
        const bool set = i < k.fonts.size();
        m_fontOn[i]->setChecked(set);
        m_fontOn[i]->setEnabled(i <= k.fonts.size());
        m_fonts[i]->setEnabled(set);
        if (!set) continue;
        // The kit's font as it is: one not installed here (a kit from another computer, the CLI) shows its name and that
        // it is missing instead of the font Qt would fall back to, and is kept unless the user picks another one
        const QString family = k.fonts[i];
        m_shownFonts[i] = family;
        if (installed(family)) {
            m_fonts[i]->setCurrentFont(QFont(family));
            m_fonts[i]->setToolTip({});
        } else {
            m_fonts[i]->setCurrentIndex(-1);
            m_fonts[i]->setEditText(T("%1 (nicht installiert)").arg(family));
            m_fonts[i]->setToolTip(T("Diese Schrift ist auf diesem Rechner nicht installiert. Sie bleibt im Branding, "
                                     "bis du eine andere wählst."));
        }
    }
    m_loading = false;
}

void BrandKitDialog::store()
{
    if (m_loading) return;
    BrandKit& k = kit();
    k.logoCorner = m_corner->currentData().toInt();
    // The box shows whole percents: the kit's width (e.g. 0.125) stays unless the user changed the box
    if (m_size->value() != int(std::lround(k.logoSize * 100))) k.logoSize = m_size->value() / 100.0;
    QStringList fonts;
    for (int i = 0; i < m_fonts.size(); ++i) {
        if (!m_fontOn[i]->isChecked()) break;
        // The font shown in the box (the kit's own name, or the one the user picked); a box switched on anew takes its font
        const QString shown = m_shownFonts.value(i);
        fonts << (shown.isEmpty() ? m_fonts[i]->currentFont().family() : shown);
    }
    k.fonts = fonts;
    load();
}
