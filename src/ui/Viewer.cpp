// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/Viewer.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Timecode.h"
#include "engine/Engine.h"
#include "engine/PreviewResolution.h"
#include "engine/StillFetcher.h"
#include "ui/TrimView.h"
#include "ui/MediaPool.h"
#include "ui/FrameView.h"
#include "ui/ViewerOverlay.h"

#include <QAction>
#include <QApplication>
#include <QDrag>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

// Scrubber under the image like in DaVinci: whole length of the source/timeline, In/Out range bright
// with brackets (like in the timeline ruler), playhead red. Click/drag jumps there.
class Scrubber : public QWidget {
public:
    std::function<void(int)> seek;

    explicit Scrubber(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(16);
        setCursor(Qt::PointingHandCursor);
    }
    void setRange(int length, int in, int out)
    {
        m_length = length;
        m_in = in;
        m_out = out;
        update();
    }
    void setMarkers(const QVector<int>& markers)
    {
        if (markers == m_markers) return;
        m_markers = markers;
        update();
    }
    void setPosition(int frame)
    {
        if (frame == m_pos) return;
        m_pos = frame;
        update();
    }

protected:
    static constexpr int kMargin = 6;
    double xOf(double frame) const { return kMargin + frame * (width() - 2 * kMargin) / std::max(1, m_length); }
    int frameAt(int x) const
    {
        const double f = (x - kMargin) * double(std::max(1, m_length)) / std::max(1, width() - 2 * kMargin);
        return std::clamp(int(f), 0, std::max(0, m_length - 1));
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), Theme::panel);
        const int mid = height() / 2;
        // recessed rail with a fine scale below (like on the mixer)
        p.fillRect(QRectF(kMargin, mid - 2, width() - 2 * kMargin, 4), Theme::well);
        for (int i = 0; i <= 20; ++i) {
            const double x = kMargin + i * (width() - 2 * kMargin - 1) / 20.0;
            p.fillRect(QRectF(x, mid + 3, 1, i % 5 ? 1 : 2), Theme::textFaint);
        }
        if (m_length <= 0) return;
        if (m_in >= 0 || m_out >= 0) {
            const double x1 = xOf(m_in >= 0 ? m_in : 0), x2 = xOf(m_out >= 0 ? m_out + 1 : m_length);
            p.fillRect(QRectF(x1, 2, x2 - x1, height() - 4), Theme::alpha(Theme::secondary, 50));
            p.setPen(QPen(Theme::secondary, 2));
            if (m_in >= 0) {
                p.drawLine(QPointF(x1 + 1, 2), QPointF(x1 + 1, height() - 2));
                p.drawLine(QPointF(x1 + 1, 3), QPointF(x1 + 5, 3));
            }
            if (m_out >= 0) {
                p.drawLine(QPointF(x2 - 1, 2), QPointF(x2 - 1, height() - 2));
                p.drawLine(QPointF(x2 - 1, 3), QPointF(x2 - 5, 3));
            }
        }
        // Markers as small triangles on top (like in the timeline ruler)
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::secondary);
        for (int m : m_markers) {
            if (m < 0 || m >= m_length) continue;
            const double mx = xOf(m + 0.5);
            p.drawPolygon(QPolygonF({QPointF(mx - 4, 1), QPointF(mx + 4, 1), QPointF(mx, 7)}));
        }
        const double x = xOf(std::clamp(m_pos, 0, m_length));
        p.setPen(QPen(Theme::playhead, 2));
        p.drawLine(QPointF(x, 1), QPointF(x, height() - 1));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && seek && m_length > 0) seek(frameAt(e->pos().x()));
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if ((e->buttons() & Qt::LeftButton) && seek && m_length > 0) seek(frameAt(e->pos().x()));
    }

private:
    int m_length = 0, m_in = -1, m_out = -1, m_pos = 0;
    QVector<int> m_markers;
};

