// SPDX-License-Identifier: AGPL-3.0-only
// Custom effect "shader" (CustomEffects.h): a GLSL fragment function written by an AI or a user, drawn on the card.
//
// Compiling: Qt RHI wants shaders as .qsb (SPIR-V plus GLSL/HLSL/MSL made from it). Qt Shader Tools (QShaderBaker)
// would do it in-process, but it is a separate Qt module that neither Debian's dev packages nor our builds need
// otherwise; its command-line form `qsb` (Debian qt6-shader-baker, MSYS2 qt6-shadertools) does the same. Both are
// GPL-3 (Qt Shader Tools): linked or bundled they would break the GPL-free packages, run as a separate program found
// on the system they do not (the .deb recommends it; AppImage/Windows find one only if installed). So the code
// is put into a fixed frame (the uniform block and bindings of the built-in shaders, quad.vert as vertex stage),
// written to a temporary file and baked by `qsb` through QProcess (no shell). Results are kept in memory and in the
// cache folder by a hash of the full source, so a shader is baked once per machine, not per frame or start.
// Drawing: Gpu::Service::run uploads the picture, draws one pass (Context::customPass) and reads it back. That is
// one upload + readback per clip and frame (~2-4 ms at 1080p), fine for an effect, and it keeps the CPU picture path.
// No CPU fallback: without a GPU the effect is skipped (Renderer warns, `check`/`render` list it as skipped).
#include "engine/picture/CustomEffects.h"

#include "engine/picture/gpu/Gpu.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLibraryInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifdef SCHNEIDI_HAVE_RHI
#include <rhi/qshader.h>
#endif

#include <cstring>
#include <mutex>

namespace Picture::Custom {

namespace {

// Lines of the frame before the user's code (#line 1 makes the compiler count the user's lines from 1)
const char* kPrelude = R"(#version 440
layout(location = 0) in vec2 dpos;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Uniforms {
    vec4 rect;
    vec4 target;
    vec4 inv0;
    vec4 inv1;
    vec4 src;
    vec4 chroma;
    mat4 yuv;
    vec4 lut;
    vec4 crop;
};
layout(binding = 1) uniform sampler2D tex0;
#define resolution (target.xy)
#define time (inv0.x)
#define frame (inv0.y)
#define fps (inv0.z)
#define p1 (inv1.x)
#define p2 (inv1.y)
#define p3 (inv1.z)
#define p4 (inv1.w)
#define p5 (src.x)
#define p6 (src.y)
#define p7 (src.z)
#define p8 (src.w)
#define color1 (chroma)
#define color2 (yuv[0])
vec4 pixelAt(ivec2 p)
{
    return texelFetch(tex0, clamp(p, ivec2(0), ivec2(target.xy) - 1), 0);
}
vec4 pixel(vec2 uv)
{
    vec2 f = uv * target.xy - 0.5;
    ivec2 i = ivec2(floor(f));
    vec2 w = f - floor(f);
    vec4 a = pixelAt(i), b = pixelAt(i + ivec2(1, 0)), c = pixelAt(i + ivec2(0, 1)), d = pixelAt(i + ivec2(1, 1));
    a.rgb *= a.a; b.rgb *= b.a; c.rgb *= c.a; d.rgb *= d.a;
    vec4 m = mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
    return m.a > 0.0 ? vec4(m.rgb / m.a, m.a) : vec4(0.0);
}
vec4 effect(vec2 uv);
void main()
{
    vec4 c = clamp(effect(dpos / target.xy), 0.0, 1.0);
    fragColor = vec4(c.rgb * c.a, c.a);
}
#line 1
)";

QString fullSource(const QString& code) { return QString::fromLatin1(kPrelude) + code + QLatin1Char('\n'); }

// The qsb program: SCHNEIDI_QSB, next to schneidi (bundled packages), Qt's own folders, the PATH
QString qsbPath()
{
    static const QString path = [] {
        const QString env = qEnvironmentVariable("SCHNEIDI_QSB");
        if (!env.isEmpty() && QFileInfo(env).isExecutable()) return env;
        const QStringList dirs = {QCoreApplication::applicationDirPath(),
                                  QLibraryInfo::path(QLibraryInfo::BinariesPath),
                                  QLibraryInfo::path(QLibraryInfo::LibraryExecutablesPath)};
        QString found = QStandardPaths::findExecutable(QStringLiteral("qsb"), dirs);
        if (found.isEmpty()) found = QStandardPaths::findExecutable(QStringLiteral("qsb"));
        return found;
    }();
    return path;
}

#ifdef SCHNEIDI_HAVE_RHI

struct Compiled {
    QShader shader;
    QString error;
};

std::mutex g_mutex;
QHash<QByteArray, Compiled> g_compiled; // hash of the full source -> result

QByteArray keyOf(const QString& source)
{
    return QCryptographicHash::hash(source.toUtf8() + QByteArray(QT_VERSION_STR), QCryptographicHash::Sha1).toHex();
}

QString cacheFile(const QByteArray& key)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (dir.isEmpty()) return {};
    return QDir(dir).filePath(QStringLiteral("shaders/%1.qsb").arg(QString::fromLatin1(key)));
}

