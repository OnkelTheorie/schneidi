#include "engine/video/Text.h"

#include "engine/video/Compose.h"

#include <Mlt.h>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <algorithm>
#include <cfloat>
#include <cstring>

namespace Text {

namespace {

QColor colorAt(mlt_properties p, const char* name, int pos, int len, QColor fallback)
{
    if (!mlt_properties_get(p, name)) return fallback;
    const mlt_color c = mlt_properties_anim_get_color(p, name, pos, len);
    return QColor(c.r, c.g, c.b, c.a);
}

int getImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    const int pos = mlt_frame_pop_service_int(frame);
    const int len = mlt_frame_pop_service_int(frame);
    mlt_properties fp = MLT_FILTER_PROPERTIES(filter);
    *format = mlt_image_rgba;
    const int error = mlt_frame_get_image(frame, image, format, width, height, 1);
    if (error || !*image || *format != mlt_image_rgba) return error;
    const QString text = QString::fromUtf8(mlt_properties_get(fp, "argument"));
    if (text.isEmpty()) return 0;
    const int w = *width, h = *height;
    const mlt_profile profile = mlt_service_profile(MLT_FILTER_SERVICE(filter));
    const double sx = profile && profile->width > 0 ? double(w) / profile->width : 1.0;
    const double sy = profile && profile->height > 0 ? double(h) / profile->height : 1.0;
    QRectF area(0, 0, w, h);
    if (mlt_properties_get(fp, "geometry")) {
        const mlt_rect r = mlt_properties_anim_get_rect(fp, "geometry", pos, len);
        if (r.w != DBL_MIN && r.h != DBL_MIN) area = QRectF(r.x * sx, r.y * sy, r.w * sx, r.h * sy);
    }
    QFont font(QString::fromUtf8(mlt_properties_get(fp, "family") ? mlt_properties_get(fp, "family") : "Sans"));
    const double size = mlt_properties_get(fp, "size") ? mlt_properties_get_double(fp, "size") : 48.0;
    font.setPixelSize(std::max(1, int(std::lround(size * sy))));
    const int weight = mlt_properties_get(fp, "weight") ? mlt_properties_get_int(fp, "weight") : 400;
    font.setWeight(QFont::Weight(std::clamp(weight, 1, 1000)));
    const char* style = mlt_properties_get(fp, "style");
    font.setItalic(style && std::strcmp(style, "italic") == 0);
    const char* ha = mlt_properties_get(fp, "halign");
    const char* va = mlt_properties_get(fp, "valign");
    const int halign = !ha ? 0 : std::strcmp(ha, "center") == 0 || std::strcmp(ha, "centre") == 0 ? 1 : std::strcmp(ha, "right") == 0 ? 2 : 0;
    const int valign = !va ? 0 : std::strcmp(va, "middle") == 0 ? 1 : std::strcmp(va, "bottom") == 0 ? 2 : 0;
    const QColor fg = colorAt(fp, "fgcolour", pos, len, Qt::white);
    const QColor bg = colorAt(fp, "bgcolour", pos, len, Qt::transparent);
    const QColor ol = colorAt(fp, "olcolour", pos, len, Qt::black);
    const double outline = std::max(0.0, mlt_properties_get_double(fp, "outline")) * sy;
    const double pad = std::max(0.0, mlt_properties_get_double(fp, "pad")) * sy;

    // The block: lines one below the other at the font's line spacing, each aligned within the widest. Around it the
    // outline's outer half and the box margin; the whole is shrunk to fit the area when larger (a box margin or an
    // outline makes the text a little smaller, as qtext did) and aligned in it.
    const QStringList lines = text.split('\n');
    const QFontMetricsF fm(font);
    double blockW = 0;
    for (const QString& line : lines) blockW = std::max(blockW, fm.horizontalAdvance(line));
    const double blockH = fm.lineSpacing() * (lines.size() - 1) + fm.height();
    const double ext = pad + outline / 2;
    const double cw = blockW + 2 * ext, ch = blockH + 2 * ext;
    double k = 1.0;
    if (cw > 0 && area.width() > 0) k = std::min(k, area.width() / cw);
    if (ch > 0 && area.height() > 0) k = std::min(k, area.height() / ch);
    const double ox = halign == 0 ? area.left() : halign == 1 ? area.left() + (area.width() - cw * k) / 2
                                                               : area.right() - cw * k;
    const double oy = valign == 0 ? area.top() : valign == 1 ? area.top() + (area.height() - ch * k) / 2
                                                             : area.bottom() - ch * k;
    QPainterPath path;
    for (int i = 0; i < lines.size(); ++i) {
        const double adv = fm.horizontalAdvance(lines[i]);
        const double x = ext + (halign == 0 ? 0.0 : halign == 1 ? (blockW - adv) / 2 : blockW - adv);
        path.addText(QPointF(x, ext + fm.ascent() + i * fm.lineSpacing()), font, lines[i]);
    }
    QImage img(*image, w, h, w * 4, QImage::Format_RGBA8888);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(ox, oy);
    p.scale(k, k);
    if (bg.alpha() > 0) p.fillRect(QRectF(0, 0, cw, ch), bg);
    if (outline > 0 && ol.alpha() > 0) p.strokePath(path, QPen(ol, outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.fillPath(path, fg);
    p.end();
    mlt_frame_set_alpha(frame, nullptr, 0, nullptr);
    return 0;
}

mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service_int(frame, int(mlt_filter_get_length2(filter, frame)));
    mlt_frame_push_service_int(frame, int(mlt_filter_get_position(filter, frame)));
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, getImage);
    return frame;
}

mlt_filter create(mlt_profile profile, const char* service)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    f->process = process;
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), "_profile", profile, 0, nullptr, nullptr);
    mlt_properties_set(MLT_FILTER_PROPERTIES(f), "mlt_service", service);
    return f;
}

void* createService(mlt_profile profile, mlt_service_type, const char* id, const void*) { return create(profile, id); }

} // namespace

Mlt::Filter filter(Mlt::Profile& profile)
{
    if (Compose::qtReference()) {
        Mlt::Filter qt(profile, "qtext");
        if (qt.is_valid()) return qt;
    }
    mlt_filter f = create(profile.get_profile(), "schneidi_text");
    if (!f) return Mlt::Filter();
    Mlt::Filter out(f); // holds its own reference
    mlt_filter_close(f);
    return out;
}

void registerService()
{
    mlt_repository repo = mlt_factory_repository();
    if (!repo) return;
    mlt_repository_register(repo, mlt_service_filter_type, "schneidi_text", createService);
    if (!mlt_properties_get_data(mlt_repository_filters(repo), "qtext", nullptr))
        mlt_repository_register(repo, mlt_service_filter_type, "qtext", createService);
}

} // namespace Text
