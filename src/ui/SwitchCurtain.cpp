// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/SwitchCurtain.h"

#include "app/Theme.h"

#include <QPainter>
#include <QSettings>
#include <QTimer>
#include <QVariantAnimation>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif

bool SwitchCurtain::reducedMotion()
{
    const QString env = qEnvironmentVariable("SCHNEIDI_REDUCE_MOTION");
    if (env == "1") return true;
    if (env == "0") return false;
    if (QSettings().value("ui/reduceMotion", false).toBool()) return true;
#ifdef Q_OS_ANDROID
    // Settings › Accessibility › "Remove animations": the animator duration scale 0
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid()) {
        QJniObject resolver = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
        const float scale = QJniObject::callStaticMethod<jfloat>(
            "android/provider/Settings$Global", "getFloat", "(Landroid/content/ContentResolver;Ljava/lang/String;F)F",
            resolver.object(), QJniObject::fromString("animator_duration_scale").object<jstring>(), jfloat(1.0f));
        if (scale == 0.0f) return true;
    }
#endif
    return false;
}

SwitchCurtain::SwitchCurtain(QWidget* window) : QWidget(window)
{
    setObjectName("switchCurtain");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_picture = window->grab();
    setGeometry(window->rect());
    m_spin = new QTimer(this);
    m_spin->setInterval(40);
    connect(m_spin, &QTimer::timeout, this, [this] {
        m_angle = (std::max(0, m_angle) + 30) % 360;
        update();
    });
    QTimer::singleShot(kSpinnerAfterMs, this, [this] { m_spin->start(); }); // (only a change that lets events run)
    show();
    raise();
    repaint(); // the frozen picture before the change starts underneath
}

SwitchCurtain* SwitchCurtain::cover(QWidget* window)
{
    if (!window || !window->isVisible()) return nullptr;
    return new SwitchCurtain(window);
}

void SwitchCurtain::reveal()
{
    m_spin->stop();
    m_angle = -1;
    if (parentWidget()) setGeometry(parentWidget()->rect());
    raise();
    if (reducedMotion()) {
        deleteLater();
        return;
    }
    auto* fade = new QVariantAnimation(this);
    fade->setStartValue(1.0);
    fade->setEndValue(0.0);
    fade->setDuration(kFadeMs);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_opacity = v.toDouble();
        update();
    });
    connect(fade, &QVariantAnimation::finished, this, &QObject::deleteLater);
    fade->start();
}

void SwitchCurtain::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setOpacity(m_opacity);
    // The old picture at its old size (the window may be another size already): from the top left, the rest the
    // window's background
    p.fillRect(rect(), Theme::window);
    p.drawPixmap(0, 0, m_picture);
    if (m_angle >= 0) { // the spinner: an arc in the middle
        const int d = 36;
        const QRect r(width() / 2 - d / 2, height() / 2 - d / 2, d, d);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 120));
        p.drawEllipse(r.adjusted(-8, -8, 8, 8));
        p.setPen(QPen(Theme::primary, 4, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(r, -m_angle * 16, 270 * 16);
    }
}
