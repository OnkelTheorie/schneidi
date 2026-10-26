#pragma once
// Shared setup of the fuzz targets (libFuzzer, see CMakeLists.txt here): one Qt application for the whole run, test
// paths (the user's settings stay untouched), a work folder with traps for path checks, and a check that stops the
// run like a crash when a rule is broken.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// A rule of the code under test is broken: stop like a crash (libFuzzer saves the input)
#define FUZZ_ASSERT(cond)                                                                                             \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            std::fprintf(stderr, "FUZZ_ASSERT failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                    \
            std::abort();                                                                                             \
        }                                                                                                             \
    } while (0)

namespace Fuzz {

// Before the first input (LLVMFuzzerInitialize): offscreen, no windows, no sound
inline void init(const char* name)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("SCHNEIDI_AUDIO_DEVICE", "null");
    // Millions of inputs: no log line per input (set QT_LOGGING_RULES to see them while looking at a finding)
    if (!qEnvironmentVariableIsSet("QT_LOGGING_RULES")) qputenv("QT_LOGGING_RULES", "*.debug=false;*.warning=false");
    static int argc = 1;
    static char arg0[] = "schneidi-fuzz";
    static char* argv[] = {arg0, nullptr};
    // Titles and subtitles have fonts: a GUI application (offscreen) like the tests
    static QGuiApplication* app = new QGuiApplication(argc, argv);
    (void)app;
    QCoreApplication::setOrganizationName("schneidi-fuzz");
    QCoreApplication::setApplicationName(QString("schneidi-fuzz-%1").arg(name));
    QStandardPaths::setTestModeEnabled(true);
}

// Reads the first bytes of the input as choices of the harness; the rest is the data
struct Choices {
    const uint8_t* data;
    size_t size;
    uint8_t take()
    {
        if (size == 0) return 0;
        --size;
        return *data++;
    }
};

// A work folder (cli/Sandbox.h) with what an attacker would like to reach: hidden files, links out of it, a link to
// nowhere, a file outside
struct WorkFolder {
    QTemporaryDir dir;
    QString root, canonical;

    WorkFolder()
    {
        if (!dir.isValid()) std::abort();
        QDir d(dir.path());
        d.mkpath("root/sub/deeper");
        d.mkpath("root/.ssh");
        d.mkpath("outside");
        QFile(d.filePath("outside/secret")).open(QIODevice::WriteOnly);
        QFile(d.filePath("root/.ssh/id_rsa")).open(QIODevice::WriteOnly);
        QFile clip(d.filePath("root/clip.mp4"));
        clip.open(QIODevice::WriteOnly);
        clip.write(QByteArray(1000, 'x'));
        clip.close();
        QFile::link(d.filePath("outside"), d.filePath("root/out"));
        QFile::link(d.filePath("outside/secret"), d.filePath("root/sub/secret"));
        QFile::link(d.filePath("nowhere"), d.filePath("root/dangling"));
        root = QDir::cleanPath(d.filePath("root"));
        canonical = QFileInfo(root).canonicalFilePath();
    }

    static bool under(const QString& path, const QString& dir) { return path == dir || path.startsWith(dir + '/'); }

    // The work folder's promise for a path the server accepted: inside the root, also through links
    void checkInside(const QString& path) const
    {
        FUZZ_ASSERT(QDir::isAbsolutePath(path));
        FUZZ_ASSERT(QDir::cleanPath(path) == path);
        FUZZ_ASSERT(under(path, root) || under(path, canonical));
        for (QString probe = path;;) {
            const QFileInfo fi(probe);
            if (fi.exists()) {
                FUZZ_ASSERT(under(fi.canonicalFilePath(), canonical));
                break;
            }
            FUZZ_ASSERT(!fi.isSymLink()); // a link pointing nowhere must have been refused
            const QString parent = fi.path();
            if (parent == probe) break;
            probe = parent;
        }
    }
};

} // namespace Fuzz
