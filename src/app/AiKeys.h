// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Keys (and company defaults) of the AI chat. Where a key comes from, first match wins:
//  1. environment: SCHNEIDI_AI_KEY, else the provider's own variable (ANTHROPIC_API_KEY, OPENAI_API_KEY,
//     OPENROUTER_API_KEY) – only for that provider, so a key never goes to another service than the one it is for
//  2. the company file schneidi-ai.json in a config folder (QStandardPaths::AppConfigLocation, all of them: Linux
//     /etc/xdg/schneidi and ~/.config/schneidi, Windows C:\ProgramData\schneidi\schneidi …): provider, address,
//     model, key or key_file, optionally "locked": true (the user cannot change provider and address)
//  3. what the user entered: the system keychain (Linux: Secret Service over D-Bus, Windows: Credential Manager),
//     without one the settings, unencrypted (Settings › AI says so)
// Keys never go into logs, the chat or test output.

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace Ai {

struct Company {
    QString file; // empty = no company file
    QString provider, address, model, key;
    bool locked = false;
    QString error; // the file is there but unusable (shown in Settings › AI)
    bool found() const { return !file.isEmpty(); }
};
// Reads one file (relative key_file: next to it)
Company readCompany(const QString& file);
// All company files in `dirs` (default: every AppConfigLocation folder). A locked file wins as it is; otherwise the
// values of a file closer to the user (earlier in the list) replace those of a system-wide one
Company company(const QStringList& dirs = {});
inline constexpr const char* CompanyFileName = "schneidi-ai.json";

// Where the key the user entered is kept
class KeyStore {
public:
    virtual ~KeyStore() = default;
    virtual QString name() const = 0; // for the user: "Secret Service", "Windows-Anmeldeinformationen" …
    virtual bool secure() const = 0;  // encrypted by the system (false: the settings file)
    virtual bool has(const QString& provider) = 0; // a key is stored (never asks to unlock)
    virtual QString read(const QString& provider, QString* error) = 0; // may ask the user to unlock the keychain
    virtual bool write(const QString& provider, const QString& key, QString* error) = 0;
    virtual bool remove(const QString& provider, QString* error) = 0;
};
// The system keychain; nullptr = none here (and always in test mode: tests never touch the user's keychain)
std::unique_ptr<KeyStore> systemKeyStore();
// Secret Service on the session bus (tests use a private bus); nullptr without D-Bus or service
std::unique_ptr<KeyStore> secretServiceStore();
// The settings (QSettings ai/chat/keys/<provider>), unencrypted
std::unique_ptr<KeyStore> settingsKeyStore();

struct KeySource {
    enum class Kind { None, Environment, Company, Keychain, Settings };
    Kind kind = Kind::None;
    QString where; // variable, company file or store name
    QString key;
};
// Environment variables for a provider, in the order they are read
QStringList keyVariables(const QString& provider);

// The chat's provider, address and model: a locked company file decides provider and address; otherwise the user's
// settings, then the company file's defaults, then the provider's own
struct Choice {
    QString provider, address, model;
    bool locked = false; // provider and address come from the company file
};
Choice choose(const QString& provider, const QString& address, const QHash<QString, QString>& models, const Company& c);

// The chat's keys: resolve (in the order above, read keys stay in memory for the session) and store/remove the
// user's own one (keychain when there is one, else the settings)
class Keys {
public:
    using Env = std::function<QString(const QString& name)>;
    Keys(); // the system keychain and the settings
    Keys(std::unique_ptr<KeyStore> keychain, std::unique_ptr<KeyStore> settings, Env env = {}); // tests
    ~Keys();

    KeySource resolve(const QString& provider, const Company& company);
    bool store(const QString& provider, const QString& key, QString* error);
    bool remove(const QString& provider, QString* error);
    // In the keychain (as far as this app stored or read it there; the keychain itself is not asked) or the settings
    bool hasStored(const QString& provider);
    bool secure() const { return m_keychain != nullptr; } // what store() uses
    QString storeName() const;
    void forget(); // drop the keys read so far (settings changed)

private:
    std::unique_ptr<KeyStore> m_keychain, m_settings;
    Env m_env;
    struct Cached {
        QString key;
        KeySource::Kind kind;
    };
    QHash<QString, Cached> m_cache;
};

} // namespace Ai
