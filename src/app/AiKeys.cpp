#include "app/AiKeys.h"

#include "app/AiProvider.h"
#include "core/I18n.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

#ifdef SCHNEIDI_HAVE_DBUS
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QEventLoop>
#include <QTimer>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace Ai {

// ---------- Company file ----------

Company readCompany(const QString& file)
{
    Company c;
    c.file = file;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        c.error = T("nicht lesbar");
        return c;
    }
    QJsonParseError err;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &err).object();
    if (err.error != QJsonParseError::NoError) {
        c.error = T("kein gültiges JSON: %1").arg(err.errorString());
        return c;
    }
    c.provider = o["provider"].toString().trimmed().toLower();
    if (c.provider == "anthropic") c.provider = "claude";
    if (!c.provider.isEmpty() && !preset(c.provider)) c.error = T("unbekannter Anbieter: %1").arg(c.provider);
    c.address = o[o.contains("address") ? "address" : "base_url"].toString().trimmed();
    c.model = o["model"].toString().trimmed();
    c.locked = o["locked"].toBool();
    c.key = o["key"].toString().trimmed();
    if (c.key.isEmpty() && o.contains("key_file")) {
        const QString path = QDir(QFileInfo(file).absolutePath()).absoluteFilePath(o["key_file"].toString().trimmed());
        QFile k(path);
        if (k.open(QIODevice::ReadOnly)) c.key = QString::fromUtf8(k.readAll()).trimmed();
        else c.error = T("Schlüssel-Datei nicht lesbar: %1").arg(QDir::toNativeSeparators(path));
    }
    return c;
}

Company company(const QStringList& dirs)
{
    QStringList files;
    if (dirs.isEmpty()) {
        files = QStandardPaths::locateAll(QStandardPaths::AppConfigLocation, CompanyFileName);
    } else {
        for (const QString& d : dirs)
            if (const QString f = QDir(d).filePath(CompanyFileName); QFileInfo::exists(f)) files << f;
    }
    // System-wide last in the list: read it first, closer files fill in or replace (a locked one ends it)
    Company out;
    for (auto it = files.crbegin(); it != files.crend(); ++it) {
        const Company c = readCompany(*it);
        if (c.locked && c.error.isEmpty()) return c;
        out.file = c.file;
        if (!c.error.isEmpty()) out.error = c.error;
        for (auto [field, value] : {std::pair{&out.provider, c.provider}, std::pair{&out.address, c.address},
                                    std::pair{&out.model, c.model}, std::pair{&out.key, c.key}})
            if (!value.isEmpty()) *field = value;
    }
    return out;
}

// ---------- Stores ----------

namespace {

const QString kSettingsGroup = QStringLiteral("ai/chat/keys/");
// "A key of this provider is in the keychain" (no secret): showing it never asks the keychain, which could start its
// service or a password prompt at the start of the app
const QString kInKeychain = QStringLiteral("ai/chat/inKeychain/");

class SettingsStore : public KeyStore {
public:
    QString name() const override { return T("Einstellungen (unverschlüsselt)"); }
    bool secure() const override { return false; }
    bool has(const QString& provider) override { return !QSettings().value(kSettingsGroup + provider).toString().isEmpty(); }
    QString read(const QString& provider, QString*) override { return QSettings().value(kSettingsGroup + provider).toString(); }
    bool write(const QString& provider, const QString& key, QString*) override
    {
        QSettings().setValue(kSettingsGroup + provider, key);
        return true;
    }
    bool remove(const QString& provider, QString*) override
    {
        QSettings().remove(kSettingsGroup + provider);
        return true;
    }
};

#ifdef Q_OS_WIN
// Windows Credential Manager ("Anmeldeinformationsverwaltung"), generic credentials "schneidi/ai/<provider>"
class WindowsStore : public KeyStore {
public:
    QString name() const override { return T("Windows-Anmeldeinformationsverwaltung"); }
    bool secure() const override { return true; }
    bool has(const QString& provider) override
    {
        PCREDENTIALW cred = nullptr;
        const std::wstring target = targetOf(provider);
        if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) return false;
        CredFree(cred);
        return true;
    }
    QString read(const QString& provider, QString* error) override
    {
        PCREDENTIALW cred = nullptr;
        const std::wstring target = targetOf(provider);
        if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
            if (GetLastError() != ERROR_NOT_FOUND && error) *error = T("Schlüssel nicht lesbar (Fehler %1)").arg(GetLastError());
            return {};
        }
        const QString key = QString::fromUtf8(reinterpret_cast<const char*>(cred->CredentialBlob), int(cred->CredentialBlobSize));
        CredFree(cred);
        return key;
    }
    bool write(const QString& provider, const QString& key, QString* error) override
    {
        std::wstring target = targetOf(provider);
        std::wstring user = L"schneidi";
        QByteArray blob = key.toUtf8();
        CREDENTIALW c{};
        c.Type = CRED_TYPE_GENERIC;
        c.TargetName = target.data();
        c.UserName = user.data();
        c.CredentialBlobSize = DWORD(blob.size());
        c.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (CredWriteW(&c, 0)) return true;
        if (error) *error = T("Schlüssel nicht gespeichert (Fehler %1)").arg(GetLastError());
        return false;
    }
    bool remove(const QString& provider, QString* error) override
    {
        const std::wstring target = targetOf(provider);
        if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) return true;
        if (error) *error = T("Schlüssel nicht gelöscht (Fehler %1)").arg(GetLastError());
        return false;
    }

