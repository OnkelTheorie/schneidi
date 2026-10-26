#include "app/UpdateCheck.h"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>

namespace {

constexpr const char* kLatest = "https://api.github.com/repos/OnkelTheorie/schneidi/releases/latest";
constexpr const char* kReleases = "https://github.com/OnkelTheorie/schneidi/releases/latest";
constexpr int kTimeoutMs = 10000;

struct Version {
    QList<int> parts; // major, minor, patch (missing = 0)
    QString suffix;   // "rc1" of "0.12.0-rc1": before the release itself
};

std::optional<Version> parse(const QString& text)
{
    static const QRegularExpression re(R"(^\s*v?(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:-([0-9A-Za-z.\-]+))?(?:\+\S*)?\s*$)");
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return std::nullopt;
    Version v;
    for (int i = 1; i <= 3; ++i) v.parts << m.captured(i).toInt();
    v.suffix = m.captured(4);
    return v;
}

} // namespace

UpdateCheck::UpdateCheck(QObject* parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

UpdateCheck::~UpdateCheck()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
}

QString UpdateCheck::runningVersion(const QString& appVersion)
{
    QString v = appVersion.section(" (", 0, 0).trimmed(); // without the build id
    if (v.contains(QStringLiteral("→"))) v = v.section(QStringLiteral("→"), 1).trimmed();
    return v;
}

std::optional<int> UpdateCheck::compareVersions(const QString& a, const QString& b)
{
    const auto va = parse(a), vb = parse(b);
    if (!va || !vb) return std::nullopt;
    for (int i = 0; i < 3; ++i)
        if (va->parts[i] != vb->parts[i]) return va->parts[i] < vb->parts[i] ? -1 : 1;
    if (va->suffix == vb->suffix) return 0;
    if (va->suffix.isEmpty()) return 1; // 0.12.0 > 0.12.0-rc1
    if (vb->suffix.isEmpty()) return -1;
    return va->suffix < vb->suffix ? -1 : 1;
}

std::optional<UpdateCheck::Release> UpdateCheck::parseRelease(const QByteArray& json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    const QString tag = o.value("tag_name").toString();
    if (!parse(tag)) return std::nullopt;
    Release r;
    r.version = tag.trimmed().startsWith('v') ? tag.trimmed().mid(1) : tag.trimmed();
    r.page = QUrl(o.value("html_url").toString());
    // Only a web page is opened, whatever the answer says
    if (!r.page.isValid() || (r.page.scheme() != "https" && r.page.scheme() != "http")) r.page = QUrl(kReleases);
    return r;
}

QUrl UpdateCheck::url()
{
    const QString custom = qEnvironmentVariable("SCHNEIDI_UPDATE_URL");
    return QUrl(custom.isEmpty() ? QString(kLatest) : custom);
}

bool UpdateCheck::enabled() { return QSettings().value("update/check", true).toBool(); }

void UpdateCheck::setEnabled(bool on) { QSettings().setValue("update/check", on); }

bool UpdateCheck::due(const QDateTime& now)
{
    const QDateTime last = QDateTime::fromString(QSettings().value("update/lastCheck").toString(), Qt::ISODate);
    // Never checked, a day ago or more, or the clock went back
    return !last.isValid() || last.secsTo(now) >= 24 * 3600 || last > now;
}

void UpdateCheck::markChecked(const QDateTime& when)
{
    QSettings().setValue("update/lastCheck", when.toUTC().toString(Qt::ISODate));
}

bool UpdateCheck::allowedHere()
{
    return QGuiApplication::platformName() != QLatin1String("offscreen")
           && qEnvironmentVariableIsEmpty("SCHNEIDI_NO_UPDATE_CHECK");
}

void UpdateCheck::check(const QString& currentVersion)
{
    if (m_reply) return;
    m_current = currentVersion;
    QNetworkRequest request(url());
    request.setHeader(QNetworkRequest::UserAgentHeader, "schneidi/" + currentVersion); // GitHub refuses requests without
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setTransferTimeout(kTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_net->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateCheck::onFinished);
}

void UpdateCheck::onFinished()
{
    QNetworkReply* r = m_reply;
    if (!r) return;
    m_reply = nullptr;
    r->deleteLater();
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (r->error() != QNetworkReply::NoError) {
        // 403/429 = GitHub's limit of requests per hour and address
        const bool limit = status == 403 || status == 429;
        qInfo("Update check failed: %s (HTTP %d)", qPrintable(r->errorString()), status);
        emit failed(limit ? QStringLiteral("GitHub: too many requests, try again later") : r->errorString());
        return;
    }
    const std::optional<Release> release = parseRelease(r->readAll());
    const std::optional<int> order = release ? compareVersions(release->version, m_current) : std::nullopt;
    if (!order) {
        qInfo("Update check: unexpected answer from %s (this version: %s)", qPrintable(r->url().toString()),
              qPrintable(m_current));
        emit failed(QStringLiteral("unexpected answer"));
        return;
    }
    qInfo("Update check: latest release %s, this is %s", qPrintable(release->version), qPrintable(m_current));
    if (*order > 0) emit newer(*release, m_current);
    else emit upToDate(m_current);
}
