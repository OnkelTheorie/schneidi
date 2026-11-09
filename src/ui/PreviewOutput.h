// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A place outside the main window the preview picture goes to: a second monitor now (ui/SecondScreen: ScreenOutput),
// later a phone's browser or NDI. The main window hands it the frames the engine shows (the same QImage the viewer
// gets - nothing is rendered twice); what it does with them is its own business (a window on a screen, a JPEG stream).

#include <QImage>
#include <QObject>
#include <QString>

class PreviewOutput : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    // Shown to the user (menu, status bar), e.g. the monitor's name
    virtual QString name() const = 0;
    // The frame to show; `projectSize` = the timeline's format (letterboxing, zoom steps)
    virtual void showFrame(const QImage& frame, const QSize& projectSize) = 0;
    // Longer edge the preview frames should at least have for this output to look sharp (Engine::setPreviewEdge);
    // 0 = no wish (the viewer's own size is enough)
    virtual int wantedPreviewEdge() const { return 0; }

signals:
    // The output went away by itself (monitor unplugged, window closed, the phone left): the main window takes the
    // preview back
    void lost();
    void previewEdgeWanted(int px); // wantedPreviewEdge() changed
};
