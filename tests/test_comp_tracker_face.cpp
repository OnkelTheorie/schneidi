// SPDX-License-Identifier: AGPL-3.0-only
// Tracker node in face mode with the real YuNet model (extension "faces", or SCHNEIDI_YUNET + a runtime); skipped (77)
// without it - never downloads anything. Only synthetic pictures (no real person): a grey picture and a moving square
// have no face, so the analysis says so instead of tracking something else. The face-following logic itself is
// tested with a stand-in detector in test_comp_tracker.
#include "check.h"
#include "engine/FaceDetect.h"
#include "engine/picture/CompTrack.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cstdio>

using namespace Picture::Comp;

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    QString why;
    if (!FaceDetect::available(&why)) {
        std::printf("skipped: %s\n", qPrintable(why));
        // Without it the error is clear and nothing is tracked
        QString err;
        CHECK(!installedFaces(&err));
        CHECK(!err.isEmpty());
        return Check::fails() ? Check::result() : Check::kSkip;
    }
    QString err;
    CHECK(bool(installedFaces(&err)));
    CHECK(err.isEmpty());
    const auto frame = [](int t) {
        QImage img(1280, 720, QImage::Format_RGB32);
        img.fill(QColor(128, 128, 128));
        QPainter p(&img);
        p.fillRect(QRectF(300 + 5 * t, 300, 80, 80), QColor(220, 180, 60));
        return img;
    };
    TrackStart st;
    st.mode = TrackMode::Face;
    st.a = {0.27, 0.47};
    const TrackRun r = trackFrames(frame, st, 0, 5);
    CHECK(!r.error.isEmpty()); // no face at the start point
    CHECK(r.points.isEmpty());
    return Check::result();
}