Viewer::Viewer(Engine* engine, QWidget* parent) : QWidget(parent), m_engine(engine)
{
    setObjectName("Panel");

    m_mode = new QLabel(T("Timeline"));
    m_mode->setObjectName("PanelTitle");
    m_mode->setAlignment(Qt::AlignCenter);

    // Hint bar (as the multicam grid's proxy hint)
    m_hint = new QWidget;
    m_hint->setObjectName("ViewerHint");
    m_hint->setAttribute(Qt::WA_StyledBackground);
    m_hint->setStyleSheet(QString("#ViewerHint { background: %1; }")
                              .arg(Theme::mix(Theme::panel, Theme::meterMid, 0.18).name()));
    auto* hintLay = new QHBoxLayout(m_hint);
    hintLay->setContentsMargins(8, 2, 4, 2);
    hintLay->setSpacing(6);
    m_hintLabel = new QLabel;
    m_hintLabel->setStyleSheet(QString("color: %1;").arg(Theme::text.name()));
    m_hintLabel->setWordWrap(true);
    hintLay->addWidget(m_hintLabel, 1);
    m_hintButton = new QToolButton;
    m_hintButton->setObjectName("viewer_hint_button");
    m_hintButton->setFocusPolicy(Qt::NoFocus);
    hintLay->addWidget(m_hintButton);
    auto* hintClose = new QToolButton;
    hintClose->setObjectName("viewer_hint_close");
    hintClose->setText("✕");
    hintClose->setToolTip(T("Hinweis schließen"));
    hintClose->setAutoRaise(true);
    hintClose->setFocusPolicy(Qt::NoFocus);
    hintLay->addWidget(hintClose);
    connect(m_hintButton, &QToolButton::clicked, this, &Viewer::hintAccepted);
    connect(hintClose, &QToolButton::clicked, this, [this] {
        setHint({});
        emit hintClosed();
    });
    m_hint->hide();

    m_screen = new FrameView;
    m_screen->setObjectName("viewer_screen");
    m_screen->setMinimumSize(320, 180);
    m_screen->installEventFilter(this); // resized / other screen: the automatic preview size follows
    m_edgeTimer = new QTimer(this);
    m_edgeTimer->setSingleShot(true);
    m_edgeTimer->setInterval(150); // a splitter drag asks once at its end, not per pixel
    connect(m_edgeTimer, &QTimer::timeout, this, [this] {
        const int edge = wantedPreviewEdge();
        if (edge != m_lastEdge) {
            m_lastEdge = edge;
            emit previewEdgeWanted();
        }
    });
    m_notice = new QLabel(m_screen);
    m_notice->setObjectName("viewer_notice");
    m_notice->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_notice->setStyleSheet(QString("QLabel { background: rgba(0, 0, 0, 170); color: %1; border-radius: 6px; "
                                    "padding: 6px 12px; font-weight: 600; }")
                                .arg(Theme::text.name()));
    m_notice->hide();
    m_noticeTimer = new QTimer(this);
    m_noticeTimer->setSingleShot(true);
    connect(m_noticeTimer, &QTimer::timeout, m_notice, &QWidget::hide);

    m_trim = new TrimView;
    // The program is on another monitor and no source clip was opened since (setProgramElsewhere)
    m_awayPage = new QWidget;
    m_awayPage->setObjectName("viewer_program_away");
    m_awayPage->setAttribute(Qt::WA_StyledBackground);
    m_awayPage->setStyleSheet(QString("#viewer_program_away { background: %1; }").arg(m_screen->background().name()));
    {
        auto* awayLay = new QVBoxLayout(m_awayPage);
        awayLay->setContentsMargins(16, 16, 16, 16);
        awayLay->addStretch(1);
        auto* text = new QLabel(T("Das Programm läuft auf dem zweiten Monitor.") + "\n"
                                + T("Doppelklick auf einen Clip zeigt ihn hier."));
        text->setAlignment(Qt::AlignCenter);
        text->setWordWrap(true);
        text->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
        awayLay->addWidget(text);
        auto* back = new QToolButton;
        back->setObjectName("viewer_program_back");
        back->setText(T("Vorschau zurückholen"));
        back->setFocusPolicy(Qt::NoFocus);
        connect(back, &QToolButton::clicked, this, &Viewer::programBackWanted);
        // Or give the area to the panels beside/below (Settings › General, the Workspace menu: the same choice)
        auto* hideArea = new QToolButton;
        hideArea->setObjectName("viewer_program_hide_area");
        hideArea->setText(T("Fläche ausblenden"));
        hideArea->setToolTip(T("Die Vorschau-Fläche im Hauptfenster schließen, solange die Vorschau auf dem zweiten "
                               "Monitor ist"));
        hideArea->setFocusPolicy(Qt::NoFocus);
        QFont small = hideArea->font(); // the smaller one of the two, it follows a larger font
        small.setPointSizeF(small.pointSizeF() * 0.85);
        hideArea->setFont(small);
        connect(hideArea, &QToolButton::clicked, this, &Viewer::areaHideWanted);
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(6);
        buttons->addStretch(1);
        buttons->addWidget(back);
        buttons->addWidget(hideArea);
        buttons->addStretch(1);
        awayLay->addLayout(buttons);
        awayLay->addStretch(1);
    }
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_screen);
    m_stack->addWidget(m_trim);
    m_stack->addWidget(m_awayPage);
    connect(m_engine->stills(), &StillFetcher::ready, m_trim,
            [this](const QString& path, int frame, const QImage& img) { m_trim->setImage(path, frame, img); });

    m_scrubber = new Scrubber;
    m_scrubber->seek = [this](int frame) {
        if (m_engine->speed() != 0.0) m_engine->pause();
        m_engine->seek(frame);
    };

    m_timecode = new QLabel;
    // Timecode in the recessed field (design language of the mixer)
    m_timecode->setStyleSheet(QString("color: %1; background: %2; border: 1px solid %3; border-radius: 2px;"
                                      " font-family: monospace; font-size: 13px; padding: 1px 8px;")
                                  .arg(Theme::text.name(), Theme::well.name(), Theme::border.name()));

    auto makeBtn = [](const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setStyleSheet("font-size: 14px; min-width: 28px;");
        return b;
    };
    auto* startBtn = makeBtn("⏮", T("Zum Anfang (Home)"));
    auto* backBtn = makeBtn("◀◀", T("Rückwärts (J)"));
    m_playBtn = makeBtn("▶", T("Play/Pause (Leertaste)"));
    auto* fwdBtn = makeBtn("▶▶", T("Vorwärts (L)"));

    connect(startBtn, &QToolButton::clicked, this, [this] { m_engine->pause(); m_engine->seek(0); });
    connect(backBtn, &QToolButton::clicked, this, [this] { m_engine->setSpeed(-1.0); });
    connect(m_playBtn, &QToolButton::clicked, m_engine, &Engine::togglePlay);
    connect(fwdBtn, &QToolButton::clicked, this, [this] { m_engine->setSpeed(2.0); });

    // Before/After (bypass color correction, Shift+D) like in the DaVinci Viewer; on = orange
    auto* bypass = new QToolButton;
    bypass->setText("◐");
    bypass->setCheckable(true);
    bypass->setFocusPolicy(Qt::NoFocus);
    bypass->setToolTip(T("Vorher/Nachher: Farbkorrektur in der Vorschau umgehen (Shift+D)"));
    bypass->setStyleSheet(QString("QToolButton { font-size: 14px; min-width: 24px; color: %1; }"
                                  "QToolButton:checked { color: %2; }")
                              .arg(Theme::textDim.name(), Theme::primary.name()));
    connect(bypass, &QToolButton::toggled, m_engine, &Engine::setColorBypass);
    connect(m_engine, &Engine::colorBypassChanged, this, [this, bypass](bool on) {
        const QSignalBlocker b(bypass);
        bypass->setChecked(on);
        updateModeText();
    });

    auto* transport = new QHBoxLayout;
    transport->setContentsMargins(4, 2, 4, 4);
    transport->addWidget(m_timecode);
    transport->addStretch(1);
    transport->addWidget(startBtn);
    transport->addWidget(backBtn);
    transport->addWidget(m_playBtn);
    transport->addWidget(fwdBtn);
    transport->addStretch(1);
    m_barBalance = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    transport->addSpacerItem(m_barBalance);
    m_barRight = new QHBoxLayout;
    m_barRight->setSpacing(0);
    m_barRight->addWidget(bypass);
    transport->addLayout(m_barRight);
    m_barBalance->changeSize(std::max(0, m_timecode->sizeHint().width() - m_barRight->sizeHint().width()), 0,
                             QSizePolicy::Fixed, QSizePolicy::Minimum);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_mode);
    lay->addWidget(m_hint);
    lay->addWidget(m_stack, 1);
    lay->addWidget(m_scrubber);
    lay->addLayout(transport);

    connect(m_engine, &Engine::frameReady, m_screen, [this](const QImage& img) {
        const bool program = m_engine->mode() != Engine::Mode::Source;
        if (program) m_programFrame = img; // shared, not copied
        if (program && m_programAway) return; // the program goes to the other monitor only
        m_screen->setFrameSize(m_engine->frameSize()); // the zoom steps refer to the project frame
        m_screen->setFrame(img);
    });
    connect(m_screen, &FrameView::zoomChanged, this, [this](double z) {
        emit zoomChanged(z);
        emit fullSizeWanted(z != FrameView::kFit && z >= 1.0);
    });
    connect(m_engine, &Engine::positionChanged, this, &Viewer::updateTimecode);
    connect(m_engine, &Engine::speedChanged, this,
            [this](double s) { m_playBtn->setText(s == 0.0 ? "▶" : "⏸"); });
    // Loop playback in the In/Out range (ruler menu, Playback menu): the button says so
    connect(m_engine, &Engine::loopChanged, this, [this](bool on) {
        m_playBtn->setToolTip(on ? T("Play/Pause (Leertaste) – wiederholt im In/Out-Bereich") : T("Play/Pause (Leertaste)"));
    });
    updateTimecode(0);
}

