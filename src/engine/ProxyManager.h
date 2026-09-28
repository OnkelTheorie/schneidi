#pragma once
// Proxy-Medien wie in DaVinci ("Generate Proxy Media"): kleine H.264-Kopien der Originale
// für eine flüssige Vorschau. Erzeugt im Hintergrund per ffmpeg (QProcess, eine Datei nach der anderen),
// abgelegt in ~/.cache/schneidi/proxies. Der Dateiname ergibt sich aus Pfad, Größe und Änderungszeit
// des Originals -> die Projektdatei muss nichts über Proxies wissen. Export nutzt immer die Originale.

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

class ProxyManager : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxSize = 960; // längere Bildkante des Proxys (Pixel)

    explicit ProxyManager(QObject* parent = nullptr);
    ~ProxyManager() override;

    static QString cacheDir();
    // Ablageort des Proxys zu einem Original (unabhängig davon, ob er schon existiert)
    static QString proxyPath(const QString& original);
    bool hasProxy(const QString& original) const;
    // Für die Vorschau: Proxy, wenn eingeschaltet und vorhanden, sonst das Original
    QString resolve(const QString& original) const;

    // "Proxy-Medien verwenden, falls vorhanden" (gespeichert, Standard an)
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

    void generate(const QStringList& originals); // in die Warteschlange (vorhandene/wartende werden übersprungen)
    void remove(const QStringList& originals);   // Proxy löschen (laufende Erzeugung wird abgebrochen)
    void cancel(const QStringList& originals);
    void cancelAll();

    bool isPending(const QString& original) const; // wartet oder läuft gerade
    int progressOf(const QString& original) const; // 0..100, -1 = nicht in Arbeit
    int pendingCount() const { return m_queue.size() + (m_current.isEmpty() ? 0 : 1); }
    QString current() const { return m_current; }

signals:
    void progress(const QString& original, int percent);
    // Proxy entstanden, gelöscht oder Erzeugung beendet/abgebrochen -> Anzeige/Vorschau auffrischen
    void proxyChanged(const QString& original);
    void failed(const QString& original, const QString& message);
    void enabledChanged(bool on);
    void queueChanged(); // Warteschlange/Fortschritt geändert (Statusanzeige)

private:
    void startNext();
    void launch(const QString& original, const QStringList& args, double durationUs, const QString& error);
    void onOutput();
    void onFinished(int exitCode, bool crashed);
    void stopCurrent();

    bool m_enabled = true;
    QStringList m_queue;
    QString m_current;      // Original, dessen Proxy gerade entsteht
    QString m_currentPart;  // halbfertige Datei (wird erst am Ende umbenannt)
    double m_durationUs = 0;
    int m_progress = 0;
    QString m_errorTail;    // letzte ffmpeg-Meldungen für die Fehleranzeige
    QProcess* m_process = nullptr;
    class QThread* m_probeThread = nullptr; // untersucht die nächste Quelle
    int m_job = 0;                          // hochgezählt bei Start/Abbruch; veraltete Untersuchungen verfallen
};
