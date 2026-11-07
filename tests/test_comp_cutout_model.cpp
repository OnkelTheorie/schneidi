// SPDX-License-Identifier: AGPL-3.0-only
// AI cutout with the real person segmentation model (extension "cutout", or SCHNEIDI_CUTOUT_MODEL + a runtime);
// skipped (77) without it - never downloads anything. Synthetic pictures only (no real person): the model must load,
// take its input and answer a plane of its own size; timing per frame at 1080p for the report. The math around it is
// tested with a stand-in in test_comp_cutout.
#include "check.h"
#include "engine/PersonMatte.h"
#include "engine/picture/CompCutout.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cstdio>

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("comp_cutout_model");
    QString why;
    if (!PersonMatte::available(&why)) {
        std::printf("skipped: %s\n", qPrintable(why));
        QString err;
        CHECK(!PersonMatte::open(&err));
        CHECK(err.contains(PersonMatte::needsExtension()));
        return Check::fails() ? Check::result() : Check::kSkip;
    }
    QString err;
    const auto s = PersonMatte::open(&err);
    if (!CHECK(s.has_value())) {
        std::printf("       %s\n", qPrintable(err));
        return Check::result();
    }
    QImage img(1920, 1080, QImage::Format_RGB32);
    img.fill(QColor(90, 120, 160));
    {
        QPainter p(&img);
        p.setBrush(QColor(200, 160, 130));
        p.drawEllipse(QRect(860, 200, 200, 260)); // a head-like blob, no real person
        p.fillRect(QRect(760, 460, 400, 620), QColor(40, 40, 60));
    }
    int w = 0, h = 0;
    std::vector<uint8_t> a = PersonMatte::segment(*s, img, &w, &h, &err);
    CHECK(!a.empty() && size_t(w) * h == a.size() && err.isEmpty());
    QElapsedTimer t;
    t.start();
    const int n = 10;
    for (int i = 0; i < n; ++i) a = PersonMatte::segment(*s, img, &w, &h, &err);
    std::printf("model %dx%d on a 1920x1080 picture: %.1f ms per frame\n", s->spec.width, s->spec.height,
                t.nsecsElapsed() / 1e6 / n);
    const auto run = Picture::Comp::cutoutFrames([&](int) { return img; }, 0, 4, *s, "model-test");
    CHECK(run.error.isEmpty() && run.frames == 5);
    std::printf("analysis (picture given at 1080p): %.1f ms per frame\n", run.msPerFrame);
    Picture::Comp::removeCutoutCache("model-test");
    return Check::result();
}
