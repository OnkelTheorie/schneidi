// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Direct page's real backend (app/DirectBackend.h): the `direct` command in the app. Suggestions run as a
// background job (Cli::Live::runInBackground on a copy of the project), Übernehmen and the short's finish (captions,
// effect sounds) through Cli::Live::runForUser = one undo step each, the user's own. Lives in the command library
// (the app library does not link it); main.cpp hands it to MainWindow::setDirectBackend.

#include "app/DirectBackend.h"

class LiveHost;

class RealDirectBackend : public DirectBackend
{
public:
    explicit RealDirectBackend(LiveHost* host);

    JobHandle suggest(const QString& recipe, const QJsonObject& material, const QJsonObject& options,
                      const Progress& onProgress, const Done& onDone, QObject* receiver) override;
    JobHandle apply(const QJsonObject& session, const QJsonObject& choice, const Progress& onProgress,
                    const Done& onDone, QObject* receiver) override;
    QStringList recipes() const override; // all five
    bool canFinish(const QJsonObject& session) const override;
    JobHandle finish(const QJsonObject& session, const QJsonObject& applied, const Progress& onProgress,
                     const Done& onDone, QObject* receiver) override;

private:
    LiveHost* m_host;
};
