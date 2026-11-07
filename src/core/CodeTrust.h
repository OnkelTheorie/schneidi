// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Trust for custom code (0.19): the filter chains and shaders of the custom effects (engine/picture/CustomEffects.h,
// clip effects "ffmpeg_filter"/"shader" and the comp node "fx" with e_chain/e_code) render only when the user trusts
// that code. A project or recipe from someone else (shared online) could otherwise keep the graphics card or the CPU
// busy; it keeps its code, which is skipped until the user says yes.
// - Process-wide set of trusted code (the text itself): what the user wrote in this run (edit op, app), what they
//   confirmed (dialog, inspector, trust_custom_code:true) and the code of files remembered as trusted.
// - Remembered per file in the settings (never in the file itself): trust/<sha1 of the path key> = sha1 over all
//   custom code of the file (sorted). Loading a project (ProjectFile::load) or applying a recipe whose record matches
//   trusts its code; a changed file asks again. Saving a project whose code is all trusted writes the record.
// Projects without custom code are never looked at twice: no record, nothing to ask.

#include <QString>
#include <QStringList>
#include <QVector>

struct EffectInstance;
struct ProjectData;

namespace CodeTrust {

// One piece of custom code in a project
struct Item {
    QString effect; // "ffmpeg_filter" or "shader" (a comp fx node: its effect)
    QString code;   // filter chain or shader code
    int sequence = 0;
    int clip = 0;
    int node = 0;   // comp node id, 0 = the clip's own effect
};

// Custom code of one effect instance (a custom effect or the fx nodes of a comp graph)
QVector<Item> itemsOf(const EffectInstance& e);
// Every piece of custom code of a project (all timelines, compound clips included), each once per place
QVector<Item> items(const ProjectData& d);
// The texts alone, sorted and unique
QStringList codes(const ProjectData& d);
QStringList codes(const QVector<Item>& items);

// Process-wide (thread-safe, a lookup per call): empty code is always trusted
bool isTrusted(const QString& code);
void trust(const QString& code);
void trust(const QStringList& codes);
// The items whose code is not trusted (empty = all fine)
QVector<Item> untrusted(const ProjectData& d);
bool allTrusted(const QStringList& codes);

// The record of a file (project, recipe): sha1 over the codes
QString hashOf(const QStringList& codes);
bool remembered(const QString& path, const QStringList& codes);
void remember(const QString& path, const QStringList& codes);
// Trusts the codes when the file's record matches; true = all codes trusted now
bool admit(const QString& path, const QStringList& codes);
// The user trusts the codes of the file: in this run and remembered for the file
void trustFile(const QString& path, const QStringList& codes);

// Tests: forget the trust given in this run
void resetForTests();

} // namespace CodeTrust
