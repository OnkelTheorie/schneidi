// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Update hint: asks GitHub for the latest release and reports a version newer than this one. The main window checks
// at most once a day, a few seconds after the start (MainWindowSettings.cpp), and on Help › Check for Updates…;
// never in test runs (--screenshot, --demo, --actions, offscreen platform) or with SCHNEIDI_NO_UPDATE_CHECK set.
// No network, errors and rate limits only go to the log. Until there is an apt repository (0.16) every system gets
// the release page. SCHNEIDI_UPDATE_URL replaces the address (tests serve their own answer).

#include <QDateTime>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <optional>

class QNetworkAccessManager;

class UpdateCheck : public QObject {
    Q_OBJECT
public:
    struct Release {
        QString version; // "0.12.1" (tag without the "v")
        QUrl page;       // release page with the downloads
    };

    explicit UpdateCheck(QObject* parent = nullptr);
    ~UpdateCheck() override;

    // Version this program counts as: the one being worked on for a development build ("0.11.0 → 0.12.0 (abc1234)"
    // -> "0.12.0", so a 0.12 build never offers 0.11.x), else the release ("0.12.0 (abc1234)" -> "0.12.0")
    static QString runningVersion(const QString& appVersion);
    // a < b: negative, equal: 0, a > b: positive; "v" in front is fine, a suffix ("-rc1", "-dev") counts lower than
    // the release itself. nullopt = one of them is not a version
    static std::optional<int> compareVersions(const QString& a, const QString& b);
    // Answer of /releases/latest: tag_name and html_url; nullopt = not such an answer
    static std::optional<Release> parseRelease(const QByteArray& json);
    static QUrl url(); // GitHub's latest release, or SCHNEIDI_UPDATE_URL

    // Settings (QSettings "update/…"): hint on/off (default on), when it was last checked
    static bool enabled();
    static void setEnabled(bool on);
    static bool due(const QDateTime& now = QDateTime::currentDateTimeUtc()); // last check more than a day ago
    static void markChecked(const QDateTime& when = QDateTime::currentDateTimeUtc());
    // Asking is possible in this run at all (not offscreen, no SCHNEIDI_NO_UPDATE_CHECK)
    static bool allowedHere();

    // Asks once (asynchronous; a running check is not started twice); the answer comes as one of the signals
    void check(const QString& currentVersion);
    bool running() const { return m_reply != nullptr; }

signals:
    void newer(const UpdateCheck::Release& release, const QString& current);
    void upToDate(const QString& current);
    void failed(const QString& message); // for the user only when they asked (English details go to the log)

private:
    void onFinished();

    QNetworkAccessManager* m_net;
    QPointer<QNetworkReply> m_reply;
    QString m_current;
};