namespace {

// Line icons for the view buttons (font glyphs for these differ too much between systems): off dim, on orange
QIcon viewIcon(Viewer::ViewIcon kind)
{
    QIcon icon;
    for (const auto& [state, color] : {std::pair{QIcon::Off, Theme::textDim}, std::pair{QIcon::On, Theme::primary}}) {
        QPixmap pm(36, 36);
        pm.setDevicePixelRatio(2);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        const QRectF r(2.5, 4.5, 13, 9);
        if (kind == Viewer::ViewIcon::FullScreen) {
            // Four corners: the picture to the screen's edges
            constexpr double c = 3.5;
            p.drawPolyline(QPolygonF({{r.left(), r.top() + c}, r.topLeft(), {r.left() + c, r.top()}}));
            p.drawPolyline(QPolygonF({{r.right() - c, r.top()}, r.topRight(), {r.right(), r.top() + c}}));
            p.drawPolyline(QPolygonF({{r.left(), r.bottom() - c}, r.bottomLeft(), {r.left() + c, r.bottom()}}));
            p.drawPolyline(QPolygonF({{r.right() - c, r.bottom()}, r.bottomRight(), {r.right(), r.bottom() - c}}));
        } else {
            // The frame with the picture pushing out to both sides
            p.drawRect(r);
            const double y = r.center().y();
            p.drawLine(QPointF(r.left() + 2.5, y), QPointF(r.right() - 2.5, y));
            p.drawPolyline(QPolygonF({{r.left() + 4.5, y - 2}, {r.left() + 2.5, y}, {r.left() + 4.5, y + 2}}));
            p.drawPolyline(QPolygonF({{r.right() - 4.5, y - 2}, {r.right() - 2.5, y}, {r.right() - 4.5, y + 2}}));
        }
        p.end();
        icon.addPixmap(pm, QIcon::Normal, state);
    }
    return icon;
}

} // namespace

