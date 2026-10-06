#pragma once
#include "ui/Scopes.h"

#include <QElapsedTimer>
#include <QImage>
#include <QWidget>

class QComboBox;
class QThread;
class QTimer;

// Scopes on the Color page (like DaVinci's scope panel): one scope at a time, chosen in the header
// (Waveform, Parade, Vectorscope, Histogram; remembered in QSettings). Fed with every preview frame (after the
// grade); the analysis runs on a worker thread. While a frame is being analysed newer frames only replace the
// pending one (the latest wins), and at most ~25 analyses per second run -> playback is not slowed down.
// Hidden panels do not analyse at all; showing the panel analyses the last frame.
class ScopesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ScopesPanel(QWidget* parent = nullptr);
    ~ScopesPanel() override;

    Scopes::Type type() const { return m_type; }
    void setType(Scopes::Type type);
    // Last finished analysis (for tests)
    const Scopes::Data& data() const { return m_data; }
    bool busy() const { return m_busy; }

public slots:
    void setFrame(const QImage& frame);

signals:
    void analysed(); // a result arrived (tests)

protected:
    void showEvent(QShowEvent* e) override;

private:
    void dispatch();
    void onResult(const Scopes::Data& data, const QImage& trace);

    class ScopeView* m_view;
    QComboBox* m_combo;
    QThread* m_thread;
    QObject* m_worker; // lives in m_thread, runs the analysis
    QTimer* m_throttle;
    QElapsedTimer m_sinceDispatch;
    Scopes::Type m_type = Scopes::Type::Waveform;
    QImage m_frame;         // newest frame
    bool m_pending = false; // m_frame not analysed yet
    bool m_busy = false;
    Scopes::Data m_data;
};