// The compiler's message without the temporary file's name, line numbers as the user's
QString cleanMessage(QString text)
{
    text.replace(QRegularExpression(QStringLiteral(R"([^\s:]*effect\.frag)")), QStringLiteral("shader"));
    QStringList keep;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const QString l = line.trimmed();
        if (l.isEmpty() || l.startsWith(QLatin1String("Failed to"))) continue;
        keep << l;
    }
    return keep.join(QLatin1Char('\n'));
}

Compiled compile(const QString& code)
{
    const QString source = fullSource(code);
    const QByteArray key = keyOf(source);
    {
        std::lock_guard lock(g_mutex);
        if (auto it = g_compiled.constFind(key); it != g_compiled.cend()) return *it;
    }
    Compiled c;
    const QString cached = cacheFile(key);
    if (QFile f(cached); !cached.isEmpty() && f.open(QIODevice::ReadOnly)) c.shader = QShader::fromSerialized(f.readAll());
    if (!c.shader.isValid()) {
        const QString qsb = qsbPath();
        QTemporaryDir tmp;
        if (qsb.isEmpty()) {
            c.error = QStringLiteral("no shader compiler: the program qsb (Qt Shader Tools) was not found");
        } else if (!tmp.isValid()) {
            c.error = QStringLiteral("no temporary folder for the shader compiler");
        } else {
            const QString in = tmp.filePath(QStringLiteral("effect.frag")), out = tmp.filePath(QStringLiteral("e.qsb"));
            QFile f(in);
            if (f.open(QIODevice::WriteOnly)) f.write(source.toUtf8());
            f.close();
            QProcess p;
            p.setProcessChannelMode(QProcess::MergedChannels);
            p.start(qsb, {QStringLiteral("--glsl"), QStringLiteral("300 es,150,330"), QStringLiteral("--hlsl"),
                          QStringLiteral("50"), QStringLiteral("--msl"), QStringLiteral("12"), QStringLiteral("-o"), out,
                          in});
            if (!p.waitForFinished(60000)) {
                p.kill();
                p.waitForFinished();
                c.error = QStringLiteral("the shader compiler did not finish");
            } else {
                QFile o(out);
                if (p.exitCode() == 0 && o.open(QIODevice::ReadOnly)) {
                    const QByteArray data = o.readAll();
                    c.shader = QShader::fromSerialized(data);
                    if (!cached.isEmpty() && c.shader.isValid() && QDir().mkpath(QFileInfo(cached).path())) {
                        QFile w(cached);
                        if (w.open(QIODevice::WriteOnly)) w.write(data);
                    }
                }
                if (!c.shader.isValid()) {
                    c.error = cleanMessage(QString::fromUtf8(p.readAll()));
                    if (c.error.isEmpty()) c.error = QStringLiteral("the shader does not compile");
                }
            }
        }
    }
    std::lock_guard lock(g_mutex);
    if (g_compiled.size() > 64) g_compiled.clear();
    g_compiled.insert(key, c);
    return c;
}

