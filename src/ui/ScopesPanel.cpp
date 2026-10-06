#include "ui/ScopesPanel.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kMinIntervalMs = 40; // at most ~25 analyses per second
constexpr int kMaxLevel = 1023;    // graticule labels in 10-bit code values like DaVinci
} // namespace

// Drawing area: background, trace (scaled up with smoothing), graticule with labels in Theme colours
class ScopeView : public QWidget {
public:
    explicit ScopeView(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(220, 160);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void set(Scopes::Type type, const Scopes::Data* data, const QImage& trace)
    {
        m_type = type;
        m_data = data;
        m_trace = trace;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), Theme::viewerBg);
        QFont f = font();
        f.setPointSizeF(std::max(6.5, f.pointSizeF() * 0.8));
        p.setFont(f);
        switch (m_type) {
        case Scopes::Type::Waveform:
        case Scopes::Type::Parade:
            paintWave(p);
            break;
        case Scopes::Type::Vectorscope:
            paintVector(p);
            break;
        case Scopes::Type::Histogram:
            paintHistogram(p);
            break;
        }
    }

private:
    QRectF plotRect() const
    {
        const int labelW = fontMetrics().horizontalAdvance("1023") + 6;
        return QRectF(labelW, 8, width() - labelW - 8, height() - 16);
    }
    void levelGrid(QPainter& p, const QRectF& r, bool vertical) const
    {
        // Lines every 128 code values, labels every 256 (vertical = histogram: levels along x)
        for (int v = 0; v <= kMaxLevel + 1; v += 128) {
            const int level = std::min(v, kMaxLevel);
            const double t = double(level) / kMaxLevel;
            const bool major = level % 256 == 0 || level == kMaxLevel;
            p.setPen(QPen(Theme::alpha(Theme::textFaint, major ? 150 : 80), 1));
            if (vertical) {
                const double x = r.left() + t * r.width();
                p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            } else {
                const double y = r.bottom() - t * r.height();
                p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
                if (major) {
                    p.setPen(Theme::textDim);
                    p.drawText(QRectF(0, y - 8, r.left() - 4, 16), Qt::AlignRight | Qt::AlignVCenter,
                               QString::number(level));
                }
            }
        }
    }
    void paintWave(QPainter& p) const
    {
        const QRectF r = plotRect();
        if (!m_trace.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            p.drawImage(r, m_trace);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
        levelGrid(p, r, false);
        if (m_type == Scopes::Type::Parade) {
            p.setPen(QPen(Theme::alpha(Theme::textFaint, 200), 1));
            for (int i = 1; i < 3; ++i) {
                const double x = r.left() + r.width() * i / 3.0;
                p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            }
        }
    }
    void paintVector(QPainter& p) const
    {
        const double side = std::min(width(), height()) - 16;
        const QRectF r((width() - side) / 2, (height() - side) / 2, side, side);
        const QPointF c = r.center();
        const double radius = side / 2;
        p.setRenderHint(QPainter::Antialiasing);
        // Outer circle (|chroma| = 0.5), cross, skin tone line (I line, 123°)
        p.setPen(QPen(Theme::alpha(Theme::textFaint, 150), 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, radius, radius);
        p.setPen(QPen(Theme::alpha(Theme::textFaint, 90), 1));
        p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
        p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));
        const double skin = 123.0 * M_PI / 180.0;
        p.setPen(QPen(Theme::alpha(Theme::scopeSkin, 150), 1, Qt::DashLine));
        p.drawLine(c, c + QPointF(std::cos(skin), -std::sin(skin)) * radius);
        if (!m_trace.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            p.drawImage(r, m_trace);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
        // 75 % colour bar targets
        struct Target { const char* name; double r, g, b; QColor color; };
        const Target targets[] = {
            {"R", 0.75, 0, 0, Theme::scopeRed},
            {"Mg", 0.75, 0, 0.75, Theme::mix(Theme::scopeRed, Theme::scopeBlue, 0.5)},
            {"B", 0, 0, 0.75, Theme::scopeBlue},
            {"Cy", 0, 0.75, 0.75, Theme::mix(Theme::scopeGreen, Theme::scopeBlue, 0.5)},
            {"G", 0, 0.75, 0, Theme::scopeGreen},
            {"Yl", 0.75, 0.75, 0, Theme::mix(Theme::scopeRed, Theme::scopeGreen, 0.5)},
        };
        for (const Target& t : targets) {
            double cb, cr;
            Scopes::chroma(t.r, t.g, t.b, &cb, &cr);
            const QPointF pt = c + QPointF(cb, -cr) * (radius / 0.5);
            p.setPen(QPen(Theme::alpha(t.color, 200), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(pt.x() - 5, pt.y() - 5, 10, 10));
            const QPointF dir = (pt - c) / std::max(1.0, std::hypot(pt.x() - c.x(), pt.y() - c.y()));
            const QPointF lp = pt + dir * 16;
            p.drawText(QRectF(lp.x() - 12, lp.y() - 8, 24, 16), Qt::AlignCenter, t.name);
        }
    }
    void paintHistogram(QPainter& p) const
    {
        const QRectF r(8, 8, width() - 16, height() - 24);
        levelGrid(p, r, true);
        p.setPen(Theme::textDim);
        for (int level : {0, 512, kMaxLevel}) {
            const double x = r.left() + double(level) / kMaxLevel * r.width();
            const Qt::Alignment a = level == 0 ? Qt::AlignLeft : level == kMaxLevel ? Qt::AlignRight : Qt::AlignHCenter;
            const double w = 40;
            const double left = level == 0 ? x : level == kMaxLevel ? x - w : x - w / 2;
            p.drawText(QRectF(left, r.bottom() + 1, w, 14), a | Qt::AlignTop, QString::number(level));
        }
        if (!m_data || m_data->isEmpty()) return;
        // Height: largest bin of the inner levels (clipped black/white would flatten everything else)
        quint32 peak = 0;
        for (int ch = 0; ch < 3; ++ch)
            for (int i = 1; i < Scopes::kLevels - 1; ++i) peak = std::max(peak, m_data->hist[ch][i]);
        if (peak == 0)
            for (int ch = 0; ch < 3; ++ch) peak = std::max(peak, *std::max_element(m_data->hist[ch].begin(), m_data->hist[ch].end()));
        if (peak == 0) return;
        p.setRenderHint(QPainter::Antialiasing);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        const QColor colors[3] = {Theme::scopeRed, Theme::scopeGreen, Theme::scopeBlue};
        for (int ch = 0; ch < 3; ++ch) {
            QPainterPath path(QPointF(r.left(), r.bottom()));
            for (int i = 0; i < Scopes::kLevels; ++i) {
                const double h = std::min(1.0, double(m_data->hist[ch][i]) / peak);
                path.lineTo(r.left() + (i + 0.5) / Scopes::kLevels * r.width(), r.bottom() - h * r.height());
            }
            path.lineTo(r.right(), r.bottom());
            path.closeSubpath();
            p.setPen(QPen(Theme::alpha(colors[ch], 220), 1));
            p.setBrush(Theme::alpha(colors[ch], 90));
            p.drawPath(path);
        }
    }

    Scopes::Type m_type = Scopes::Type::Waveform;
    const Scopes::Data* m_data = nullptr;
    QImage m_trace;
};

ScopesPanel::ScopesPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("Panel");
    auto* title = new QLabel(T("Scopes"));
    title->setObjectName("PanelTitle");
    m_combo = new QComboBox;
    m_combo->addItem(T("Waveform"), int(Scopes::Type::Waveform));
    m_combo->addItem(T("Parade"), int(Scopes::Type::Parade));
    m_combo->addItem(T("Vektorskop"), int(Scopes::Type::Vectorscope));
    m_combo->addItem(T("Histogramm"), int(Scopes::Type::Histogram));
    m_combo->setToolTip(T("Art des Scopes (berechnet aus dem Vorschaubild nach der Farbkorrektur)"));
    m_combo->setFocusPolicy(Qt::NoFocus);
    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 8, 0);
    head->setSpacing(6);
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(m_combo);

    m_view = new ScopeView;
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(head);
    lay->addWidget(m_view, 1);

    m_thread = new QThread(this);
    m_thread->setObjectName("scopes");
    m_worker = new QObject;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start(QThread::LowPriority);

    m_throttle = new QTimer(this);
    m_throttle->setSingleShot(true);
    connect(m_throttle, &QTimer::timeout, this, &ScopesPanel::dispatch);

    const int saved = QSettings().value("color/scope", int(Scopes::Type::Waveform)).toInt();
    m_type = Scopes::Type(std::clamp(saved, 0, 3));
    m_combo->setCurrentIndex(m_combo->findData(int(m_type)));
    connect(m_combo, &QComboBox::currentIndexChanged, this, [this] {
        setType(Scopes::Type(m_combo->currentData().toInt()));
        QSettings().setValue("color/scope", int(m_type));
    });
    m_view->set(m_type, &m_data, {});
}

