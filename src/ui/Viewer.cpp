#include "ui/Viewer.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Timecode.h"
#include "engine/Engine.h"
#include "ui/MediaPool.h"

#include <QApplication>
#include <QDrag>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

// Zeichnet das aktuelle Frame, seitenverhältnis-treu, auf schwarzem Grund.
// Mit gesetzter dragData-Funktion lässt sich das Bild in die Timeline ziehen.
class Screen : public QWidget {
public:
    using QWidget::QWidget;
    std::function<QMimeData*()> dragData;

    void setImage(const QImage& img)
    {
        m_image = img;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x10, 0x10, 0x12));
        if (m_image.isNull()) return;
        QSize s = m_image.size().scaled(size(), Qt::KeepAspectRatio);
        QRect r(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(r, m_image);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) m_pressPos = e->pos();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || m_pressPos.x() < 0 || !dragData) return;
        if ((e->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        m_pressPos = QPoint(-1, -1);
        QMimeData* mime = dragData();
        if (!mime) return;
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        if (!m_image.isNull())
            drag->setPixmap(QPixmap::fromImage(m_image.scaled(160, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        drag->exec(Qt::CopyAction);
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_pressPos = QPoint(-1, -1); }

private:
    QImage m_image;
    QPoint m_pressPos{-1, -1};
};

// Scrubber unter dem Bild wie in DaVinci: ganze Länge der Quelle/Timeline, In/Out-Bereich hell
// mit Klammern (wie im Timeline-Lineal), Playhead rot. Klicken/Ziehen springt dorthin.
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
        p.fillRect(QRectF(kMargin, mid - 2, width() - 2 * kMargin, 4), Theme::trackBg);
        if (m_length <= 0) return;
        if (m_in >= 0 || m_out >= 0) {
            const double x1 = xOf(m_in >= 0 ? m_in : 0), x2 = xOf(m_out >= 0 ? m_out + 1 : m_length);
            p.fillRect(QRectF(x1, 2, x2 - x1, height() - 4), QColor(255, 255, 255, 45));
            p.setPen(QPen(Theme::text, 2));
            if (m_in >= 0) {
                p.drawLine(QPointF(x1 + 1, 2), QPointF(x1 + 1, height() - 2));
                p.drawLine(QPointF(x1 + 1, 3), QPointF(x1 + 5, 3));
            }
            if (m_out >= 0) {
                p.drawLine(QPointF(x2 - 1, 2), QPointF(x2 - 1, height() - 2));
                p.drawLine(QPointF(x2 - 1, 3), QPointF(x2 - 5, 3));
            }
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
};

Viewer::Viewer(Engine* engine, QWidget* parent) : QWidget(parent), m_engine(engine)
{
    setObjectName("Panel");

    m_mode = new QLabel("Timeline");
    m_mode->setObjectName("PanelTitle");
    m_mode->setAlignment(Qt::AlignCenter);

    m_screen = new Screen;
    m_screen->setMinimumSize(320, 180);
    m_screen->dragData = [this] { return dragData(); };

    m_scrubber = new Scrubber;
    m_scrubber->seek = [this](int frame) {
        if (m_engine->speed() != 0.0) m_engine->pause();
        m_engine->seek(frame);
    };

    m_timecode = new QLabel;
    m_timecode->setStyleSheet(QString("color: %1; font-family: monospace; font-size: 13px; padding: 0 8px;")
                                  .arg(Theme::text.name()));

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

    // Vorher/Nachher (Farbkorrektur umgehen, Shift+D) wie im DaVinci-Viewer; an = orange
    auto* bypass = new QToolButton;
    bypass->setText("◐");
    bypass->setCheckable(true);
    bypass->setFocusPolicy(Qt::NoFocus);
    bypass->setToolTip(T("Vorher/Nachher: Farbkorrektur in der Vorschau umgehen (Shift+D)"));
    bypass->setStyleSheet(QString("QToolButton { font-size: 14px; min-width: 24px; color: %1; }"
                                  "QToolButton:checked { color: %2; }")
                              .arg(Theme::textDim.name(), Theme::accent.name()));
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
    transport->addSpacing(std::max(0, m_timecode->sizeHint().width() - bypass->sizeHint().width()));
    transport->addWidget(bypass);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_mode);
    lay->addWidget(m_screen, 1);
    lay->addWidget(m_scrubber);
    lay->addLayout(transport);

    connect(m_engine, &Engine::frameReady, m_screen, &Screen::setImage);
    connect(m_engine, &Engine::positionChanged, this, &Viewer::updateTimecode);
    connect(m_engine, &Engine::speedChanged, this,
            [this](double s) { m_playBtn->setText(s == 0.0 ? "▶" : "⏸"); });
    updateTimecode(0);
}

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
    updateModeText();
    m_screen->setCursor(path.isEmpty() ? Qt::ArrowCursor : Qt::OpenHandCursor);
}

void Viewer::updateModeText()
{
    QString text = m_sourcePath.isEmpty() ? QStringLiteral("Timeline") : T("Quelle – %1").arg(m_sourceName);
    if (m_engine->colorBypass()) text += T("  (Farbkorrektur umgangen)");
    m_mode->setText(text);
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
