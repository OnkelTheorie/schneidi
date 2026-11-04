// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The errors of the command layer (cli: a stable code plus an English message for AIs and programs) in the user's
// language, where the app shows them itself: a step of a plan card that did not run, a menu entry that maps onto an
// edit op ("Musik auf Länge…"). Known codes (and a few known messages of them) get a sentence of their own, the rest
// a sentence of the UI language around the tool's message.

#include <QString>

namespace ToolErrors {

QString text(const QString& code, const QString& message);

} // namespace ToolErrors
