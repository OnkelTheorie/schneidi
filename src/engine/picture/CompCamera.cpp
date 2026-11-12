// SPDX-License-Identifier: AGPL-3.0-only
// The shared camera of a compositing graph (0.22): node kind "camera", the layer values z / camera and the map of a
// layer through the camera (see CompCamera.h)
#include "engine/picture/CompCamera.h"

#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/I18n.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace Picture::Comp {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

EffectParam num(const char* key, const QString& label, double def, double lo, double hi, double step, int decimals,
                const QString& description = {})
{
    EffectParam p;
    p.key = QString::fromLatin1(key);
    p.label = label;
    p.type = EffectParam::Double;
    p.defaultValue = def;
    p.min = lo;
    p.max = hi;
    p.step = step;
    p.decimals = decimals;
    p.description = description;
    return p;
}

double finite(double v, double fallback) { return std::isfinite(v) ? v : fallback; }

} // namespace

double driftPush(double perSecond, double seconds)
{
    const double x = perSecond * seconds;
    if (!(seconds > 0) || !std::isfinite(x)) return std::isfinite(x) ? x : 0.0; // before the start: straight on
    // Starts at perSecond, eases towards +-kDriftPushCap: never through the picture plane, never shrinking to nothing
    return std::copysign(kDriftPushCap * (1 - std::exp(-std::abs(x) / kDriftPushCap)), x);
}

namespace {

// 3x3 products, row-major
void mul(const double a[9], const double b[9], double out[9])
{
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
    std::memcpy(out, r, sizeof r);
}

} // namespace

Camera cameraAt(const std::function<double(int, const QString&)>& numOf, int id, QSize size, double seconds)
{
    Camera c;
    const double W = size.width(), H = size.height();
    const auto v = [&](const char* key, double fallback) { return finite(numOf(id, QString::fromLatin1(key)), fallback); };
    // The drift's own clock: seconds from the clip's start plus drift_offset (a head trim moves it, Comp::followHead)
    const double s = finite(seconds, 0.0) + v("drift_offset", 0);
    c.panX = (v("pan_x", 0) + v("drift_x", 0) * s) * W;
    c.panY = (v("pan_y", 0) + v("drift_y", 0) * s) * H;
    c.push = v("push", 0) + driftPush(v("drift_push", 0), s);
    c.zoom = std::clamp(v("zoom", 1), 0.01, 100.0);
    c.tiltX = std::clamp(v("tilt_x", 0), -89.0, 89.0);
    c.tiltY = std::clamp(v("tilt_y", 0), -89.0, 89.0);
    c.roll = v("roll", 0);
    c.fov = std::clamp(v("fov", 40), 5.0, 120.0);
    // Following a tracker: its motion (x, y moved from rx, ry, scaled by scale, turned by angle around it) as the
    // camera's pan, push and roll, so that the picture plane (z = 1) goes exactly with the tracked picture:
    // screen = T + s R(a) (q - Ref) (centred pixels) = R(-roll) (q - C) / (1 - push) gives push = 1 - 1/s,
    // roll = -a, C = Ref - R(-a) T / s
    if (const int f = ::Comp::followId(numOf(id, QStringLiteral("follow"))); f > 0 && f != id) {
        const auto t = [&](const char* key, double fallback) { return finite(numOf(f, QString::fromLatin1(key)), fallback); };
        double sc = t("scale", 1);
        sc = sc > 0.01 ? sc : 1.0;
        const double a = t("angle", 0) * kDeg;
        const double tx = (t("x", 0.5) - 0.5) * W, ty = (t("y", 0.5) - 0.5) * H;
        const double rx = (t("rx", 0.5) - 0.5) * W, ry = (t("ry", 0.5) - 0.5) * H;
        const double ca = std::cos(a), sa = std::sin(a);
        // R(-a) T = (cos a tx + sin a ty, -sin a tx + cos a ty)
        c.panX += rx - (ca * tx + sa * ty) / sc;
        c.panY += ry - (-sa * tx + ca * ty) / sc;
        c.push += 1 - 1 / sc;
        c.roll -= t("angle", 0);
    }
    return c;
}

