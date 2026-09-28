#include "ui/Viewer.h"

#include "app/Theme.h"
#include "core/Timecode.h"
#include "engine/Engine.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

// Zeichnet das aktuelle Frame, seitenverhältnis-treu, auf schwarzem Grund
class Screen : public QWidget {
public:
    using QWidget::QWidget;
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

private:
    QImage m_image;
};

Viewer::Viewer(Engine* engine, QWidget* parent) : QWidget(parent), m_engine(engine)
{
    setObjectName("Panel");

    m_mode = new QLabel("Timeline");
    m_mode->setObjectName("PanelTitle");
    m_mode->setAlignment(Qt::AlignCenter);

    m_screen = new Screen;
    m_screen->setMinimumSize(320, 180);

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
    auto* startBtn = makeBtn("⏮", "Zum Anfang (Home)");
    auto* backBtn = makeBtn("◀◀", "Rückwärts (J)");
    m_playBtn = makeBtn("▶", "Play/Pause (Leertaste)");
    auto* fwdBtn = makeBtn("▶▶", "Vorwärts (L)");

    connect(startBtn, &QToolButton::clicked, this, [this] { m_engine->pause(); m_engine->seek(0); });
    connect(backBtn, &QToolButton::clicked, this, [this] { m_engine->setSpeed(-1.0); });
    connect(m_playBtn, &QToolButton::clicked, m_engine, &Engine::togglePlay);
    connect(fwdBtn, &QToolButton::clicked, this, [this] { m_engine->setSpeed(2.0); });

    auto* transport = new QHBoxLayout;
    transport->setContentsMargins(4, 2, 4, 4);
    transport->addWidget(m_timecode);
    transport->addStretch(1);
    transport->addWidget(startBtn);
    transport->addWidget(backBtn);
    transport->addWidget(m_playBtn);
    transport->addWidget(fwdBtn);
    transport->addStretch(1);
    transport->addSpacing(m_timecode->sizeHint().width());

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_mode);
    lay->addWidget(m_screen, 1);
    lay->addLayout(transport);

    connect(m_engine, &Engine::frameReady, m_screen, &Screen::setImage);
    connect(m_engine, &Engine::positionChanged, this, &Viewer::updateTimecode);
    connect(m_engine, &Engine::speedChanged, this,
            [this](double s) { m_playBtn->setText(s == 0.0 ? "▶" : "⏸"); });
    connect(m_engine, &Engine::modeChanged, this, [this](Engine::Mode m) {
        m_mode->setText(m == Engine::Mode::Timeline ? "Timeline" : "Quelle");
    });
    updateTimecode(0);
}

void Viewer::updateTimecode(int frame)
{
    m_timecode->setText(Timecode::format(frame, m_engine->fps()));
}
