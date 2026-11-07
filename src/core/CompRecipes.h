// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Compositing recipes (0.19): a ready graph for the effect "comp" (core/CompGraph.h) with named slots the user (or an
// AI) fills: {op:'comp', clips, recipe:'greenscreen', with:{background:'beach.jpg'}}.
//
// Form (JSON, the built-in ones in assets/comp/<id>.json, own ones as folders like the content templates):
//   {"name": "Green screen", "description": "...", "note": "shown after applying (next steps)",
//    "graph": {"nodes": [{"id": 1, "type": "clip"}, {"id": 2, "type": "key", "params": {"color": "#ff00ff00"}}, ...],
//              "links": [[1, 2, "src"], ...], "out": 5},
//    "slots": {"background": {"type": "media", "description": "...", "targets": ["4.path"]},
//              "size": {"type": "number", "targets": ["3.size", {"param": "2.size", "mul": 1.04}]}, ...}}
// The clip's own picture goes in where the graph has a "clip" node (several are fine). A slot's value goes into every
// target ("<node id>.<key>"; numbers as value * mul + add). Slot types: media (a file: media pool item), file (another
// file: LUT, texture), text, color, number, choice, bool. `with` may also set any node value directly as
// "<node id>.<key>". Missing slots keep the recipe's values (neutral: a recipe changes only what its name says);
// `required` slots must be given.
// Own recipes: AppDataLocation/comp/<id>/recipe.json, their files in <id>/media/ (paths in the recipe relative to the
// folder). Sharing = a ZIP of the folder (core/Zip.h, root folder = the id).
// Core only knows node kinds by name: whether types, inputs and ranges fit is checked where the kinds are (the CLI
// checks every value like a graph given by hand, Editor::setCompGraph validates the graph).

#include "core/CompGraph.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <optional>

struct CompRecipeTarget {
    int node = 0;
    QString key;
    double mul = 1, add = 0; // numbers only
};

struct CompRecipeSlot {
    QString id;
    QString type; // media, file, text, color, number, choice, bool
    QString description;
    bool required = false;
    QVector<CompRecipeTarget> targets;
};

struct CompRecipe {
    QString id;   // file/folder name
    QString name; // shown
    QString description;
    QString note; // hint after applying (e.g. "run track_node on node 2")
    bool builtIn = false;
    QString dir;  // own: the folder; built-in: empty
    QString file; // the recipe's JSON file
    CompGraph graph;
    QHash<int, QVariantMap> values; // node id -> {key: value}; files absolute
    QVector<CompRecipeSlot> slotList;

    const CompRecipeSlot* slot(const QString& id) const;
};

namespace CompRecipes {

inline constexpr const char* FileName = "recipe.json";
inline constexpr const char* MediaDirName = "media";

QString dir();         // AppDataLocation/comp (own recipes)
QString builtInRoot(); // ":/comp" (resources), or what setBuiltInRoot set
void setBuiltInRoot(const QString& root); // tests

// Reads a recipe from its JSON; relative file values are resolved against `baseDir` (empty = left as they are)
bool fromJson(const QJsonObject& o, const QString& baseDir, CompRecipe* out, QString* error = nullptr);
// The JSON form (files as stored in r.values; save() makes them relative before writing)
QJsonObject toJson(const CompRecipe& r);

QVector<CompRecipe> builtIn(); // by id
QVector<CompRecipe> own();     // the folders in dir() with a recipe.json, by name
QVector<CompRecipe> all();     // built-in, then own
// By id (built-in first), then by name (any case), or a path to a recipe folder / its JSON file
std::optional<CompRecipe> find(const QString& idOrNameOrPath, QString* error = nullptr);
// A recipe folder, its recipe.json or a recipe .json file anywhere
std::optional<CompRecipe> open(const QString& path, QString* error = nullptr);

// The graph and node values of the recipe with the slots filled from `with` ({slot: value} or {"<id>.<key>": value}).
// Errors (code BAD_ARGUMENT): unknown slot (the message lists the slots), a value of the wrong type, a required slot
// missing. Media/file values are taken as given (the caller makes them absolute and checks them)
bool apply(const CompRecipe& r, const QVariantMap& with, CompGraph* graph, QHash<int, QVariantMap>* values,
           QString* error = nullptr);

// The custom code (filter chains, shaders of fx nodes) of a recipe, for its trust record (core/CodeTrust.h)
QStringList customCode(const CompRecipe& r);

// Shown name / description in language `lang` (default: the UI language): the recipe file's optional
// "name_<lang>" / "description_<lang>" (the built-in ones have de, en, es, fr, pl, ru), else "name" / "description".
// "name" itself stays English for the AI. core/CompRecipeText.cpp (reads the file again, cached by its time)
QString shownName(const CompRecipe& r, const QString& lang = {});
QString shownDescription(const CompRecipe& r, const QString& lang = {});

// Folder name of an own recipe of this name (safe on every system)
QString idForName(const QString& name);

struct SaveOptions {
    QString description;
    bool replace = false; // replace an own recipe of the same id (else code EXISTS)
};
// Saves the instance of effect "comp" (graph + static node values; keyframes are not part of a recipe) as own recipe
// `name`: its files (Comp::fileKeys) are copied into media/; every media node and text node gets a slot ("media" /
// "text", or "media<id>" / "text<id>" when there are several). code: BAD_ARGUMENT, EXISTS, SAVE_FAILED
std::optional<CompRecipe> save(const EffectInstance& comp, const QString& name, const SaveOptions& options = {},
                               QString* error = nullptr, QString* code = nullptr);
// Own recipes only. code NOT_FOUND, FAILED
bool remove(const QString& id, QString* error = nullptr, QString* code = nullptr);

// Sharing: a recipe (built-in or own) as a ZIP of its folder (root folder = its id; a built-in one gets its media-less
// folder). An existing file only with overwrite. code: NOT_FOUND, BAD_ARGUMENT, EXISTS, SAVE_FAILED
bool exportZip(const QString& idOrName, const QString& zipPath, bool overwrite, QString* error = nullptr,
               QString* code = nullptr, int* fileCount = nullptr);
// Unpacks an archive (recipe.json at its root or in its one top folder) into the own recipes; the name comes from the
// recipe. A recipe of that name exists: replace, else a name of its own ("Name 2"). Unsafe archives are refused
// (core/Zip.h) before anything is written. code: NOT_FOUND, BAD_ZIP, BAD_RECIPE, SAVE_FAILED
std::optional<CompRecipe> importZip(const QString& zipPath, bool replace = false, QString* error = nullptr,
                                    QString* code = nullptr);

} // namespace CompRecipes