#endif

} // namespace

QString shaderHelp()
{
    return QStringLiteral(
        "GLSL 4.40 code that defines `vec4 effect(vec2 uv)`: uv = 0..1 of the picture (x right, y down), return the "
        "colour (straight alpha, 0..1). Given: pixel(uv) = the clip's picture there (bilinear, straight alpha), "
        "pixelAt(ivec2) = one pixel, resolution (vec2, pixels), time (clip seconds), frame (clip frame), fps, p1..p8 "
        "(the effect's number values, keyframable), color1, color2 (vec4). Own helper functions, consts and #define "
        "are fine; do not name things time, frame, p1 …. Example: vec4 effect(vec2 uv) { vec4 c = pixel(uv + vec2(sin(uv.y "
        "* 40.0 + time * 6.0) * 0.01 * p1, 0.0)); return c; }");
}

bool shaderCompilerAvailable(QString* why)
{
#ifdef SCHNEIDI_HAVE_RHI
    if (!qsbPath().isEmpty()) return true;
    if (why) *why = QStringLiteral("the program qsb (Qt Shader Tools) was not found, shaders cannot be compiled");
    return false;
#else
    if (why) *why = QStringLiteral("this build has no GPU path (Qt RHI)");
    return false;
#endif
}

bool shaderRenderable(QString* why)
{
    if (!shaderCompilerAvailable(why)) return false;
    if (Gpu::enabled()) return true;
    if (why) *why = QStringLiteral("no graphics card for shader effects (%1)").arg(Gpu::probe().error);
    return false;
}

QString checkShader(const QString& code)
{
    if (code.trimmed().isEmpty()) return {};
    QString why;
    if (!shaderCompilerAvailable(&why)) return why;
#ifdef SCHNEIDI_HAVE_RHI
    if (!code.contains(QRegularExpression(QStringLiteral(R"(\bvec4\s+effect\s*\()"))))
        return QStringLiteral("the code must define vec4 effect(vec2 uv)");
    return compile(code).error;
#else
    return why;
#endif
}

bool applyShader(const QString& code, const ShaderValues& v, uint8_t* rgba, int width, int height, QString* error)
{
    if (code.trimmed().isEmpty() || width <= 0 || height <= 0) return true;
#ifdef SCHNEIDI_HAVE_RHI
    auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    const Compiled c = compile(code);
    if (!c.shader.isValid()) return fail(c.error);
    Gpu::Service* gpu = Gpu::Service::instance();
    if (!gpu) return fail(QStringLiteral("no graphics card for shader effects (%1)").arg(Gpu::probe().error));
    if (gpu->onThread()) return fail(QStringLiteral("shader effect inside a GPU job"));
    float p[40] = {};
    p[0] = float(v.seconds), p[1] = float(v.frame), p[2] = float(v.fps);
    for (int i = 0; i < 8; ++i) p[4 + i] = float(v.p[i]);
    const QColor c1 = v.color1.toRgb(), c2 = v.color2.toRgb();
    const float cols[8] = {float(c1.redF()), float(c1.greenF()), float(c1.blueF()), float(c1.alphaF()),
                           float(c2.redF()), float(c2.greenF()), float(c2.blueF()), float(c2.alphaF())};
    std::memcpy(p + 12, cols, sizeof cols);
    const QByteArray key = keyOf(fullSource(code));
    bool drawn = false;
    // Kept until the frame is finished (the pool would hand them to the next job of the same frame)
    Gpu::TexturePtr in, out;
    const bool ran = gpu->run([&](Gpu::Context& g) {
        in = g.uploadRgba(rgba, width, height, false);
        if (!in) return;
        out = g.customPass(key, c.shader, QSize(width, height), p, in.get());
        if (!out) return;
        g.readRgba(*out, rgba);
        drawn = true;
    });
    if (!ran || !drawn) return fail(QStringLiteral("the graphics card could not draw the shader"));
    return true;
#else
    (void)v, (void)rgba;
    if (error) *error = QStringLiteral("this build has no GPU path (Qt RHI)");
    return false;
#endif
}

} // namespace Picture::Custom
