#include "app/DesignDialog.h"

#include "core/I18n.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

// Verkleinerte Oberfläche in den gewählten Farben: Panel mit Auswahl, Viewer mit Timecode und Regler,
// Timeline mit Clips, Keyframes, In/Out, Marker und Playhead, daneben ein Pegel
class DesignPreview : public QWidget {
public:
    DesignPreview() { setFixedSize(520, 250); }
    void setColors(const Theme::Colors& c)
    {
        m_c = c;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        auto c = [this](const char* key) { return m_c.value(key); };
        const QColor primary = c("primary");
        QPainter p(this);
        QFont f = font();
        f.setPointSizeF(7);
        p.setFont(f);
        const int W = width(), H = height();
        p.fillRect(rect(), c("window"));

        // obere Leiste mit Panel-Schaltern
        p.fillRect(0, 0, W, 16, c("topBar"));
        p.fillRect(0, 16, W, 1, c("border"));
        p.setPen(primary);
        p.drawText(QRect(6, 0, 70, 16), Qt::AlignVCenter, T("Media Pool"));
        p.setPen(c("text"));
        p.drawText(QRect(82, 0, 60, 16), Qt::AlignVCenter, T("Effekte"));

        // Panel links: Titel, Liste mit Auswahl
        const QRect panel(0, 17, 118, 110);
        p.fillRect(panel, c("panel"));
        p.fillRect(panel.left(), panel.top(), panel.width(), 15, c("panelHeader"));
        p.setPen(c("text"));
        p.drawText(QRect(6, panel.top(), 100, 15), Qt::AlignVCenter, T("Media Pool"));
        const char* items[] = {"Interview.mp4", "Drohne.mov", "Musik.wav"};
        for (int i = 0; i < 3; ++i) {
            const QRect r(4, panel.top() + 20 + i * 18, panel.width() - 8, 16);
            if (i == 1) {
                p.fillRect(r, c("control"));
                p.setPen(primary);
                p.drawRect(r.adjusted(0, 0, -1, -1));
            }
            p.fillRect(r.left() + 3, r.top() + 3, 16, 10, c("thumbBg"));
            p.setPen(i == 1 ? c("text") : c("textDim"));
            p.drawText(r.adjusted(24, 0, 0, 0), Qt::AlignVCenter, items[i]);
        }

        // Viewer mit Timecode (eingelassenes Feld) und Regler
        const QRect viewer(122, 17, W - 122, 88);
        p.fillRect(viewer, c("viewerBg"));
        const QRect img(viewer.center().x() - 70, viewer.top() + 6, 140, viewer.height() - 12);
        QLinearGradient sky(img.topLeft(), img.bottomLeft());
        sky.setColorAt(0, QColor(0x6f, 0x9c, 0xc8));
        sky.setColorAt(1, QColor(0x3d, 0x5a, 0x3a));
        p.fillRect(img, sky);
        const QRect transport(122, viewer.bottom() + 1, W - 122, 22);
        p.fillRect(transport, c("panel"));
        const QRect tc(transport.left() + 6, transport.top() + 4, 72, 14);
        p.fillRect(tc, c("well"));
        p.setPen(c("text"));
        p.drawText(tc, Qt::AlignCenter, "01:00:12:08");
        const QRect groove(tc.right() + 14, transport.center().y() - 1, transport.right() - tc.right() - 28, 3);
        p.fillRect(groove, c("well"));
        p.fillRect(groove.left(), groove.top(), groove.width() * 2 / 5, groove.height(), primary);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(c("border"));
        p.setBrush(c("text"));
        p.drawEllipse(QPointF(groove.left() + groove.width() * 2 / 5.0, groove.center().y() + 0.5), 4, 4);
        p.setRenderHint(QPainter::Antialiasing, false);

        // Timeline
        const int tlTop = 131, headerW = 58, meterW = 44;
        const QRect tl(0, tlTop, W - meterW - 4, H - tlTop);
        p.fillRect(tl, c("timelineBg"));
        const QRect ruler(headerW, tlTop, tl.width() - headerW, 16);
        p.fillRect(ruler, c("ruler"));
        p.fillRect(0, tlTop, headerW, 16, c("panelHeader"));
        p.setPen(c("text"));
        p.drawText(QRect(4, tlTop, headerW, 16), Qt::AlignVCenter, "01:00:12");
        auto xOf = [&](double t) { return int(ruler.left() + t * ruler.width()); };
        for (int i = 0; i <= 20; ++i) {
            p.setPen(i % 5 ? c("textFaint") : c("textDim"));
            p.drawLine(xOf(i / 20.0), ruler.bottom() - (i % 5 ? 3 : 7), xOf(i / 20.0), ruler.bottom());
        }
        const QColor secondary = c("secondary");
        p.fillRect(QRect(xOf(0.55), ruler.top() + 6, xOf(0.8) - xOf(0.55), 10), Theme::alpha(secondary, 45));
        p.fillRect(xOf(0.55), ruler.top() + 6, 2, 10, secondary);
        p.fillRect(xOf(0.8) - 2, ruler.top() + 6, 2, 10, secondary);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath flag;
        const double mx = xOf(0.3);
        flag.moveTo(mx - 4, ruler.top() + 3);
        flag.lineTo(mx + 4, ruler.top() + 3);
        flag.lineTo(mx + 4, ruler.top() + 8);
        flag.lineTo(mx, ruler.top() + 12);
        flag.lineTo(mx - 4, ruler.top() + 8);
        flag.closeSubpath();
        p.fillPath(flag, secondary);
        p.setRenderHint(QPainter::Antialiasing, false);

        struct Row {
            const char* name;
            int h;
            bool lane;
        };
        const Row rows[] = {{"V2", 22, false}, {"V1", 26, true}, {"A1", 26, false}};
        int y = ruler.bottom() + 1;
        for (int i = 0; i < 3; ++i) {
            const int h = rows[i].h + (rows[i].lane ? 12 : 0);
            p.fillRect(QRect(headerW, y, tl.width() - headerW, h), i % 2 ? c("trackBgAlt") : c("trackBg"));
            p.fillRect(QRect(0, y, headerW - 1, h - 1), c("trackHeader"));
            p.fillRect(QRect(0, y, 3, h - 1), i < 2 ? c("videoClip") : c("audioClip"));
            const QRect box(8, y + 5, 20, 12);
            p.setPen(i == 1 ? primary : c("textFaint"));
            p.setBrush(Qt::NoBrush);
            p.drawRect(box.adjusted(0, 0, -1, -1));
            p.setPen(c("text"));
            p.drawText(box, Qt::AlignCenter, rows[i].name);
            p.fillRect(QRect(headerW, y + h - 1, tl.width() - headerW, 1), c("border"));
            auto clip = [&](double a, double b, const QColor& col, bool selected, const QString& name) {
                const QRect r(xOf(a), y + 2, xOf(b) - xOf(a) - 1, rows[i].h - 5);
                p.fillRect(r, col.darker(135));
                p.fillRect(r.left(), r.top(), r.width(), 9, col);
                p.setPen(QColor(0xf0, 0xf0, 0xf0));
                p.drawText(r.adjusted(3, -1, 0, 0), Qt::AlignTop, name);
                p.setPen(selected ? QPen(Theme::selectionFrom(primary), 2) : QPen(QColor(0, 0, 0, 120), 1));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(0, 0, -1, -1));
            };
            if (i == 0) {
                clip(0.18, 0.46, c("titleClip"), false, T("Titel"));
                clip(0.62, 0.9, c("compoundClip"), false, "Compound");
            } else if (i == 1) {
                clip(0.0, 0.4, c("videoClip"), false, "Interview.mp4");
                clip(0.4, 0.78, c("videoClip"), true, "Drohne.mov");
                clip(0.78, 1.0, c("controlOff"), false, T("aus"));
                // Keyframe-Spur unter dem ausgewählten Clip
                const QRect lane(xOf(0.4), y + rows[i].h - 2, xOf(0.78) - xOf(0.4) - 1, 11);
                p.fillRect(lane, c("lane"));
                p.setRenderHint(QPainter::Antialiasing);
                for (double k : {0.46, 0.58, 0.7}) {
                    const QPointF pt(xOf(k), lane.center().y() + 0.5);
                    QPolygonF d;
                    d << pt + QPointF(0, -4) << pt + QPointF(4, 0) << pt + QPointF(0, 4) << pt + QPointF(-4, 0);
                    p.setPen(QPen(QColor(0, 0, 0, 180), 1));
                    p.setBrush(k == 0.58 ? primary : secondary);
                    p.drawPolygon(d);
                }
                p.setRenderHint(QPainter::Antialiasing, false);
            } else {
                clip(0.0, 0.78, c("audioClip"), false, "Interview.mp4");
                clip(0.78, 1.0, c("audioClip"), false, "Musik.wav");
            }
            y += h;
        }
        // Untertitel-Streifen
        const QRect sub(xOf(0.1), y + 3, xOf(0.35) - xOf(0.1), 11);
        p.fillRect(sub, c("subtitleClip"));
        p.setPen(QColor(0xf0, 0xf0, 0xf0));
        p.drawText(sub.adjusted(3, 0, 0, 0), Qt::AlignVCenter, T("Untertitel"));
        const QRect cache(xOf(0.0), ruler.top(), xOf(0.5) - xOf(0.0), 2);
        p.fillRect(cache, c("cacheReady"));
        p.fillRect(QRect(cache.right() + 1, ruler.top(), xOf(0.62) - cache.right(), 2), c("cacheMissing"));
        // Playhead
        const int px = xOf(0.58);
        p.fillRect(px, ruler.top() + 4, 1, H - ruler.top() - 4, c("playhead"));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(c("playhead"));
        p.drawPolygon(QPolygonF({QPointF(px - 4.5, ruler.top() + 4), QPointF(px + 5.5, ruler.top() + 4),
                                 QPointF(px + 0.5, ruler.top() + 10)}));
        p.setRenderHint(QPainter::Antialiasing, false);