ScopesPanel::~ScopesPanel()
{
    m_thread->quit();
    m_thread->wait();
}

void ScopesPanel::setType(Scopes::Type type)
{
    if (type == m_type) return;
    m_type = type;
    if (const int i = m_combo->findData(int(type)); i != m_combo->currentIndex()) m_combo->setCurrentIndex(i);
    m_view->set(m_type, &m_data, {});
    // New trace for the shown frame
    if (!m_frame.isNull()) {
        m_pending = true;
        dispatch();
    }
}

void ScopesPanel::setFrame(const QImage& frame)
{
    m_frame = frame;
    m_pending = true;
    dispatch();
}

void ScopesPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    dispatch();
}

void ScopesPanel::dispatch()
{
    if (!m_pending || m_busy || m_frame.isNull() || !isVisible()) return;
    if (m_sinceDispatch.isValid() && m_sinceDispatch.elapsed() < kMinIntervalMs) {
        if (!m_throttle->isActive()) m_throttle->start(int(kMinIntervalMs - m_sinceDispatch.elapsed()));
        return;
    }
    m_pending = false;
    m_busy = true;
    m_sinceDispatch.start();
    const QImage frame = m_frame; // shared, read-only in the worker
    const Scopes::Type type = m_type;
    // The destructor waits for the worker thread, so the panel outlives every running analysis; a result posted
    // after the panel is gone is dropped with the panel's event queue.
    ScopesPanel* self = this;
    QMetaObject::invokeMethod(m_worker, [self, frame, type] {
        Scopes::Data data = Scopes::compute(frame);
        QImage trace = Scopes::trace(data, type);
        QMetaObject::invokeMethod(self, [self, data = std::move(data), trace, type] {
            self->m_busy = false;
            if (type == self->m_type) self->onResult(data, trace);
            else self->m_pending = true; // type changed meanwhile -> analyse again
            self->dispatch();
        });
    });
}

void ScopesPanel::onResult(const Scopes::Data& data, const QImage& trace)
{
    m_data = data;
    m_view->set(m_type, &m_data, trace);
    emit analysed();
}
