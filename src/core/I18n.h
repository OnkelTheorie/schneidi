#pragma once

#include <QChar>
#include <QList>
#include <QString>

// UI language: German is the source language in the code, every other language comes from i18n/<code>.json
// (German text -> translated text). A new language only needs a new file there. Switching takes effect after a restart.
namespace I18n {

struct Language {
    QString code; // "de", "en", "pl", … (file name i18n/<code>.json)
    QString name; // native name for the language menu
};

QList<Language> languages();           // German plus one language per dictionary in i18n/, German first
QString language();                    // code of the current UI language
void setLanguage(const QString& lang); // only stores it, takes effect on the next start
void install(const QString& override = {}); // after creating the QApplication; override = language for this run only
QString translate(const QString& lang, const char* source); // source text in another language than the installed one
QChar decimalPoint();                  // decimal separator of the UI language ("," in German, "." in English)

} // namespace I18n

// Marks text in tables (const char*) for the dictionary; translation happens on display with T()
#define N_(s) s

// Translated UI text (German source)
QString T(const char* source);
QString T(const QString& source);
