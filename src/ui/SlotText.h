// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Translated texts about template placeholders (ClipSlot, core/TemplateFill.h) for the app: kind and fit names, the
// tooltip of a placeholder clip, what a fill did (status bar) and the problems "Check Fields" lists. Core reports in
// English for the CLI; the app builds its own words from the machine-readable codes.

#include "core/TemplateFill.h"
#include "core/Types.h"

#include <QString>

namespace SlotText {

QString kindName(SlotKind kind);
QString fitName(SlotFit fit);
// Seconds with the locale's decimal point ("3,5 s")
QString seconds(double s);
// Label, kind, hint and rules of a placeholder (rich text lines for a tooltip); fps of the project
QString tooltip(const ClipSlot& slot, double fps);
// One problem (code + slot) in the user's language
QString problem(const TemplateFill::Problem& p);
// What a fill did, one line: "Platzhalter „Clip 1“ gefüllt: gekürzt 12,0 s → 3,0 s, angepasst (Mitte)" or the error
QString report(const TemplateFill::Report& report, const QString& label);

} // namespace SlotText
