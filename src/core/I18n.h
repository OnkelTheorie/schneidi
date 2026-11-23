// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QChar>
#include <QList>
#include <QMutex>
#include <QString>

#include <atomic>

// UI language: German is the source language in the code, every other language comes from i18n/<code>.json
// (German text -> translated text). A new language only needs a new file there. switchTo() changes it in a running
// program (the main window then builds its widgets again: MainWindow::rebuildUi).
namespace I18n {

struct Language {
    QString code; // "de", "en", "pl", … (file name i18n/<code>.json)
    QString name; // native name for the language menu
};

// "日本語 (Japanese)": the language in itself and in English; englishName: "Japanese" (for an AI prompt)
QString displayName(const QString& code);
QString englishName(const QString& code);
QList<Language> languages();           // German plus one language per dictionary in i18n/, German first
QString language();                    // code of the current UI language
QString installedLanguage();           // language of this run (install(), also --lang); language() before install()
void setLanguage(const QString& lang); // only stores it (switchTo or the next start applies it)
// Before creating the QApplication (application name set): environment for texts of the platform theme (LANGUAGE)
void prepareEnvironment(const QString& override = {});
void install(const QString& override = {}); // after creating the QApplication; override = language for this run only
// While running: the translators of install() replaced by those of `lang` (also the locale and LANGUAGE); texts made
// before keep their old language - whoever shows them makes them again. false = `lang` unknown, nothing changed
bool switchTo(const QString& lang);
// The setting changed since install()/switchTo (setLanguage): switchTo(language()) is due. A --lang of this run is no
// change of the setting.
bool languageChanged();
// Counts install()/switchTo: texts made in the language before are out of date
int generation();

// A table with translated texts (effect names, node types ...), made once per UI language: get() returns the one of
// the language in use, made by `build` on first use after install()/switchTo. Tables of a language before stay alive
// (a render thread may still read one; pointers into them stay valid). Thread-safe.
template <class Table>
class Cached {
public:
    template <class Build>
    const Table& get(Build build)
    {
        const int now = generation();
        if (m_generation.load(std::memory_order_acquire) != now) {
            QMutexLocker lock(&m_mutex);
            if (m_generation.load(std::memory_order_relaxed) != now) {
                m_table.store(new Table(build()), std::memory_order_release); // (the one before stays: see above)
                m_generation.store(now, std::memory_order_release);
            }
        }
        return *m_table.load(std::memory_order_acquire);
    }

private:
    QMutex m_mutex;
    std::atomic<const Table*> m_table{nullptr};
    std::atomic<int> m_generation{-1};
};
QString translate(const QString& lang, const char* source); // source text in another language than the installed one
QChar decimalPoint();                  // decimal separator of the UI language ("," in German, "." in English)
// Thousands separator of the UI language ("." in German and Spanish, "," in English, a no-break space in French,
// Polish and Russian)
QChar groupSeparator();
// The same for a given language code ("de", "en" …), whatever translation is installed in this process
QChar decimalPointOf(const QString& lang);
QChar groupSeparatorOf(const QString& lang);
// Order of names in lists (media, LUTs, effects, timelines): by the system's collation, but without case first even
// where the system has none (C/POSIX locale on servers and in containers sorts by bytes: "Zeta" before "alpha")
int compareNames(const QString& a, const QString& b);

} // namespace I18n

// Marks text in tables (const char*) for the dictionary; translation happens on display with T()
#define N_(s) s

// Translated UI text (German source)
QString T(const char* source);
QString T(const QString& source);