bool cameraMap(const Camera& cam, double z, QSize size, Compose::Homography* out)
{
    const double W = size.width(), H = size.height();
    if (!(z > 0) || !std::isfinite(z) || W <= 0 || H <= 0) return false;
    const double D = H / 2.0 / std::tan(cam.fov * kDeg / 2.0);
    const double f = D * cam.zoom;
    // Layer pixel (sx, sy, 1) -> its place in space relative to the camera
    const double A[9] = {z, 0, -z * W / 2 - cam.panX, 0, z, -z * H / 2 - cam.panY, 0, 0, z * D - cam.push * D};
    // The camera's turn R = Ry(turn right) Rx(look up) Rz(roll clockwise); points seen by it: R^T
    const double ry = cam.tiltY * kDeg, rx = cam.tiltX * kDeg, rz = cam.roll * kDeg;
    const double Ry[9] = {std::cos(ry), 0, std::sin(ry), 0, 1, 0, -std::sin(ry), 0, std::cos(ry)};
    const double Rx[9] = {1, 0, 0, 0, std::cos(rx), -std::sin(rx), 0, std::sin(rx), std::cos(rx)};
    const double Rz[9] = {std::cos(rz), -std::sin(rz), 0, std::sin(rz), std::cos(rz), 0, 0, 0, 1};
    double R[9], Rt[9];
    mul(Ry, Rx, R);
    mul(R, Rz, R);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) Rt[3 * i + j] = R[3 * j + i];
    double P[9];
    mul(Rt, A, P);
    // Every corner in front of the camera (a layer partly behind it is not drawn: no clipping in 2.5D)
    const double corners[8] = {0, 0, W, 0, W, H, 0, H};
    double mean = 0;
    for (int i = 0; i < 4; ++i) {
        const double depth = P[6] * corners[2 * i] + P[7] * corners[2 * i + 1] + P[8];
        if (!(depth > D * 0.02)) return false;
        mean += depth;
    }
    mean /= 4;
    const double K[9] = {f, 0, W / 2, 0, f, H / 2, 0, 0, 1};
    double M[9];
    mul(K, P, M);
    Compose::Homography h;
    for (int i = 0; i < 9; ++i) {
        h.m[i] = M[i] / mean;
        if (!std::isfinite(h.m[i])) return false;
    }
    *out = h;
    return true;
}

bool inFront(const Compose::Homography& h, int sw, int sh)
{
    const double corners[8] = {0, 0, double(sw), 0, double(sw), double(sh), 0, double(sh)};
    for (int i = 0; i < 4; ++i) {
        const double w = h.m[6] * corners[2 * i] + h.m[7] * corners[2 * i + 1] + h.m[8];
        if (!(w > 1e-9)) return false;
    }
    return true;
}

double scaleAt(const Compose::Homography& h, double x, double y)
{
    const double* m = h.m;
    const double w = m[6] * x + m[7] * y + m[8];
    if (!(std::abs(w) > 1e-12)) return 0;
    const double X = (m[0] * x + m[1] * y + m[2]) / w, Y = (m[3] * x + m[4] * y + m[5]) / w;
    const double a = (m[0] - X * m[6]) / w, b = (m[1] - X * m[7]) / w;
    const double c = (m[3] - Y * m[6]) / w, d = (m[4] - Y * m[7]) / w;
    return std::sqrt(std::abs(a * d - b * c));
}

bool isIdentity(const Compose::Homography& h)
{
    static constexpr double id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) {
        // The translation in pixels, the rest relative (a millionth of a pixel over a 4K picture)
        const double tol = i == 2 || i == 5 ? 1e-6 : i >= 6 ? 1e-12 : 1e-9;
        if (std::abs(h.m[i] - id[i]) > tol) return false;
    }
    return true;
}