        // Pegel rechts (wie der Mixer)
        const QRect strip(W - meterW, tlTop, meterW, H - tlTop);
        p.fillRect(strip, c("panel"));
        const QRect mute(strip.left() + 4, strip.top() + 4, 16, 12), solo(mute.right() + 4, mute.top(), 16, 12);
        p.fillRect(mute, c("warning"));
        p.setPen(Theme::readableOn(c("warning")));
        p.drawText(mute, Qt::AlignCenter, "M");
        p.fillRect(solo, c("control"));
        p.setPen(c("text"));
        p.drawText(solo, Qt::AlignCenter, "S");
        const int top = mute.bottom() + 6, bottom = H - 6;
        for (int ch = 0; ch < 2; ++ch) {
            const QRect bar(strip.left() + 10 + ch * 8, top, 6, bottom - top);
            p.fillRect(bar, c("well"));
            const int level = ch ? 78 : 92; // Prozent
            const int yTop = bottom - (bottom - top) * level / 100;
            const int yY = bottom - (bottom - top) * 60 / 100, yR = bottom - (bottom - top) * 85 / 100;
            p.fillRect(bar.left(), yY, 6, bottom - yY, c("meterLow"));
            p.fillRect(bar.left(), std::max(yTop, yR), 6, yY - std::max(yTop, yR), c("meterMid"));
            if (yTop < yR) p.fillRect(bar.left(), yTop, 6, yR - yTop, c("meterHigh"));
            if (ch == 0) p.fillRect(bar.left(), top - 4, 6, 3, c("warning"));
        }
        p.setPen(c("border"));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

private:
    Theme::Colors m_c;
};

