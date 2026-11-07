// SPDX-License-Identifier: AGPL-3.0-only
#include "core/CompRecipes.h"

#include "core/I18n.h"
#include "core/StartTemplates.h"
#include "core/TemplateZip.h"
#include "core/Types.h"
#include "core/Zip.h"

#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>

namespace {

QString& builtInOverride()
{
    static QString root;
    return root;
}

bool failWith(QString* error, QString* code, const char* c, const QString& message)
{
    if (error) *error = message;
    if (code) *code = QString::fromLatin1(c);
    return false;
}

const QStringList kSlotTypes{"media", "file", "text", "color", "number", "choice", "bool"};

// "3.size" -> node 3, key size (also "n3.size", the form of the stored values)
bool splitTarget(QString s, int* node, QString* key)
{
    if (s.startsWith('n')) s.remove(0, 1);
    const int dot = s.indexOf('.');
    if (dot <= 0) return false;
    bool ok = false;
    *node = s.left(dot).toInt(&ok);
    *key = s.mid(dot + 1);
    return ok && *node > 0 && !key->isEmpty() && !key->contains('.');
}

// A JSON value as a node value: colors stay "#aarrggbb" strings, numbers double, the rest as JSON gives it
QVariant valueOf(const QJsonValue& v) { return v.toVariant(); }

QJsonValue jsonOf(const QVariant& v)
{
    if (v.typeId() == QMetaType::QColor) return v.value<QColor>().name(QColor::HexArgb);
    return QJsonValue::fromVariant(v);
}

std::optional<CompRecipe> readFile(const QString& file, const QString& baseDir, QString* error)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QString("cannot read %1").arg(QDir::toNativeSeparators(file));
        return std::nullopt;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        if (error) *error = QString("%1: not a recipe (%2)").arg(QDir::toNativeSeparators(file), pe.errorString());
        return std::nullopt;
    }
    CompRecipe r;
    QString why;
    if (!CompRecipes::fromJson(doc.object(), baseDir, &r, &why)) {
        if (error) *error = QString("%1: %2").arg(QDir::toNativeSeparators(file), why);
        return std::nullopt;
    }
    r.file = file;
    return r;
}

QVector<CompRecipe> sortedByName(QVector<CompRecipe> list)
{
    std::stable_sort(list.begin(), list.end(),
                     [](const CompRecipe& a, const CompRecipe& b) { return I18n::compareNames(a.name, b.name) < 0; });
    return list;
}

QString stagingDir(const QString& tag)
{
    return QDir(CompRecipes::dir())
        .filePath(QString(".%1.%2").arg(tag).arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0')));
}

bool writeJson(const QString& file, const QJsonObject& o)
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return f.commit();
}

// Moves a finished staging folder to dir()/<id> (an old one replaced only with replace)
bool moveIntoPlace(const QString& staging, const QString& id, bool replace, QString* error, QString* code)
{
    const QString target = QDir(CompRecipes::dir()).filePath(id);
    if (QFileInfo::exists(target)) {
        if (!replace) {
            QDir(staging).removeRecursively();
            return failWith(error, code, "EXISTS", QString("a recipe '%1' exists already (replace it or choose another name)").arg(id));
        }
        if (!QDir(target).removeRecursively()) {
            QDir(staging).removeRecursively();
            return failWith(error, code, "SAVE_FAILED", QString("cannot replace %1").arg(QDir::toNativeSeparators(target)));
        }
    }
    if (!QDir().rename(staging, target)) {
        QDir(staging).removeRecursively();
        return failWith(error, code, "SAVE_FAILED", QString("cannot write %1").arg(QDir::toNativeSeparators(target)));
    }
    return true;
}

} // namespace

const CompRecipeSlot* CompRecipe::slot(const QString& id) const
{
    for (const CompRecipeSlot& s : slotList)
        if (s.id == id) return &s;
    return nullptr;
}

