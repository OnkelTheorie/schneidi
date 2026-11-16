// SPDX-License-Identifier: AGPL-3.0-only
#include "core/GradeStills.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"

#include <QBuffer>

#include <algorithm>

namespace GradeStills {

GradeStill make(int id, const QString& label, const QImage& picture, const Clip& source)
{
    GradeStill s;
    s.id = id;
    s.label = label;
    QImage small = picture.width() > kMaxWidth ? picture.scaledToWidth(kMaxWidth, Qt::SmoothTransformation) : picture;
    QBuffer buf(&s.image);
    buf.open(QIODevice::WriteOnly);
    small.convertToFormat(QImage::Format_RGB32).save(&buf, "JPG", 88);
    // The clip with only its grade: placement kept (keyframes are relative to it), nothing else
    Clip c;
    c.id = source.id;
    c.start = source.start;
    c.in = source.in;
    c.out = source.out;
    c.speed = source.speed;
    if (const EffectInstance* g = EffectRegistry::instance(source, "grade")) c.effects << *g;
    for (auto it = source.keys.cbegin(); it != source.keys.cend(); ++it) {
        const AnimParam p = it.key();
        QString effectId;
        const bool own = p >= AnimParam::GradeLiftY && p <= AnimParam::GradeExposure;
        if (own || (Keys::effectParamOf(p, &effectId, nullptr) && effectId == QLatin1String("grade")))
            c.keys.insert(p, it.value());
    }
    s.grade = c;
    return s;
}

QImage picture(const GradeStill& s)
{
    QImage img;
    img.loadFromData(s.image, "JPG");
    return img;
}

int nextId(const QVector<GradeStill>& stills)
{
    int id = 0;
    for (const GradeStill& s : stills) id = std::max(id, s.id);
    return id + 1;
}

} // namespace GradeStills