private:
    static std::wstring targetOf(const QString& provider) { return QString("schneidi/ai/%1").arg(provider).toStdWString(); }
};
#endif

} // namespace

#ifdef SCHNEIDI_HAVE_DBUS
} // namespace Ai

// Secret Service (freedesktop.org): GNOME Keyring, KWallet, KeePassXC … over the session bus
namespace AiSecret {

struct Secret {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

QDBusArgument& operator<<(QDBusArgument& a, const Secret& s)
{
    a.beginStructure();
    a << s.session << s.parameters << s.value << s.contentType;
    a.endStructure();
    return a;
}

const QDBusArgument& operator>>(const QDBusArgument& a, Secret& s)
{
    a.beginStructure();
    a >> s.session >> s.parameters >> s.value >> s.contentType;
    a.endStructure();
    return a;
}

using Attributes = QMap<QString, QString>;

} // namespace AiSecret

Q_DECLARE_METATYPE(AiSecret::Secret)

namespace Ai {

namespace {

const QString kService = QStringLiteral("org.freedesktop.secrets");
const QString kServicePath = QStringLiteral("/org/freedesktop/secrets");
const QString kServiceIface = QStringLiteral("org.freedesktop.Secret.Service");

class SecretServiceStore : public KeyStore {
public:
    SecretServiceStore() : m_bus(QDBusConnection::sessionBus())
    {
        qDBusRegisterMetaType<AiSecret::Secret>();
        qDBusRegisterMetaType<AiSecret::Attributes>();
    }

    bool available()
    {
        if (!m_bus.isConnected() || !m_bus.interface()) return false;
        if (m_bus.interface()->isServiceRegistered(kService)) return true;
        const QDBusMessage r = m_bus.call(QDBusMessage::createMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                                         "org.freedesktop.DBus", "ListActivatableNames"),
                                          QDBus::Block, 3000);
        return r.type() == QDBusMessage::ReplyMessage && r.arguments().value(0).toStringList().contains(kService);
    }

    QString name() const override { return T("Schlüsselbund des Systems (Secret Service)"); }
    bool secure() const override { return true; }

    bool has(const QString& provider) override
    {
        const auto [unlocked, locked] = search(provider);
        return !unlocked.isEmpty() || !locked.isEmpty();
    }

    QString read(const QString& provider, QString* error) override
    {
        auto [unlocked, locked] = search(provider);
        if (unlocked.isEmpty() && !locked.isEmpty()) {
            unlock(locked);
            unlocked = search(provider).first;
            if (unlocked.isEmpty()) {
                if (error) *error = T("Der Schlüsselbund ist gesperrt.");
                return {};
            }
        }
        if (unlocked.isEmpty() || !openSession(error)) return {};
        QDBusMessage m = QDBusMessage::createMethodCall(kService, unlocked.first().path(), "org.freedesktop.Secret.Item",
                                                        "GetSecret");
        m << QVariant::fromValue(m_session);
        const QDBusMessage r = m_bus.call(m, QDBus::Block, 10000);
        if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty()) {
            if (error) *error = r.errorMessage();
            return {};
        }
        const auto secret = qdbus_cast<AiSecret::Secret>(r.arguments().first());
        return QString::fromUtf8(secret.value);
    }