bool layerCamera(int self, double z, double camera, int graphCamera, const std::function<QString(int)>& typeOf,
                 const std::function<double(int, const QString&)>& numOf, QSize size, double seconds,
                 Compose::Homography* h, bool* hidden)
{
    *hidden = false;
    if (!numOf || !typeOf || size.isEmpty() || !(z > 0) || !std::isfinite(z)) return false;
    int id = ::Comp::followId(camera);
    if (id == 0) id = graphCamera; // not named: the graph's camera
    if (id <= 0 || id == self || typeOf(id) != QLatin1String(::Comp::kCameraType)) return false;
    const Camera cam = cameraAt(numOf, id, size, seconds);
    Compose::Homography m;
    if (!cameraMap(cam, z, size, &m)) {
        *hidden = true;
        return true;
    }
    if (isIdentity(m)) return false;
    *h = m;
    return true;
}

bool layerCamera(const NodeCtx& ctx, Compose::Homography* h, bool* hidden)
{
    *hidden = false;
    if (!ctx.num) return false;
    return layerCamera(ctx.node, ctx.num(QString::fromLatin1(::Comp::kLayerDepthKey)),
                       ctx.num(QString::fromLatin1(::Comp::kCameraKey)), ctx.graphCamera, ctx.typeOf, ctx.numOf, ctx.size,
                       ctx.t / std::max(1.0, ctx.fps), h, hidden);
}

void applyCamera(NodeCtx& ctx, Buf& out)
{
    if (out.kind != Buf::Rgba || out.isNull()) return;
    Compose::Homography h;
    bool hidden = false;
    if (!layerCamera(ctx, &h, &hidden)) return;
    if (hidden) { // behind the camera: nothing
        std::fill(out.px.begin(), out.px.end(), uint8_t(0));
        out.area = {};
        return;
    }
    Buf moved = ctx.take(Buf::Rgba);
    if (moved.w != out.w || moved.h != out.h) {
        ctx.give(std::move(moved));
        return;
    }
    Compose::draw(moved.data(), moved.w, moved.h, out.data(), out.w, out.h, h, 1.0);
    // The content area along (its corners mapped, a pixel of soft edge around); unknown = the whole picture
    QRect area;
    const QRect a = out.contentArea();
    bool ok = true;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const QPointF p : {QPointF(a.left(), a.top()), QPointF(a.right() + 1, a.top()),
                            QPointF(a.right() + 1, a.bottom() + 1), QPointF(a.left(), a.bottom() + 1)}) {
        double X = 0, Y = 0;
        if (!Compose::mapPoint(h, p.x(), p.y(), &X, &Y)) {
            ok = false;
            break;
        }
        x0 = std::min(x0, X), y0 = std::min(y0, Y), x1 = std::max(x1, X), y1 = std::max(y1, Y);
    }
    if (ok) {
        const QRect frame(0, 0, out.w, out.h);
        area = QRect(QPoint(int(std::floor(x0)) - 2, int(std::floor(y0)) - 2), QPoint(int(std::ceil(x1)) + 2, int(std::ceil(y1)) + 2))
                   .intersected(frame);
        if (area.isEmpty()) area = QRect(0, 0, 1, 1); // nothing visible (outside the frame); a 1-px area of nothing
    }
    out.px.swap(moved.px);
    out.area = area;
    ctx.give(std::move(moved));
}

bool finishesWithCamera(const NodeType& k)
{
    return ::Comp::takesCamera(k.id) && k.id != QLatin1String("transform");
}

QVector<EffectParam> cameraLayerParams()
{
    return {
        num(::Comp::kLayerDepthKey, T("Tiefe im Raum"), 0, 0, 100, 0.01, 2,
            T("Abstand zur Kamera: 1 = Bildebene, 2 = doppelt so weit (bewegt sich halb so viel), 0,5 = näher; 0 = die "
              "Kamera bewegt diese Ebene nicht")),
        num(::Comp::kCameraKey, T("Kamera"), 0, 0, 1e6, 1, 0,
            T("Id eines Kamera-Knotens (0 = die Kamera des Graphen); mit einer Tiefe über 0 bewegt sie diese Ebene")),
    };
}

