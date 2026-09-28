#pragma once
// Katalog der verfügbaren Effekte. Ein Effekt = Beschreibung + MLT-Filter.
// Neuer Effekt: in EffectRegistry.cpp einen Eintrag ergänzen – Engine (und später
// der Inspector) lesen nur die Registry, sonst muss nichts angepasst werden.

#include <QString>
#include <QVariant>
#include <QVector>

struct EffectParam {
    enum Type { Double, Color };
    QString key;         // Name in EffectInstance::params
    QString label;       // Anzeige im Inspector
    QString mltProperty; // Property am MLT-Filter
    Type type = Double;
    QVariant defaultValue;
    double min = 0, max = 1;
};

struct EffectDescriptor {
    QString id;
    QString name;
    QString mltService;
    bool video = true; // false = Audioeffekt
    QVector<EffectParam> params;
};

namespace EffectRegistry {
const QVector<EffectDescriptor>& all();
const EffectDescriptor* find(const QString& id);
} // namespace EffectRegistry