QToolButton* Viewer::addViewAction(QAction* action, ViewIcon icon)
{
    auto* b = new QToolButton;
    b->setObjectName(icon == ViewIcon::Large ? "viewer_large" : "viewer_full_screen");
    b->setIcon(viewIcon(icon));
    b->setIconSize(QSize(18, 18));
    b->setCheckable(true);
    b->setChecked(action->isChecked());
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(icon == ViewIcon::Large
                      ? T("Große Vorschau: Bereiche ausblenden, das Bild bekommt ihren Platz (Esc: zurück)")
                      : T("Vorschau im Vollbild: nur das Bild auf dem ganzen Bildschirm (Esc: zurück)"));
    // The action decides (it may refuse); the button only shows its state
    connect(b, &QToolButton::clicked, action, [b, action] {
        b->setChecked(action->isChecked());
        action->trigger();
    });
    connect(action, &QAction::toggled, b, &QToolButton::setChecked);
    m_barRight->addWidget(b);
    m_barBalance->changeSize(std::max(0, m_timecode->sizeHint().width() - m_barRight->sizeHint().width()), 0,
                             QSizePolicy::Fixed, QSizePolicy::Minimum);
    layout()->invalidate();
    return b;
}

QImage Viewer::picture() const
{
    return m_screen->frame();
}

void Viewer::setZoom(double zoom)
{
    m_screen->setZoom(zoom);
}

double Viewer::zoom() const
{
    return m_screen->zoom();
}

