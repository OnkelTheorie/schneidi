// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Settings of the AI features (Settings › AI), the same for all projects (QSettings "ai/…"). Read once at start and
// after the settings dialog; test runs change them only in memory (MainWindow does not save them then).
// The AI chat's key is not here: it lives in the system keychain (app/AiKeys.h).

#include "app/AiContext.h"

#include <QColor>
#include <QHash>
#include <QString>

struct AiSettings {
    // AI indicator in the top bar: always (also to open the log), only while an AI is connected or works, never
    enum class Indicator { Always, WhileActive, Never };

    bool liveAllowed = true; // AIs may work on the project open in the app (schneidi-cli, MCP)
    int releaseMinutes = 2;  // an AI that sends nothing this long lets the app go for another one (0 = never)
    bool highlight = true;   // light up what an AI changes in the timeline
    QColor color = defaultColor();
    int highlightMs = 2000;  // how long it stays lit (fades out)
    bool distinguish = true; // new clips filled, edited ones outlined (off: both filled)
    Indicator indicator = Indicator::WhileActive;
    int logLength = 200;     // entries the AI log keeps (this session only; 0 = no log)

    // AI chat (the "AI" panel, app/AiChat.h). Approval: Auto = every change goes through (one undo step per edit);
    // Review = the changes of one request stay visible until the user accepts them (one undo step) or discards them.
    enum class Approval { Auto, Review };
    QString chatProvider;               // preset id (app/AiProvider.h); empty = not set up yet
    QString chatAddress;                // base URL; empty = the provider's own
    QHash<QString, QString> chatModels; // model per provider (empty/missing = the provider's default)
    Approval approval = Approval::Auto;
    // The short request for small or local models (app/AiContext.h): automatic, always, never
    Ai::Compact compact = Ai::Compact::Auto;
    QString chatModel() const { return chatModels.value(chatProvider); } // of the chosen provider

    static QColor defaultColor(); // violet, like the AI indicator
    static constexpr int kMinHighlightMs = 300;
    static constexpr int kMaxHighlightMs = 10000;
    static constexpr int kMaxLogLength = 1000;
    static constexpr int kMaxReleaseMinutes = 120;

    static AiSettings load();
    void save() const;
    bool operator==(const AiSettings&) const = default;
};