namespace {

QString swatchCss(const QColor& c)
{
    return QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
        .arg(c.name(), Theme::border.name());
}

} // namespace

DesignDialog::DesignDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Design"));
    m_design = Theme::savedDesign();
    const QList<Theme::Design> designs = Theme::designs();
    for (const Theme::Design& d : designs) {
        const Theme::Colors s = Theme::saved(d.id);
        m_custom.insert(d.id, {s.value("primary"), s.value("secondary")});
    }
    const Theme::Colors current = Theme::saved(m_design);
    for (const QString& key : Theme::signalKeys()) m_signals.insert(key, current.value(key));

    auto* lay = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(T("Design")));
    m_designs = new QComboBox;
    for (const Theme::Design& d : designs) m_designs->addItem(d.name, d.id);
    m_designs->setCurrentIndex(std::max(0, m_designs->findData(m_design)));
    top->addWidget(m_designs);
    m_description = new QLabel;
    m_description->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    top->addWidget(m_description, 1);
    lay->addLayout(top);
    connect(m_designs, QOverload<int>::of(&QComboBox::activated), this,
            [this](int i) { selectDesign(m_designs->itemData(i).toString()); });

    m_preview = new DesignPreview;
    lay->addWidget(m_preview, 0, Qt::AlignHCenter);

    auto swatch = [this](const QString& key, const QString& tip) {
        auto* b = new QToolButton;
        b->setFixedSize(36, 18);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::TabFocus);
        connect(b, &QToolButton::clicked, this, [this, key] { pick(key); });
        m_swatches.insert(key, b);
        return b;
    };
    auto resetButton = [this](const QString& key, const QString& text) {
        auto* b = new QToolButton;
        b->setText(text);
        m_resets.insert(key, b);
        return b;
    };

    auto* own = new QGroupBox(T("Farben des Designs"));
    auto* ownGrid = new QGridLayout(own);
    const struct { const char* key; const char* label; const char* tip; } designKeys[] = {
        {"primary", N_("Primärfarbe"), N_("Aktive Schalter, Auswahl, Regler-Füllung, Fortschritt")},
        {"secondary", N_("Sekundärfarbe"), N_("Keyframes, Marker, In/Out, Hervorhebung beim Ziehen")},
    };
    int row = 0;
    for (const auto& k : designKeys) {
        ownGrid->addWidget(new QLabel(T(k.label)), row, 0);
        ownGrid->addWidget(swatch(k.key, T(k.tip)), row, 1);
        auto* hint = new QLabel(T(k.tip));
        hint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
        ownGrid->addWidget(hint, row, 2);
        QToolButton* reset = resetButton(k.key, T("Zurücksetzen"));
        reset->setToolTip(T("Vorgabe des Designs"));
        connect(reset, &QToolButton::clicked, this, [this, key = QString(k.key)] { resetDesignColor(key); });
        ownGrid->addWidget(reset, row, 3);
        ++row;
    }
    ownGrid->setColumnStretch(2, 1);
    lay->addWidget(own);

    auto* sig = new QGroupBox(T("Signalfarben (gleich in allen Designs)"));
    auto* sigGrid = new QGridLayout(sig);
    const QStringList keys = Theme::signalKeys();
    const int perColumn = (int(keys.size()) + 1) / 2;
    for (int i = 0; i < keys.size(); ++i) {
        const int r = i % perColumn, col = i / perColumn * 3;
        sigGrid->addWidget(new QLabel(Theme::signalLabel(keys[i])), r, col);
        sigGrid->addWidget(swatch(keys[i], Theme::signalLabel(keys[i])), r, col + 1);
    }
    sigGrid->setColumnMinimumWidth(2, 24);
    sigGrid->setColumnStretch(2, 1);
    sigGrid->setColumnStretch(5, 1);
    QToolButton* sigReset = resetButton("signals", T("Signalfarben zurücksetzen"));
    connect(sigReset, &QToolButton::clicked, this, &DesignDialog::resetSignals);
    sigGrid->addWidget(sigReset, perColumn, 0, 1, 6, Qt::AlignRight);
    lay->addWidget(sig);

    auto* note = new QLabel(T("Das Design ändert sich nach dem Neustart von schneidi."));
    note->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &DesignDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &DesignDialog::reject);
    lay->addWidget(buttons);

    refresh();
}