    bool write(const QString& provider, const QString& key, QString* error) override
    {
        if (!openSession(error)) return false;
        QDBusMessage alias = QDBusMessage::createMethodCall(kService, kServicePath, kServiceIface, "ReadAlias");
        alias << QString("default");
        const QDBusMessage ar = m_bus.call(alias, QDBus::Block, 10000);
        const QDBusObjectPath collection = ar.arguments().value(0).value<QDBusObjectPath>();
        if (ar.type() != QDBusMessage::ReplyMessage || collection.path().isEmpty() || collection.path() == "/") {
            if (error) *error = T("Kein Standard-Schlüsselbund vorhanden.");
            return false;
        }
        unlock({collection});
        QVariantMap props;
        props.insert("org.freedesktop.Secret.Item.Label", T("schneidi: KI-Schlüssel (%1)").arg(presetName(provider)));
        props.insert("org.freedesktop.Secret.Item.Attributes", QVariant::fromValue(attributes(provider)));
        const AiSecret::Secret secret{m_session, {}, key.toUtf8(), "text/plain; charset=utf8"};
        QDBusMessage m = QDBusMessage::createMethodCall(kService, collection.path(), "org.freedesktop.Secret.Collection",
                                                        "CreateItem");
        m << props << QVariant::fromValue(secret) << true;
        const QDBusMessage r = m_bus.call(m, QDBus::Block, 10000);
        if (r.type() != QDBusMessage::ReplyMessage) {
            if (error) *error = r.errorMessage();
            return false;
        }
        if (const QDBusObjectPath prompt = r.arguments().value(1).value<QDBusObjectPath>(); prompt.path() != "/")
            runPrompt(prompt);
        if (has(provider)) return true;
        if (error) *error = T("Der Schlüsselbund hat den Schlüssel nicht angenommen.");
        return false;
    }

    bool remove(const QString& provider, QString* error) override
    {
        const auto [unlocked, locked] = search(provider);
        for (const QDBusObjectPath& item : unlocked + locked) {
            const QDBusMessage r = m_bus.call(
                QDBusMessage::createMethodCall(kService, item.path(), "org.freedesktop.Secret.Item", "Delete"),
                QDBus::Block, 10000);
            if (r.type() != QDBusMessage::ReplyMessage) {
                if (error) *error = r.errorMessage();
                return false;
            }
            if (const QDBusObjectPath prompt = r.arguments().value(0).value<QDBusObjectPath>(); prompt.path() != "/")
                runPrompt(prompt);
        }
        return !has(provider);
    }

private:
    static AiSecret::Attributes attributes(const QString& provider)
    {
        return {{"application", "schneidi"}, {"purpose", "ai-chat"}, {"provider", provider}};
    }

    std::pair<QList<QDBusObjectPath>, QList<QDBusObjectPath>> search(const QString& provider)
    {
        QDBusMessage m = QDBusMessage::createMethodCall(kService, kServicePath, kServiceIface, "SearchItems");
        m << QVariant::fromValue(attributes(provider));
        const QDBusMessage r = m_bus.call(m, QDBus::Block, 10000);
        if (r.type() != QDBusMessage::ReplyMessage || r.arguments().size() < 2) return {};
        return {qdbus_cast<QList<QDBusObjectPath>>(r.arguments().at(0)), qdbus_cast<QList<QDBusObjectPath>>(r.arguments().at(1))};
    }

    bool openSession(QString* error)
    {
        if (!m_session.path().isEmpty()) return true;
        QDBusMessage m = QDBusMessage::createMethodCall(kService, kServicePath, kServiceIface, "OpenSession");
        m << QString("plain") << QVariant::fromValue(QDBusVariant(QString()));
        const QDBusMessage r = m_bus.call(m, QDBus::Block, 10000);
        if (r.type() != QDBusMessage::ReplyMessage || r.arguments().size() < 2) {
            if (error) *error = r.errorMessage();
            return false;
        }
        m_session = r.arguments().at(1).value<QDBusObjectPath>();
        return !m_session.path().isEmpty();
    }

    void unlock(const QList<QDBusObjectPath>& objects)
    {
        QDBusMessage m = QDBusMessage::createMethodCall(kService, kServicePath, kServiceIface, "Unlock");
        m << QVariant::fromValue(objects);
        const QDBusMessage r = m_bus.call(m, QDBus::Block, 10000);
        if (r.type() != QDBusMessage::ReplyMessage) return;
        if (const QDBusObjectPath prompt = r.arguments().value(1).value<QDBusObjectPath>(); prompt.path() != "/")
            runPrompt(prompt);
    }

