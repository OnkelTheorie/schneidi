// Update hint (app/UpdateCheck): the version this build counts as (a development build = the version being worked
// on), comparing versions, reading GitHub's answer; against a local HTTP server (SCHNEIDI_UPDATE_URL): newer, same,
// older, garbage, 404 and GitHub's rate limit, with a User-Agent and GitHub's Accept header; at most once a day. The
// main window shows its bar only for a newer version ("Later" / "Don't ask again" hide it, a test run keeps the
// setting), a manual check says "up to date", Settings › General "Check now" shows the answer.
// UPDATE_DUMP=<folder>: the main window with the bar (update-bar.png).

#include "check.h"

#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "app/Theme.h"
#include "app/UpdateCheck.h"
#include "core/I18n.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkProxyFactory>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QToolButton>
#include <clocale>
#include <functional>

namespace {

// Answers every request with `status` and `body`, remembers the last request
class FakeGitHub : public QObject {
public:
    QTcpServer server;
    int status = 200;
    QByteArray body;
    QByteArray request;
    int requests = 0;

    FakeGitHub()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                auto* buffer = new QByteArray;
                connect(s, &QTcpSocket::disconnected, s, [s, buffer] {
                    delete buffer;
                    s->deleteLater();
                });
                connect(s, &QTcpSocket::readyRead, s, [this, s, buffer] {
                    *buffer += s->readAll();
                    if (!buffer->contains("\r\n\r\n")) return;
                    request = *buffer;
                    ++requests;
                    s->write(QString("HTTP/1.1 %1 X\r\nContent-Type: application/json\r\nContent-Length: %2\r\n"
                                     "Connection: close\r\n\r\n")
                                 .arg(status)
                                 .arg(body.size())
                                 .toLatin1()
                             + body);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QByteArray url() const
    {
        return QString("http://127.0.0.1:%1/repos/OnkelTheorie/schneidi/releases/latest").arg(server.serverPort()).toUtf8();
    }
    void release(const QString& tag)
    {
        status = 200;
        body = QString(R"({"tag_name":"%1","html_url":"https://github.com/OnkelTheorie/schneidi/releases/tag/%1",)"
                       R"("draft":false,"prerelease":false})")
                   .arg(tag)
                   .toUtf8();
    }
};

bool waitFor(const std::function<bool()>& done, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

void testVersions()
{
    CHECK_EQ(UpdateCheck::runningVersion("0.11.0 → 0.12.0 (abc1234+)"), QString("0.12.0"));
    CHECK_EQ(UpdateCheck::runningVersion("0.12.0 (abc1234)"), QString("0.12.0"));
    CHECK_EQ(UpdateCheck::runningVersion("0.12.0"), QString("0.12.0"));
    const auto cmp = [](const char* a, const char* b) { return UpdateCheck::compareVersions(a, b).value_or(99); };
    CHECK_EQ(cmp("v0.12.1", "0.12.0"), 1);
    CHECK_EQ(cmp("0.12.0", "v0.12.0"), 0);
    CHECK_EQ(cmp("0.11.0", "0.12.0"), -1);   // a 0.12 development build never offers 0.11
    CHECK_EQ(cmp("0.10.0", "0.9.9"), 1);     // numbers, not text
    CHECK_EQ(cmp("1.0", "0.99.9"), 1);
    CHECK_EQ(cmp("0.12.0-rc1", "0.12.0"), -1); // a release candidate comes before the release
    CHECK_EQ(cmp("0.12.0", "0.12.0-rc1"), 1);
    CHECK_EQ(cmp("0.12.0-rc2", "0.12.0-rc1"), 1);
    CHECK(!UpdateCheck::compareVersions("nightly", "0.12.0"));
    CHECK(!UpdateCheck::compareVersions("0.12.0", ""));

    const auto r = UpdateCheck::parseRelease(R"({"tag_name":"v0.12.1","html_url":"https://github.com/x/y/releases/tag/v0.12.1"})");
    if (CHECK(r)) {
        CHECK_EQ(r->version, QString("0.12.1"));
        CHECK_EQ(r->page, QUrl("https://github.com/x/y/releases/tag/v0.12.1"));
    }
    CHECK(!UpdateCheck::parseRelease("<html>rate limited</html>"));
    CHECK(!UpdateCheck::parseRelease(R"({"message":"Not Found"})"));
    // Only a web page is opened, whatever the answer says
    const auto odd = UpdateCheck::parseRelease(R"({"tag_name":"0.13.0","html_url":"file:///etc/passwd"})");
    CHECK(odd && odd->page.scheme() == "https");

    // At most once a day
    QSettings().remove("update");
    CHECK(UpdateCheck::enabled());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    CHECK(UpdateCheck::due(now));
    UpdateCheck::markChecked(now.addSecs(-3600));
    CHECK(!UpdateCheck::due(now));
    UpdateCheck::markChecked(now.addSecs(-25 * 3600));
    CHECK(UpdateCheck::due(now));
    UpdateCheck::markChecked(now.addDays(3)); // clock went back
    CHECK(UpdateCheck::due(now));
    QSettings().remove("update");
    CHECK(!UpdateCheck::allowedHere()); // tests run offscreen: never automatically
}

void testRequests(FakeGitHub& gh)
{
    UpdateCheck check;
    QString result;
    QObject::connect(&check, &UpdateCheck::newer, [&](const UpdateCheck::Release& r, const QString&) { result = "newer " + r.version; });
    QObject::connect(&check, &UpdateCheck::upToDate, [&](const QString& v) { result = "current " + v; });
    QObject::connect(&check, &UpdateCheck::failed, [&](const QString& m) { result = "failed " + m; });
    const auto ask = [&](const QString& current) {
        result.clear();
        check.check(current);
        CHECK(check.running());
        waitFor([&] { return !result.isEmpty(); });
        CHECK(!check.running());
        return result;
    };
    gh.release("v0.13.0");
    CHECK_EQ(ask("0.12.0"), QString("newer 0.13.0"));
    CHECK(gh.request.startsWith("GET /repos/OnkelTheorie/schneidi/releases/latest"));
    CHECK(gh.request.toLower().contains("user-agent: schneidi/0.12.0"));
    CHECK(gh.request.toLower().contains("accept: application/vnd.github+json"));
    gh.release("v0.12.0");
    CHECK_EQ(ask("0.12.0"), QString("current 0.12.0"));
    gh.release("v0.11.0"); // a development build of 0.12 while 0.11 is the latest release
    CHECK_EQ(ask("0.12.0"), QString("current 0.12.0"));
    gh.status = 200;
    gh.body = "not json at all";
    CHECK(ask("0.12.0").startsWith("failed"));
    gh.status = 404;
    gh.body = R"({"message":"Not Found"})";
    CHECK(ask("0.12.0").startsWith("failed"));
    gh.status = 403;
    gh.body = R"({"message":"API rate limit exceeded"})";
    CHECK(ask("0.12.0").contains("too many requests"));
}

void testMainWindow(FakeGitHub& gh)
{
    QSettings().remove("update");
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    auto* bar = w.findChild<QWidget*>("update_bar");
    auto* later = w.findChild<QToolButton*>("update_later");
    auto* never = w.findChild<QToolButton*>("update_never");
    auto* check = w.findChild<UpdateCheck*>();
    if (!CHECK(bar && later && never && check)) return;
    CHECK(bar->isHidden());

    // Newer: the bar with both versions
    gh.release("v99.0.0");
    w.checkForUpdates(false);
    CHECK(waitFor([&] { return !bar->isHidden(); }));
    auto* text = bar->findChild<QLabel*>();
    CHECK(text && text->text().contains("99.0.0"));
    if (const QString dump = qEnvironmentVariable("UPDATE_DUMP"); !dump.isEmpty()) {
        QApplication::processEvents();
        w.grab(QRect(0, 0, w.width(), 160)).save(dump + "/update-bar.png");
    }
    later->click();
    CHECK(bar->isHidden());
    w.checkForUpdates(false);
    CHECK(waitFor([&] { return !bar->isHidden(); }));
    never->click();
    CHECK(bar->isHidden());
    CHECK(UpdateCheck::enabled()); // a test run keeps the setting
    CHECK(!QSettings().contains("update/lastCheck"));

    // Same version: no bar; asked by hand it says so
    gh.release("v0.0.1");
    const int before = gh.requests;
    w.checkForUpdates(false);
    CHECK(waitFor([&] { return gh.requests > before && !check->running(); }));
    QApplication::processEvents();
    CHECK(bar->isHidden());
    CHECK(!w.findChild<QMessageBox*>("update_message"));
    w.checkForUpdates(true);
    CHECK(waitFor([&] { return w.findChild<QMessageBox*>("update_message") != nullptr; }));
    if (auto* box = w.findChild<QMessageBox*>("update_message")) {
        CHECK_EQ(box->text(), T("schneidi ist aktuell (%1).").arg("0.12.0"));
        box->close();
    }
    // No answer at all (nothing listens there): silent when automatic
    qputenv("SCHNEIDI_UPDATE_URL", "http://127.0.0.1:1/nothing");
    w.checkForUpdates(false);
    CHECK(waitFor([&] { return !check->running(); }));
    QApplication::processEvents();
    CHECK(bar->isHidden());
    CHECK(!w.findChild<QMessageBox*>("update_message"));
    qputenv("SCHNEIDI_UPDATE_URL", gh.url());

    // Settings › General: "Check now" shows the answer in the dialog
    gh.release("v99.0.0");
    SettingsDialog dlg(AiSettings{});
    auto* now = dlg.findChild<QPushButton*>("update_check_now");
    auto* answer = dlg.findChild<QLabel*>("update_result");
    if (CHECK(now && answer)) {
        now->click();
        CHECK(waitFor([&] { return answer->text().contains("99.0.0"); }));
        CHECK(answer->text().contains("href"));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("update-check");
    QApplication::setApplicationVersion("0.11.0 → 0.12.0 (test)"); // a development build of 0.12
    Theme::apply(app);
    I18n::install("de");
    QNetworkProxyFactory::setUseSystemConfiguration(false); // the local server, never a proxy
    FakeGitHub gh;
    qputenv("SCHNEIDI_UPDATE_URL", gh.url());
    testVersions();
    testRequests(gh);
    testMainWindow(gh);
    QSettings().remove("update");
    return Check::result();
}