Theme::Colors DesignDialog::colors() const
{
    Theme::Colors c = Theme::defaults(m_design);
    const auto custom = m_custom.value(m_design);
    if (custom.first.isValid()) c.insert("primary", custom.first);
    if (custom.second.isValid()) c.insert("secondary", custom.second);
    for (auto it = m_signals.begin(); it != m_signals.end(); ++it) c.insert(it.key(), it.value());
    return c;
}

void DesignDialog::selectDesign(const QString& id)
{
    if (m_designs->findData(id) < 0) return;
    m_design = id;
    m_designs->setCurrentIndex(m_designs->findData(id));
    refresh();
}

void DesignDialog::setColor(const QString& key, const QColor& c)
{
    if (!c.isValid()) return;
    if (key == "primary") m_custom[m_design].first = c;
    else if (key == "secondary") m_custom[m_design].second = c;
    else if (m_signals.contains(key)) m_signals.insert(key, c);
    refresh();
}

void DesignDialog::resetDesignColor(const QString& key)
{
    const Theme::Colors def = Theme::defaults(m_design);
    setColor(key, def.value(key));
}

void DesignDialog::resetSignals()
{
    const Theme::Colors def = Theme::defaults(m_design);
    for (const QString& key : Theme::signalKeys()) m_signals.insert(key, def.value(key));
    refresh();
}

void DesignDialog::pick(const QString& key)
{
    const QString title = key == "primary" ? T("Primärfarbe") : key == "secondary" ? T("Sekundärfarbe")
                                                                                  : Theme::signalLabel(key);
    const QColor c = QColorDialog::getColor(colors().value(key), this, title);
    if (c.isValid()) setColor(key, c);
}

void DesignDialog::refresh()
{
    const Theme::Colors c = colors(), def = Theme::defaults(m_design);
    for (auto it = m_swatches.begin(); it != m_swatches.end(); ++it) it.value()->setStyleSheet(swatchCss(c.value(it.key())));
    for (const char* key : {"primary", "secondary"}) m_resets.value(key)->setEnabled(c.value(key) != def.value(key));
    const QStringList keys = Theme::signalKeys();
    m_resets.value("signals")->setEnabled(
        std::any_of(keys.begin(), keys.end(), [&](const QString& k) { return c.value(k) != def.value(k); }));
    for (const Theme::Design& d : Theme::designs())
        if (d.id == m_design) m_description->setText(d.description);
    m_preview->setColors(c);
}

void DesignDialog::accept()
{
    Theme::save(m_design, colors());
    QDialog::accept();
}
