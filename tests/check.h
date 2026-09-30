#pragma once
// Kleine Test-Hilfen für die Testprogramme (CTest): CHECK/CHECK_EQ zählen Fehler, testResult() liefert den
// Rückgabecode (0 = ok, 1 = Fehler, 77 = übersprungen). Ausgegeben werden nur Fehler und die Zusammenfassung.

#include "core/Types.h"
#include "engine/Bundle.h"

#include <Mlt.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <cstdio>

namespace Check {

inline int& fails() { static int n = 0; return n; }
inline int& total() { static int n = 0; return n; }
constexpr int kSkip = 77;

inline bool report(bool ok, const char* expr, const char* file, int line, const QString& detail = {})
{
    ++total();
    if (ok) return true;
    ++fails();
    std::printf("FEHLER %s:%d  %s\n", QFileInfo(file).fileName().toUtf8().constData(), line, expr);
    if (!detail.isEmpty()) std::printf("       %s\n", qPrintable(detail));
    std::fflush(stdout);
    return false;
}

template <typename A, typename B>
bool equal(const A& a, const B& b, const char* expr, const char* file, int line)
{
    if (a == b) return report(true, expr, file, line);
    QString s;
    QDebug(&s).nospace() << "ist: " << a << "\n       erwartet: " << b;
    return report(false, expr, file, line, s);
}

// Vor dem Anlegen der QApplication: nie sichtbare Fenster, kein Ton
inline void initEnv()
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("SDL_AUDIODRIVER", "dummy");
    qputenv("SDL_NO_SIGNAL_HANDLERS", "1");
}

// Nach dem Anlegen der QApplication: eigener Name und Test-Pfade -> Einstellungen/Cache des Nutzers bleiben unberührt
inline void initApp(const char* name)
{
    QCoreApplication::setOrganizationName("schneidi-tests");
    QCoreApplication::setApplicationName(QString("schneidi-test-%1").arg(name));
    QStandardPaths::setTestModeEnabled(true);
}

// MLT wie das Programm starten (mitgelieferte bzw. unter Windows die MSYS2-Module)
inline void initMlt()
{
    Bundle::prepareMltEnvironment();
    const QString modules = Bundle::mltModuleDir();
    if (modules.isEmpty()) Mlt::Factory::init();
    else Mlt::Factory::init(Bundle::pathForMlt(modules).constData());
}

inline bool haveFfmpeg() { return !QStandardPaths::findExecutable("ffmpeg").isEmpty(); }

// Testmedium per ffmpeg erzeugen (args = Eingänge/Filter/Codecs ohne Ausgabedatei); leer = fehlgeschlagen
inline QString makeMedia(const QString& path, const QStringList& args)
{
    QProcess p;
    p.start("ffmpeg", QStringList{"-y", "-nostdin", "-loglevel", "error"} + args + QStringList{path});
    if (!p.waitForFinished(120000) || p.exitCode() != 0) {
        std::printf("ffmpeg fehlgeschlagen: %s\n", p.readAllStandardError().constData());
        return {};
    }
    return path;
}

inline int result()
{
    std::printf("%s: %d Prüfungen, %d Fehler\n", fails() ? "FEHLER" : "ok", total(), fails());
    return fails() ? 1 : 0;
}

inline int skip(const char* why)
{
    std::printf("übersprungen: %s\n", why);
    return kSkip;
}

// Timeline als kurzer Text für Vergleiche: "V1: a[0-40|0-39] b[40-60|10-29]  A1: …" (Name[start-end|in-out])
inline QString dump(const Timeline& tl)
{
    QString s;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = tl.tracks(k);
        for (int i = 0; i < tracks.size(); ++i) {
            s += QString("%1%2:").arg(k == TrackKind::Video ? "V" : "A").arg(i + 1);
            for (const Clip& c : tracks[i].clips)
                s += QString(" %1[%2-%3|%4-%5]")
                         .arg(c.isTitle()      ? QString("T")
                                      : c.isCompound() ? QString("C%1").arg(c.sequenceId)
                                                       : QFileInfo(c.mediaPath).baseName())
                         .arg(c.start).arg(c.end()).arg(c.in).arg(c.out);
            s += "  ";
        }
    }
    return s.trimmed();
}

} // namespace Check

#define CHECK(...) Check::report(bool(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_EQ(a, b) Check::equal((a), (b), #a " == " #b, __FILE__, __LINE__)