    // The keychain asks the user (password, confirmation): wait for its answer, at most two minutes
    void runPrompt(const QDBusObjectPath& prompt)
    {
        QEventLoop loop;
        m_bus.connect(kService, prompt.path(), "org.freedesktop.Secret.Prompt", "Completed", &loop, SLOT(quit()));
        QDBusMessage m = QDBusMessage::createMethodCall(kService, prompt.path(), "org.freedesktop.Secret.Prompt", "Prompt");
        m << QString();
        if (m_bus.call(m, QDBus::Block, 10000).type() == QDBusMessage::ReplyMessage) {
            QTimer::singleShot(120000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        m_bus.disconnect(kService, prompt.path(), "org.freedesktop.Secret.Prompt", "Completed", &loop, SLOT(quit()));
    }

    QDBusConnection m_bus;
    QDBusObjectPath m_session;
};

} // namespace
#endif

std::unique_ptr<KeyStore> secretServiceStore()
{
#ifdef SCHNEIDI_HAVE_DBUS
    auto s = std::make_unique<SecretServiceStore>();
    if (s->available()) return s;
#endif
    return nullptr;
}

std::unique_ptr<KeyStore> systemKeyStore()
{
    if (QStandardPaths::isTestModeEnabled()) return nullptr;
#ifdef Q_OS_WIN
    return std::make_unique<WindowsStore>();
#else
    return secretServiceStore();
#endif
}

std::unique_ptr<KeyStore> settingsKeyStore() { return std::make_unique<SettingsStore>(); }

// ---------- Resolving ----------

QStringList keyVariables(const QString& provider)
{
    QStringList vars{"SCHNEIDI_AI_KEY"};
    if (const Preset* p = preset(provider); p && !p->keyEnv.isEmpty()) vars << p->keyEnv;
    return vars;
}

Choice choose(const QString& provider, const QString& address, const QHash<QString, QString>& models, const Company& c)
{
    Choice ch;
    const bool usable = c.found() && c.error.isEmpty();
    ch.locked = usable && c.locked;
    if (ch.locked) {
        ch.provider = c.provider.isEmpty() ? provider : c.provider;
        ch.address = c.address;
    } else {
        ch.provider = provider.isEmpty() && usable ? c.provider : provider;
        ch.address = ch.provider == provider ? address : QString();
    }
    const bool companyFits = usable && (c.provider.isEmpty() || c.provider == ch.provider);
    if (ch.address.isEmpty() && companyFits) ch.address = c.address;
    const Preset* p = preset(ch.provider);
    if (ch.address.isEmpty() && p) ch.address = p->address;
    ch.model = models.value(ch.provider);
    if (ch.model.isEmpty() && companyFits) ch.model = c.model;
    if (ch.model.isEmpty() && p) ch.model = p->model;
    return ch;
}

Keys::Keys() : Keys(systemKeyStore(), settingsKeyStore()) {}

Keys::Keys(std::unique_ptr<KeyStore> keychain, std::unique_ptr<KeyStore> settings, Env env)
    : m_keychain(std::move(keychain)), m_settings(std::move(settings)), m_env(std::move(env))
{
    if (!m_env) m_env = [](const QString& name) { return qEnvironmentVariable(name.toLatin1().constData()).trimmed(); };
}

Keys::~Keys() = default;

KeySource Keys::resolve(const QString& provider, const Company& company)
{
    for (const QString& var : keyVariables(provider))
        if (const QString key = m_env(var); !key.isEmpty()) return {KeySource::Kind::Environment, var, key};
    if (company.found() && !company.key.isEmpty() && (company.provider.isEmpty() || company.provider == provider))
        return {KeySource::Kind::Company, company.file, company.key};
    if (const auto it = m_cache.constFind(provider); it != m_cache.cend())
        return {it->kind, it->kind == KeySource::Kind::Keychain ? m_keychain->name() : m_settings->name(), it->key};
    QString error;
    if (m_keychain && m_keychain->has(provider)) {
        if (const QString key = m_keychain->read(provider, &error); !key.isEmpty()) {
            m_cache.insert(provider, {key, KeySource::Kind::Keychain});
            QSettings().setValue(kInKeychain + provider, true);
            return {KeySource::Kind::Keychain, m_keychain->name(), key};
        }
    } else {
        QSettings().remove(kInKeychain + provider); // deleted in the keychain meanwhile
    }
    if (m_settings)
        if (const QString key = m_settings->read(provider, &error); !key.isEmpty()) {
            m_cache.insert(provider, {key, KeySource::Kind::Settings});
            return {KeySource::Kind::Settings, m_settings->name(), key};
        }
    return {};
}

bool Keys::store(const QString& provider, const QString& key, QString* error)
{
    m_cache.remove(provider);
    if (m_keychain) {
        if (!m_keychain->write(provider, key, error)) return false;
        if (m_settings) m_settings->remove(provider, nullptr); // no second, unencrypted copy
        QSettings().setValue(kInKeychain + provider, true);
        return true;
    }
    return m_settings && m_settings->write(provider, key, error);
}

bool Keys::remove(const QString& provider, QString* error)
{
    m_cache.remove(provider);
    QSettings().remove(kInKeychain + provider);
    bool ok = true;
    if (m_keychain) ok = m_keychain->remove(provider, error);
    if (m_settings) ok = m_settings->remove(provider, error) && ok;
    return ok;
}

bool Keys::hasStored(const QString& provider)
{
    if (m_cache.contains(provider)) return true;
    return (m_keychain && QSettings().value(kInKeychain + provider).toBool()) || (m_settings && m_settings->has(provider));
}

QString Keys::storeName() const { return m_keychain ? m_keychain->name() : m_settings ? m_settings->name() : QString(); }

void Keys::forget() { m_cache.clear(); }

} // namespace Ai
