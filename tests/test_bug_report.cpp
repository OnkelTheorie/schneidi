// SPDX-License-Identifier: AGPL-3.0-only
// Help › Report a bug… (app/BugReport.h): the menu entry exists, the address opens the bug form with version, system
// and log folder filled in, special characters are encoded, the home folder never appears in it, long values are cut.
#include "check.h"

#include "app/BugReport.h"
#include "app/MainWindow.h"
#include "core/I18n.h"
#include "engine/Engine.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QUrlQuery>

#include <clocale>

namespace {

void testUrl()
{
    BugReport::Facts f;
    f.version = "0.25.0 (abc1234+dirty)";
    f.os = "Debian GNU/Linux 13 (trixie)";
    f.kernel = "linux 6.12.117";
    f.cpuArch = "x86_64";
    f.gpu = "NVIDIA GeForce GTX 1050 Ti & #2";
    f.gpuApi = "Vulkan";
    f.language = "de";
    f.logDir = "~/.local/share/schneidi/logs";
    const QUrl url = BugReport::issueUrl(f);
    CHECK(url.isValid());
    CHECK_EQ(url.scheme(), QString("https"));
    CHECK_EQ(url.host(), QString("github.com"));
    CHECK_EQ(url.path(), QString("/OnkelTheorie/schneidi/issues/new"));
    const QString encoded = url.toString(QUrl::FullyEncoded);
    CHECK(encoded.size() < 6000);
    CHECK(!encoded.contains(' ') && !encoded.contains("&#") && !encoded.contains("+dirty"));
    const QUrlQuery q(url);
    CHECK_EQ(q.queryItemValue("template"), QString("bug_report.yml"));
    CHECK_EQ(q.queryItemValue("version", QUrl::FullyDecoded), f.version);
    const QString system = q.queryItemValue("system", QUrl::FullyDecoded);
    CHECK(system.contains("Debian GNU/Linux 13") && system.contains("6.12.117") && system.contains("x86_64"));
    CHECK(system.contains("GPU: NVIDIA GeForce GTX 1050 Ti & #2 (Vulkan)") && system.contains("UI language: de"));
    CHECK(q.queryItemValue("log", QUrl::FullyDecoded).contains("~/.local/share/schneidi/logs"));

    // A very long value (a strange driver name) keeps the address short
    f.gpu = QString(20000, 'x');
    CHECK(BugReport::issueUrl(f).toString(QUrl::FullyEncoded).size() < 6000);
}

void testShortHome()
{
#ifdef Q_OS_WIN
    CHECK_EQ(BugReport::shortHome("C:\\Users\\Anna\\AppData\\Roaming\\schneidi\\logs", "C:/Users/Anna"),
             QString("%USERPROFILE%\\AppData\\Roaming\\schneidi\\logs"));
    CHECK_EQ(BugReport::shortHome("c:/users/anna/x", "C:/Users/Anna"), QString("%USERPROFILE%\\x"));
#else
    CHECK_EQ(BugReport::shortHome("/home/anna/.local/share/schneidi/logs", "/home/anna"),
             QString("~/.local/share/schneidi/logs"));
    CHECK_EQ(BugReport::shortHome("/home/anna", "/home/anna"), QString("~"));
    CHECK_EQ(BugReport::shortHome("/home/annabel/logs", "/home/anna"), QString("/home/annabel/logs")); // not a prefix
#endif
}

// What this machine sends: never the home folder
void testCollect()
{
    const BugReport::Facts f = BugReport::collect();
    CHECK(!f.version.isEmpty() && !f.os.isEmpty());
    const QString encoded = BugReport::issueUrl(f).toString(QUrl::PrettyDecoded);
    CHECK(!encoded.contains(QDir::homePath()));
    CHECK(!encoded.contains(QDir::toNativeSeparators(QDir::homePath())));
    std::printf("URL: %s\n", BugReport::issueUrl(f).toString(QUrl::FullyEncoded).toUtf8().constData());
}

void testMenu(MainWindow& w)
{
    QMenu* help = nullptr;
    for (QAction* a : w.menuBar()->actions())
        if (a->text() == "&Hilfe") help = a->menu();
    if (!CHECK(help)) return;
    bool found = false;
    for (QAction* a : help->actions()) found |= a->text() == QString::fromUtf8("Fehler melden…");
    CHECK(found);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QTemporaryDir tmp;
    qputenv("HOME", tmp.path().toUtf8());
    QApplication app(argc, argv);
    Check::initApp("bug-report");
    QCoreApplication::setApplicationVersion("0.25.0 (test)");
    I18n::install("de");
    testUrl();
    testShortHome();
    testCollect();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    testMenu(w);
    return Check::result();
}