void Viewer::addZoomActions(const QList<QAction*>& actions)
{
    // Menu button left of Before/After: shows the zoom, the menu holds the steps
    auto* b = new QToolButton;
    b->setObjectName("viewer_zoom");
    b->setPopupMode(QToolButton::InstantPopup);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(T("Zoom der Vorschau (Strg+Mausrad im Bild, mittlere Maustaste verschiebt)"));
    b->setStyleSheet(QString("QToolButton { color: %1; padding: 0 4px; } QToolButton::menu-indicator { image: none; }")
                         .arg(Theme::textDim.name()));
    auto* menu = new QMenu(b);
    menu->addActions(actions);
    b->setMenu(menu);
    const auto label = [](double z) {
        return z == FrameView::kFit ? T("Einpassen") : QString("%1 %").arg(QString::number(z * 100, 'g', 4).replace('.', I18n::decimalPoint()));
    };
    b->setText(label(zoom()));
    connect(this, &Viewer::zoomChanged, b, [b, label](double z) { b->setText(label(z)); });
    m_barRight->insertWidget(0, b);
    m_barBalance->changeSize(std::max(0, m_timecode->sizeHint().width() - m_barRight->sizeHint().width()), 0,
                             QSizePolicy::Fixed, QSizePolicy::Minimum);
    layout()->invalidate();
}

int Viewer::heightFor(int width) const
{
    const QSize frame = m_engine->frameSize();
    if (frame.isEmpty()) return 0;
    return height() - m_stack->height() + int(std::ceil(double(width) * frame.height() / frame.width()));
}

int Viewer::wantedPreviewEdge() const
{
    if (!m_screen->isVisible() || m_screen->width() <= 0 || m_screen->height() <= 0) return 0;
    const QSize px = m_screen->size() * m_screen->devicePixelRatioF();
    QSize project = m_engine->frameSize();
    if (project.isEmpty()) project = px;
    const QSize covered = project.scaled(px, Qt::KeepAspectRatio);
    const int longer = std::max(covered.width(), covered.height());
    return std::clamp((longer + 63) / 64 * 64, 64, PreviewResolution::kAutoCap);
}

bool Viewer::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_screen) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::DevicePixelRatioChange:
            m_edgeTimer->start();
            if (m_notice->isVisible()) m_notice->move((m_screen->width() - m_notice->width()) / 2, 12);
            break;
        default: break;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void Viewer::showNotice(const QString& text, int ms)
{
    m_notice->setText(text);
    m_notice->adjustSize();
    m_notice->move((m_screen->width() - m_notice->width()) / 2, 12);
    m_notice->show();
    m_notice->raise(); // above the GPU surface and the overlay layer
    m_noticeTimer->start(ms);
}

QString Viewer::notice() const
{
    return m_notice->isHidden() ? QString() : m_notice->text();
}

void Viewer::setHint(const QString& text, const QString& button)
{
    m_hintLabel->setText(text);
    m_hintButton->setText(button);
    m_hintButton->setVisible(!button.isEmpty());
    m_hint->setVisible(!text.isEmpty());
}

QString Viewer::hint() const
{
    return m_hint->isHidden() ? QString() : m_hintLabel->text();
}

void Viewer::showTrim(const TrimFrames::View& view)
{
    if (view.isNull()) {
        endTrim();
        return;
    }
    m_trim->setFps(m_engine->fps());
    m_trim->setAspect(m_engine->frameSize());
    m_trim->setView(view);
    // Cached frames at once, the rest in the background (latest request wins -> stays responsive while dragging)
    StillFetcher* stills = m_engine->stills();
    QVector<StillFetcher::Request> missing;
    for (const QVector<TrimFrames::Pane>* panes : {&view.main, &view.small})
        for (const TrimFrames::Pane& pane : *panes) {
            if (pane.path.isEmpty()) continue;
            const QImage img = stills->cached(pane.path, pane.fileFrame);
            if (!img.isNull()) m_trim->setImage(pane.path, pane.fileFrame, img);
            else missing << StillFetcher::Request{pane.path, pane.fileFrame};
        }
    stills->request(missing);
    m_stack->setCurrentWidget(m_trim);
}

void Viewer::endTrim()
{
    m_engine->stills()->request({}); // nothing more to decode
    m_trim->setView({});
    m_stack->setCurrentWidget(m_screen); // not the trim view any more: updatePage chooses
    updatePage();
}

void Viewer::setMulticamView(QWidget* view)
{
    m_multicam = view;
    m_stack->addWidget(view);
}

