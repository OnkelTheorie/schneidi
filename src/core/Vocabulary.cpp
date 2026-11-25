// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Vocabulary.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

#include <algorithm>

namespace Vocabulary {

namespace {
constexpr char kKey[] = "transcribe/vocabulary";
}

QStringList parse(const QString& text)
{
    static const QRegularExpression split(QStringLiteral("[\\n\\r,;]+"));
    QStringList out;
    QSet<QString> seen;
    for (QString w : text.split(split, Qt::SkipEmptyParts)) {
        w = w.simplified();
        if (w.isEmpty() || seen.contains(w.toLower())) continue;
        seen.insert(w.toLower());
        out << w;
    }
    return out;
}

QStringList combine(const QStringList& a, const QStringList& b)
{
    return parse((a + b).join('\n'));
}

QStringList stored()
{
    return parse(QSettings().value(kKey).toString());
}

void setStored(const QStringList& words)
{
    const QStringList list = parse(words.join('\n'));
    if (list.isEmpty()) QSettings().remove(kKey);
    else QSettings().setValue(kKey, list.join('\n'));
}

bool gameTermsOn()
{
    return QSettings().value(QStringLiteral("transcribe/gameTerms"), false).toBool();
}

void setGameTermsOn(bool on)
{
    if (on) QSettings().setValue(QStringLiteral("transcribe/gameTerms"), true);
    else QSettings().remove(QStringLiteral("transcribe/gameTerms"));
}

bool useGameTerms(const QJsonValue& given)
{
    return given.isBool() ? given.toBool() : gameTermsOn();
}

bool captionsCheckOn()
{
    return QSettings().value(QStringLiteral("transcribe/captionsCheck"), true).toBool();
}

void setCaptionsCheckOn(bool on)
{
    if (on) QSettings().remove(QStringLiteral("transcribe/captionsCheck"));
    else QSettings().setValue(QStringLiteral("transcribe/captionsCheck"), false);
}

QString captionsCheck(const QJsonValue& given)
{
    const QString v = given.toString().trimmed().toLower();
    if (v == QLatin1String("auto") || v == QLatin1String("on") || v == QLatin1String("off")) return v;
    if (given.isBool()) return given.toBool() ? QStringLiteral("on") : QStringLiteral("off");
    return captionsCheckOn() ? QStringLiteral("auto") : QStringLiteral("off");
}

QStringList gameTerms(const QString& game)
{
    const QString g = game.trimmed().toLower();
    // Short on purpose: each term takes room in whisper's prompt and pulls the spelling of similar sounds towards it
    if (g == QLatin1String("cs2") || g == QLatin1String("csgo") || g == QLatin1String("cs"))
        return {"Headshot", "AWP", "AK", "M4", "Deagle", "Pump", "Clutch", "Ace", "Rush", "Smoke", "Flash", "Molotov",
                "Molli", "Eco", "Force-Buy", "Defuse", "Bombe", "Mid", "Long", "Short", "CT", "T-Spawn"};
    if (g == QLatin1String("valorant"))
        return {"Headshot", "Spike", "Defuse", "Plant", "Ult", "Vandal", "Phantom", "Operator", "Sheriff", "Clutch",
                "Ace", "Rush", "Eco", "Mid"};
    return {};
}

int estimateTokens(const QString& text)
{
    int n = 0;
    for (const QString& w : text.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        n += std::max<int>(1, int((w.toUtf8().size() + 2) / 3));
    return n;
}

QString prompt(const QString& base, const QStringList& words, int maxTokens, QStringList* dropped)
{
    QString out = base.trimmed();
    int used = estimateTokens(out);
    QStringList kept;
    for (int i = 0; i < words.size(); ++i) {
        // ", word" (the comma rides on the word) and the final "."
        const int cost = estimateTokens(words[i] + ',');
        if (used + cost + 1 > maxTokens) {
            if (dropped) *dropped = words.mid(i);
            break;
        }
        used += cost;
        kept << words[i];
    }
    if (kept.isEmpty()) return out;
    return (out.isEmpty() ? QString() : out + ' ') + kept.join(QStringLiteral(", ")) + '.';
}

QString cacheId(const QStringList& words)
{
    if (words.isEmpty()) return {};
    return QString::fromLatin1(QCryptographicHash::hash(words.join('\n').toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
}

} // namespace Vocabulary