namespace CompRecipes {

QString dir() { return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("comp"); }

QString builtInRoot() { return builtInOverride().isEmpty() ? QStringLiteral(":/comp") : builtInOverride(); }

void setBuiltInRoot(const QString& root) { builtInOverride() = root; }

QString idForName(const QString& name)
{
    QString id = StartTemplates::idForName(name.trimmed());
    if (id == QLatin1String("template")) id = QStringLiteral("recipe");
    return id;
}

bool fromJson(const QJsonObject& o, const QString& baseDir, CompRecipe* out, QString* error)
{
    const auto bad = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    CompRecipe r;
    r.name = o["name"].toString().trimmed();
    r.description = o["description"].toString();
    r.note = o["note"].toString();
    const QJsonObject g = o["graph"].toObject();
    const QJsonArray nodes = g["nodes"].toArray();
    if (nodes.isEmpty()) return bad("the recipe has no graph (graph: {nodes, links, out})");
    // Storage form of CompGraph: nodes without values, links as [from, to, input]
    QVariantList vnodes, vlinks;
    for (const QJsonValue& nv : nodes) {
        const QJsonObject n = nv.toObject();
        const int id = n["id"].toInt();
        vnodes << QVariantMap{{"id", id}, {"type", n["type"].toString()}, {"x", n["x"].toDouble()}, {"y", n["y"].toDouble()}};
        const QJsonObject params = n["params"].toObject();
        for (auto it = params.begin(); it != params.end(); ++it) r.values[id][it.key()] = valueOf(it.value());
    }
    for (const QJsonValue& lv : g["links"].toArray()) {
        const QJsonArray l = lv.toArray();
        if (l.size() != 3) return bad("a link is [from, to, input]");
        vlinks << QVariant(QVariantList{l[0].toInt(), l[1].toInt(), l[2].toString()});
    }
    const int outId = g.contains("out") ? g["out"].toInt() : nodes.last().toObject()["id"].toInt();
    QString why;
    if (!CompGraph::fromVariant(QVariantMap{{"nodes", vnodes}, {"links", vlinks}, {"out", outId}}, &r.graph, &why))
        return bad("graph: " + why);
    for (auto it = r.values.begin(); it != r.values.end(); ++it)
        if (!r.graph.node(it.key())) return bad(QString("values of node %1, which is not in the graph").arg(it.key()));
    // Files relative to the recipe's folder, never outside it (a shared recipe must not reach other files)
    if (!baseDir.isEmpty()) {
        const QString root = QDir::cleanPath(QDir(baseDir).absolutePath()) + '/';
        for (const CompNode& n : r.graph.nodes)
            for (const QString& key : Comp::fileKeys(n.type)) {
                QVariantMap& v = r.values[n.id];
                const QString path = v.value(key).toString();
                if (path.isEmpty() || QDir::isAbsolutePath(path)) continue;
                const QString full = QDir::cleanPath(QDir(baseDir).absoluteFilePath(path));
                if (!full.startsWith(root)) return bad(QString("node %1: the file '%2' is outside the recipe").arg(n.id).arg(path));
                v[key] = full;
            }
    }
    const QJsonObject slotList = o["slots"].toObject();
    for (auto it = slotList.begin(); it != slotList.end(); ++it) {
        const QJsonObject so = it.value().toObject();
        CompRecipeSlot s;
        s.id = it.key();
        s.type = so["type"].toString();
        s.description = so["description"].toString();
        s.required = so["required"].toBool();
        if (s.id.isEmpty() || s.id.contains('.')) return bad(QString("slot '%1': a slot name has no dot").arg(s.id));
        if (!kSlotTypes.contains(s.type))
            return bad(QString("slot '%1': type must be one of %2").arg(s.id, kSlotTypes.join(", ")));
        for (const QJsonValue& tv : so["targets"].toArray()) {
            CompRecipeTarget t;
            const QJsonObject to = tv.toObject();
            const QString spec = tv.isString() ? tv.toString() : to["param"].toString();
            if (!splitTarget(spec, &t.node, &t.key) || !r.graph.node(t.node))
                return bad(QString("slot '%1': target '%2' is not <node id>.<key> of a node in the graph").arg(s.id, spec));
            t.mul = to["mul"].toDouble(1);
            t.add = to["add"].toDouble(0);
            s.targets << t;
        }
        if (s.targets.isEmpty()) return bad(QString("slot '%1' has no targets").arg(s.id));
        r.slotList << s;
    }
    *out = r;
    return true;
}

QJsonObject toJson(const CompRecipe& r)
{
    QJsonArray nodes, links;
    for (const CompNode& n : r.graph.nodes) {
        QJsonObject o{{"id", n.id}, {"type", n.type}};
        if (!n.pos.isNull()) {
            o["x"] = n.pos.x();
            o["y"] = n.pos.y();
        }
        QJsonObject params;
        const QVariantMap v = r.values.value(n.id);
        for (auto it = v.begin(); it != v.end(); ++it) params[it.key()] = jsonOf(it.value());
        if (!params.isEmpty()) o["params"] = params;
        nodes << o;
    }
    for (const CompLink& l : r.graph.links) links << QJsonArray{l.from, l.to, l.input};
    QJsonObject slotList;
    for (const CompRecipeSlot& s : r.slotList) {
        QJsonArray targets;
        for (const CompRecipeTarget& t : s.targets) {
            const QString spec = QString("%1.%2").arg(t.node).arg(t.key);
            if (t.mul == 1 && t.add == 0) targets << spec;
            else targets << QJsonObject{{"param", spec}, {"mul", t.mul}, {"add", t.add}};
        }
        QJsonObject so{{"type", s.type}, {"targets", targets}};
        if (!s.description.isEmpty()) so["description"] = s.description;
        if (s.required) so["required"] = true;
        slotList[s.id] = so;
    }
    QJsonObject o{{"name", r.name}, {"graph", QJsonObject{{"nodes", nodes}, {"links", links}, {"out", r.graph.out}}},
                  {"slots", slotList}};
    if (!r.description.isEmpty()) o["description"] = r.description;
    if (!r.note.isEmpty()) o["note"] = r.note;
    return o;
}

QVector<CompRecipe> builtIn()
{
    QVector<CompRecipe> list;
    const QDir root(builtInRoot());
    for (const QFileInfo& fi : root.entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
        std::optional<CompRecipe> r = readFile(fi.absoluteFilePath(), {}, nullptr);
        if (!r) {
            qWarning("built-in recipe %s is broken", qPrintable(fi.fileName()));
            continue;
        }
        r->id = fi.completeBaseName();
        if (r->name.isEmpty()) r->name = r->id;
        r->builtIn = true;
        list << *r;
    }
    return list;
}

QVector<CompRecipe> own()
{
    QVector<CompRecipe> list;
    for (const QFileInfo& fi : QDir(dir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString file = QDir(fi.absoluteFilePath()).filePath(FileName);
        if (!QFileInfo::exists(file)) continue;
        std::optional<CompRecipe> r = readFile(file, fi.absoluteFilePath(), nullptr);
        if (!r) continue;
        r->id = fi.fileName();
        if (r->name.isEmpty()) r->name = r->id;
        r->dir = fi.absoluteFilePath();
        list << *r;
    }
    return sortedByName(list);
}

QVector<CompRecipe> all() { return builtIn() + own(); }

std::optional<CompRecipe> open(const QString& path, QString* error)
{
    const QFileInfo fi(path);
    if (fi.isDir()) {
        std::optional<CompRecipe> r = readFile(QDir(path).filePath(FileName), fi.absoluteFilePath(), error);
        if (r) {
            r->id = fi.fileName();
            r->dir = fi.absoluteFilePath();
            if (r->name.isEmpty()) r->name = r->id;
        }
        return r;
    }
    if (!fi.isFile()) {
        if (error) *error = QString("no recipe at %1").arg(QDir::toNativeSeparators(path));
        return std::nullopt;
    }
    const bool folder = fi.fileName() == QLatin1String(FileName);
    std::optional<CompRecipe> r = readFile(fi.absoluteFilePath(), fi.absolutePath(), error);
    if (r) {
        r->id = folder ? fi.dir().dirName() : fi.completeBaseName();
        if (folder) r->dir = fi.absolutePath();
        if (r->name.isEmpty()) r->name = r->id;
    }
    return r;
}

std::optional<CompRecipe> find(const QString& key, QString* error)
{
    const QVector<CompRecipe> list = all();
    for (const CompRecipe& r : list)
        if (r.id.compare(key, Qt::CaseInsensitive) == 0) return r;
    for (const CompRecipe& r : list)
        if (r.name.compare(key, Qt::CaseInsensitive) == 0) return r;
    if ((key.contains('/') || key.contains('\\') || key.endsWith(".json")) && QFileInfo::exists(key)) return open(key, error);
    if (error) {
        QStringList ids;
        for (const CompRecipe& r : list) ids << r.id;
        *error = QString("no recipe '%1' (recipes: %2; `effects kind:'recipes'` lists them)").arg(key, ids.join(", "));
    }
    return std::nullopt;
}

bool apply(const CompRecipe& r, const QVariantMap& with, CompGraph* graph, QHash<int, QVariantMap>* values, QString* error)
{
    const auto bad = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    QHash<int, QVariantMap> v = r.values;
    QStringList slotIds;
    for (const CompRecipeSlot& s : r.slotList) slotIds << s.id;
    for (auto it = with.begin(); it != with.end(); ++it) {
        const QString& name = it.key();
        const QVariant& x = it.value();
        const CompRecipeSlot* s = r.slot(name);
        if (!s) {
            int node = 0;
            QString key;
            if (splitTarget(name, &node, &key)) { // a node value directly
                if (!r.graph.node(node)) return bad(QString("recipe %1: no node %2 (with '%3')").arg(r.id).arg(node).arg(name));
                v[node][key] = x;
                continue;
            }
            return bad(QString("recipe %1 has no slot '%2' (slots: %3; or '<node id>.<key>')")
                           .arg(r.id, name, slotIds.isEmpty() ? QString("none") : slotIds.join(", ")));
        }
        const QString where = QString("with.%1").arg(name);
        QVariant value;
        const QMetaType::Type t = QMetaType::Type(x.typeId());
        const bool isNumber = t == QMetaType::Double || t == QMetaType::Int || t == QMetaType::LongLong
                              || t == QMetaType::UInt || t == QMetaType::ULongLong || t == QMetaType::Float;
        if (s->type == QLatin1String("number")) {
            if (!isNumber) return bad(where + " must be a number");
        } else if (s->type == QLatin1String("bool")) {
            if (t != QMetaType::Bool && !isNumber) return bad(where + " must be true or false");
            value = t == QMetaType::Bool ? x.toBool() : x.toDouble() != 0;
        } else if (s->type == QLatin1String("color")) {
            const QColor c = t == QMetaType::QColor ? x.value<QColor>() : QColor::fromString(x.toString());
            if (!c.isValid() || (t != QMetaType::QColor && t != QMetaType::QString))
                return bad(where + " must be a color (\"#rrggbb\" or \"#aarrggbb\")");
            value = c.name(QColor::HexArgb);
        } else { // media, file, text, choice
            if (t != QMetaType::QString) return bad(where + " must be a text");
            if (s->type != QLatin1String("text") && x.toString().isEmpty()) return bad(where + " is empty");
            value = x.toString();
        }
        for (const CompRecipeTarget& tg : s->targets)
            v[tg.node][tg.key] = s->type == QLatin1String("number") ? QVariant(x.toDouble() * tg.mul + tg.add) : value;
    }
    for (const CompRecipeSlot& s : r.slotList)
        if (s.required && !with.contains(s.id))
            return bad(QString("recipe %1 needs with.%2 (%3)").arg(r.id, s.id, s.description.isEmpty() ? s.type : s.description));
    *graph = r.graph;
    *values = v;
    return true;
}

std::optional<CompRecipe> save(const EffectInstance& comp, const QString& name, const SaveOptions& o, QString* error,
                               QString* code)
{
    if (name.trimmed().isEmpty()) {
        failWith(error, code, "BAD_ARGUMENT", "a recipe needs a name");
        return std::nullopt;
    }
    CompRecipe r;
    if (!Comp::graphOf(comp, &r.graph) || r.graph.isEmpty()) {
        failWith(error, code, "BAD_ARGUMENT", "the clip has no compositing graph to save");
        return std::nullopt;
    }
    r.name = name.trimmed();
    r.id = idForName(r.name);
    r.description = o.description;
    for (auto it = comp.params.begin(); it != comp.params.end(); ++it) {
        int id = 0;
        QString key;
        if (CompGraph::splitParamKey(it.key(), &id, &key) && r.graph.node(id)) r.values[id][key] = it.value();
    }
    if (!QDir().mkpath(dir())) {
        failWith(error, code, "SAVE_FAILED", QString("cannot make %1").arg(QDir::toNativeSeparators(dir())));
        return std::nullopt;
    }
    if (!o.replace && QFileInfo::exists(QDir(dir()).filePath(r.id))) {
        failWith(error, code, "EXISTS", QString("a recipe '%1' exists already (replace:true or another name)").arg(r.id));
        return std::nullopt;
    }
    // Files into media/ of a staging folder, the recipe pointing at them relative to its folder
    const QString staging = stagingDir(r.id + ".saving");
    QDir().mkpath(QDir(staging).filePath(MediaDirName));
    QHash<QString, QString> copied; // source -> relative name
    QSet<QString> names;
    for (const CompNode& n : r.graph.nodes)
        for (const QString& key : Comp::fileKeys(n.type)) {
            QVariantMap& v = r.values[n.id];
            const QString path = v.value(key).toString();
            if (path.isEmpty()) continue;
            if (path.startsWith(QLatin1String(":/"))) continue; // a built-in LUT: the program has it
            if (!copied.contains(path)) {
                const QFileInfo fi(path);
                QString file = fi.fileName();
                for (int i = 2; names.contains(file.toLower()); ++i)
                    file = QString("%1-%2%3").arg(fi.completeBaseName()).arg(i).arg(fi.suffix().isEmpty() ? QString() : "." + fi.suffix());
                names << file.toLower();
                const QString rel = QString("%1/%2").arg(QLatin1String(MediaDirName), file);
                if (!QFile::copy(path, QDir(staging).filePath(rel))) {
                    QDir(staging).removeRecursively();
                    failWith(error, code, "SAVE_FAILED", QString("cannot copy %1").arg(QDir::toNativeSeparators(path)));
                    return std::nullopt;
                }
                copied[path] = rel;
            }
            v[key] = copied.value(path);
        }
    // Slots: the files of media nodes and the words of text nodes
    int media = 0, texts = 0;
    for (const CompNode& n : r.graph.nodes) {
        media += n.type == QLatin1String(Comp::kMediaType);
        texts += n.type == QLatin1String(Comp::kTextType);
    }
    for (const CompNode& n : r.graph.nodes) {
        CompRecipeSlot s;
        if (n.type == QLatin1String(Comp::kMediaType)) {
            s.id = media == 1 ? QStringLiteral("media") : QString("media%1").arg(n.id);
            s.type = QStringLiteral("media");
            s.targets << CompRecipeTarget{n.id, Comp::kPathKey};
        } else if (n.type == QLatin1String(Comp::kTextType)) {
            s.id = texts == 1 ? QStringLiteral("text") : QString("text%1").arg(n.id);
            s.type = QStringLiteral("text");
            s.targets << CompRecipeTarget{n.id, QStringLiteral("text")};
        } else {
            continue;
        }
        r.slotList << s;
    }
    if (copied.isEmpty()) QDir(staging).rmdir(MediaDirName);
    if (!writeJson(QDir(staging).filePath(FileName), toJson(r))) {
        QDir(staging).removeRecursively();
        failWith(error, code, "SAVE_FAILED", QString("cannot write %1").arg(QDir::toNativeSeparators(dir())));
        return std::nullopt;
    }
    if (!moveIntoPlace(staging, r.id, o.replace, error, code)) return std::nullopt;
    return open(QDir(dir()).filePath(r.id), error);
}

bool remove(const QString& id, QString* error, QString* code)
{
    for (const CompRecipe& r : own())
        if (r.id.compare(id, Qt::CaseInsensitive) == 0 || r.name.compare(id, Qt::CaseInsensitive) == 0) {
            if (!QDir(r.dir).removeRecursively())
                return failWith(error, code, "FAILED", QString("cannot delete %1").arg(QDir::toNativeSeparators(r.dir)));
            return true;
        }
    return failWith(error, code, "NOT_FOUND", QString("no own recipe '%1' (built-in ones cannot be deleted)").arg(id));
}

bool exportZip(const QString& idOrName, const QString& zipPath, bool overwrite, QString* error, QString* code, int* fileCount)
{
    QString why;
    const std::optional<CompRecipe> r = find(idOrName, &why);
    if (!r) return failWith(error, code, "NOT_FOUND", why);
    if (!zipPath.endsWith(".zip", Qt::CaseInsensitive)) return failWith(error, code, "BAD_ARGUMENT", "the file must end in .zip");
    if (QFileInfo::exists(zipPath) && !overwrite)
        return failWith(error, code, "EXISTS", QString("%1 exists (overwrite:true)").arg(QDir::toNativeSeparators(zipPath)));
    QVector<Zip::Source> files;
    QTemporaryDir tmp; // a built-in recipe: its JSON written out (resources are no files for everyone)
    if (r->dir.isEmpty()) {
        const QString file = tmp.filePath(FileName);
        if (!writeJson(file, toJson(*r))) return failWith(error, code, "SAVE_FAILED", "cannot write a temporary file");
        files << Zip::Source{QString("%1/%2").arg(r->id, QLatin1String(FileName)), file};
    } else {
        QDirIterator it(r->dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            files << Zip::Source{QString("%1/%2").arg(r->id, QDir(r->dir).relativeFilePath(f)), f};
        }
    }
    const QString part = zipPath + ".part";
    QFile::remove(part);
    if (!Zip::write(part, files, &why)) {
        QFile::remove(part);
        return failWith(error, code, "SAVE_FAILED", why);
    }
    QFile::remove(zipPath);
    if (!QFile::rename(part, zipPath)) {
        QFile::remove(part);
        return failWith(error, code, "SAVE_FAILED", QString("cannot write %1").arg(QDir::toNativeSeparators(zipPath)));
    }
    if (fileCount) *fileCount = int(files.size());
    return true;
}

std::optional<CompRecipe> importZip(const QString& zipPath, bool replace, QString* error, QString* code)
{
    if (!QFileInfo(zipPath).isFile()) {
        failWith(error, code, "NOT_FOUND", QString("no file %1").arg(QDir::toNativeSeparators(zipPath)));
        return std::nullopt;
    }
    QString why;
    const std::optional<QVector<Zip::Entry>> list = Zip::entries(zipPath, &why);
    if (!list) {
        failWith(error, code, "BAD_ZIP", why);
        return std::nullopt;
    }
    // recipe.json at the root, or in the one top folder
    QString prefix;
    bool found = false;
    QSet<QString> tops;
    for (const Zip::Entry& e : *list) {
        const QString n = Zip::safeName(e.name);
        if (n.isEmpty() || Zip::junk(n)) continue;
        tops << n.section('/', 0, 0);
        if (n == QLatin1String(FileName)) found = true;
    }
    if (!found && tops.size() == 1) {
        prefix = *tops.begin() + '/';
        for (const Zip::Entry& e : *list)
            if (Zip::safeName(e.name) == prefix + FileName) found = true;
    }
    if (!found) {
        failWith(error, code, "BAD_RECIPE", "the archive holds no recipe.json (at its root or in its one folder)");
        return std::nullopt;
    }
    if (!QDir().mkpath(dir())) {
        failWith(error, code, "SAVE_FAILED", QString("cannot make %1").arg(QDir::toNativeSeparators(dir())));
        return std::nullopt;
    }
    const QString staging = stagingDir("import");
    if (!Zip::extract(zipPath, staging, prefix, &why, TemplateLibrary::importLimit(QFileInfo(zipPath).size()))) {
        QDir(staging).removeRecursively();
        failWith(error, code, "BAD_ZIP", why);
        return std::nullopt;
    }
    std::optional<CompRecipe> r = readFile(QDir(staging).filePath(FileName), staging, &why);
    if (!r) {
        QDir(staging).removeRecursively();
        failWith(error, code, "BAD_RECIPE", why);
        return std::nullopt;
    }
    if (r->name.isEmpty()) r->name = prefix.isEmpty() ? QFileInfo(zipPath).completeBaseName() : prefix.chopped(1);
    QString name = r->name;
    if (!replace) // a name of its own: "Name 2", "Name 3" …
        for (int i = 2; QFileInfo::exists(QDir(dir()).filePath(idForName(name))); ++i) name = QString("%1 %2").arg(r->name).arg(i);
    if (name != r->name) {
        QFile f(QDir(staging).filePath(FileName));
        QJsonObject o;
        if (f.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        o["name"] = name;
        writeJson(f.fileName(), o);
    }
    if (!moveIntoPlace(staging, idForName(name), true, error, code)) return std::nullopt;
    return open(QDir(dir()).filePath(idForName(name)), error);
}

} // namespace CompRecipes