void Viewer::showMulticam(bool on)
{
    if (on == m_multicamOn) return;
    m_multicamOn = on;
    updatePage();
    updateModeText();
}

void Viewer::setProgramElsewhere(bool on)
{
    if (on == m_programAway) return;
    m_programAway = on;
    m_keptSource = on && !m_sourcePath.isEmpty() ? m_sourceName : QString();
    // Back home on the timeline: its picture instead of the source frame kept meanwhile
    if (!on && m_sourcePath.isEmpty() && !m_programFrame.isNull()) m_screen->setFrame(m_programFrame);
    updatePage();
    updateModeText();
}

bool Viewer::programAwayPageShown() const
{
    return m_stack->currentWidget() == m_awayPage;
}

bool Viewer::keepsPicture() const
{
    return m_programAway && m_sourcePath.isEmpty();
}

void Viewer::updatePage()
{
    // The overlay's handles belong to the timeline's picture: not over a kept source frame
    if (ViewerOverlay* want = keepsPicture() ? nullptr : m_overlay; m_screen->overlay() != want) m_screen->setOverlay(want);
    if (trimShown()) return; // endTrim comes back here
    QWidget* page = m_screen;
    if (m_multicamOn && m_multicam) page = m_multicam; // the angles stay in the main window (program on the monitor)
    else if (keepsPicture() && m_keptSource.isEmpty()) page = m_awayPage;
    m_stack->setCurrentWidget(page);
}

void Viewer::setMarkers(const QVector<int>& markers)
{
    m_scrubber->setMarkers(markers);
}

bool Viewer::trimShown() const
{
    return m_stack->currentWidget() == m_trim;
}

void Viewer::setOverlay(ViewerOverlay* overlay)
{
    m_overlay = overlay;
    m_screen->setOverlay(keepsPicture() ? nullptr : overlay);
}

ViewerOverlay* Viewer::overlay() const { return m_overlay; }

void Viewer::setRange(int length, int markIn, int markOut)
{
    m_length = length;
    m_in = markIn;
    m_out = markOut;
    m_scrubber->setRange(length, markIn, markOut);
}

void Viewer::setSource(const QString& path, const QString& name)
{
    m_sourcePath = path;
    m_sourceName = name;
    if (m_programAway && !path.isEmpty()) m_keptSource = name;
    updatePage();
    updateModeText();
    m_screen->setIdleCursor(path.isEmpty() ? Qt::ArrowCursor : Qt::OpenHandCursor);
    // Only a source can be dragged into the timeline; in the timeline view a left drag pans the zoomed picture
    if (path.isEmpty()) m_screen->dragData = nullptr;
    else m_screen->dragData = [this] { return dragData(); };
}

void Viewer::updateModeText()
{
    QString text = !m_sourcePath.isEmpty() ? T("Quelle – %1").arg(m_sourceName)
                   : m_multicamOn         ? T("Timeline – Multicam")
                   : !m_programAway       ? T("Timeline")
                   : m_keptSource.isEmpty() ? T("Timeline auf dem zweiten Monitor")
                                            : T("Quelle – %1 (angehalten)").arg(m_keptSource);
    if (m_engine->colorBypass()) text += T("  (Farbkorrektur umgangen)");
    m_mode->setTextFormat(m_recording ? Qt::RichText : Qt::PlainText);
    if (m_recording)
        text = text.toHtmlEscaped()
               + QString("&nbsp;&nbsp;<b style=\"color:%1\">● %2</b>").arg(Theme::recording.name(), T("Aufnahme"));
    m_mode->setText(text);
}

void Viewer::setRecording(bool on)
{
    m_recording = on;
    updateModeText();
}

QMimeData* Viewer::dragData() const
{
    if (m_sourcePath.isEmpty() || m_length <= 0) return nullptr;
    const int in = m_in >= 0 ? m_in : 0;
    const int out = m_out >= 0 ? m_out : m_length - 1;
    if (out < in) return nullptr;
    auto* mime = new QMimeData;
    mime->setData(MediaPool::MimeType, m_sourcePath.toUtf8());
    mime->setData(RangeMimeType, QString("%1 %2").arg(in).arg(out).toUtf8());
    return mime;
}

void Viewer::updateTimecode(int frame)
{
    m_timecode->setText(Timecode::format(frame, m_engine->fps()));
    m_scrubber->setPosition(frame);
}
