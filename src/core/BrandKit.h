// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Brand kit (0.18): logo (with a corner and size for placing it), up to four colours, up to two fonts and an
// intro/outro clip, set once. The global kit lives in AppDataLocation/brand-kit.json (all projects); a project may
// carry its own (ProjectData::brand) that replaces it. Titles reference the kit (TitleStyle::colorRef …) and keep
// the concrete value next to the reference: rendering never reads the kit, Editor::applyBrand copies the kit's
// values into the referencing titles. Neutral default: an empty kit, nothing references it.

#include <QColor>
#include <QDir>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

struct ProjectData;
struct TitleStyle;

struct BrandKit {
    QString logo;                 // file path (image/video), empty = none
    int logoCorner = 1;           // 0 = none/center, 1 top-right, 2 top-left, 3 bottom-right, 4 bottom-left
    double logoSize = 0.12;       // width as share of the picture width
    QVector<QColor> colors;       // 0..4 ("brand color 1..4")
    QStringList fonts;            // 0..2 ("brand font 1..2": heading, body)
    QString intro, outro;         // clip paths, empty = none
    bool operator==(const BrandKit&) const = default;

    static constexpr int kMaxColors = 4;
    static constexpr int kMaxFonts = 2;
    bool isEmpty() const { return *this == BrandKit{}; }
    // Brand colour / font n (1-based); nullopt = the kit has none there (a reference to it keeps the stored value)
    std::optional<QColor> color(int n) const;
    std::optional<QString> font(int n) const;
    // Values clamped to their ranges (hand-edited files, CLI): corner 0..4, size 0.01..1, at most 4 colours (invalid
    // ones dropped) and 2 fonts (empty ones dropped)
    BrandKit clamped() const;
};

namespace Brand {

// JSON of a kit. Paths: `base` = the folder relative paths are written against and read from (the project's folder
// for a project override, so a project moved together with its logo keeps it); without a base only absolute paths.
// Only fields that are set are written.
QJsonObject toJson(const BrandKit& kit, const QDir* base = nullptr);
BrandKit fromJson(const QJsonObject& o, const QDir* base = nullptr);

// The global kit (AppDataLocation/brand-kit.json; missing or broken = empty) and writing it (false + err on failure;
// an empty kit removes the file)
QString globalPath();
BrandKit global();
bool setGlobal(const BrandKit& kit, QString* err = nullptr);

// What applies to a project: its own kit, else the global one
BrandKit effective(const ProjectData& project);
BrandKit effective(const std::optional<BrandKit>& projectBrand);

// Resolve the references of one title style with the kit (colour, outline, box, font). Missing brand colour/font =
// the stored value stays. True when something changed.
bool resolve(TitleStyle& style, const BrandKit& kit);

} // namespace Brand