namespace {

bool evalCamera(NodeCtx&, Buf*) { return false; } // values only, no picture

} // namespace

void registerCameraNodes(QVector<NodeType>& out)
{
    NodeType k;
    k.id = QString::fromLatin1(::Comp::kCameraType);
    k.name = T("Kamera");
    k.description = T("Gemeinsame Kamera für Ebenen mit Tiefe (2,5D): Schwenk, Fahrt, Zoom und Kippen; nahe Ebenen "
                      "wandern weiter als ferne (Parallaxe). Ohne Bild: sie bewegt jede Ebene mit einer Tiefe über 0");
    k.params = {
        num("pan_x", T("Schwenk X"), 0, -10, 10, 0.001, 3,
            T("Wohin die Kamera fährt (Bildbreiten auf der Bildebene, positiv = nach rechts; das Bild wandert andersherum)")),
        num("pan_y", T("Schwenk Y"), 0, -10, 10, 0.001, 3,
            T("Wohin die Kamera fährt (Bildhöhen auf der Bildebene, positiv = nach unten)")),
        num("push", T("Fahrt"), 0, -10, 0.95, 0.001, 3,
            T("Kamera fährt nach vorn (Anteil des Abstands zur Bildebene: 0,5 = Bildebene doppelt so groß); nahe Ebenen "
              "wachsen stärker als ferne")),
        num("zoom", T("Zoom"), 1, 0.05, 20, 0.001, 3, T("Brennweite: alles gleich größer, ohne Parallaxe")),
        num("tilt_x", T("Neigen"), 0, -89, 89, 0.1, 1, T("Kamera blickt nach oben (Grad; das Bild wandert nach unten)")),
        num("tilt_y", T("Drehen"), 0, -89, 89, 0.1, 1, T("Kamera dreht nach rechts (Grad; das Bild wandert nach links)")),
        num("roll", T("Rollen"), 0, -3600, 3600, 0.1, 1, T("Kamera dreht sich im Uhrzeigersinn (Grad)")),
        num("fov", T("Blickwinkel"), 40, 5, 120, 0.1, 1,
            T("Senkrechter Blickwinkel (Grad): klein = flach, groß = starke Perspektive beim Neigen und Drehen")),
        num("drift_x", T("Gleiten X"), 0, -2, 2, 0.001, 3,
            T("Gleichmäßiger Schwenk je Sekunde ab Clipanfang (Bildbreiten): langsame Bewegung ohne Keyframes")),
        num("drift_y", T("Gleiten Y"), 0, -2, 2, 0.001, 3, T("Gleichmäßiger Schwenk je Sekunde ab Clipanfang (Bildhöhen)")),
        num("drift_push", T("Gleitende Fahrt"), 0, -0.5, 0.5, 0.001, 3,
            T("Fahrt nach vorn je Sekunde ab Clipanfang (Ken-Burns-3D); wird mit der Zeit langsamer und kommt nie an der Bildebene an")),
        num("drift_offset", T("Gleiten ab"), 0, -36000, 36000, 0.01, 2,
            T("Sekunden Gleiten, die am Clipanfang schon vergangen sind (Trimmen am Anfang setzt das, damit das Bild stehen bleibt)")),
        num("follow", T("Folgt Tracker"), 0, 0, 1e6, 1, 0,
            T("Id eines Tracker-Knotens (0 = keiner): seine Bewegung wird Schwenk, Fahrt und Rollen der Kamera, Ebenen "
              "mit Tiefe 1 gehen genau mit, andere Tiefen bekommen Parallaxe")),
    };
    // Its values move the layers; the camera itself makes no picture. A drift moves it every frame
    k.isVarying = [](const NodeType& kind, const QVariantMap& values) {
        for (const char* d : {"drift_x", "drift_y", "drift_push"}) // (drift_offset alone moves nothing)
            if (numValue(kind, values, QString::fromLatin1(d)) != 0) return true;
        return false;
    };
    k.eval = evalCamera;
    out << k;
}

} // namespace Picture::Comp
