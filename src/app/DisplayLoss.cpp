// SPDX-License-Identifier: AGPL-3.0-only
#include "app/DisplayLoss.h"

#include <QtGlobal>
#include <cstdio>
#include <cstdlib>

namespace DisplayLoss {
namespace {

QtMessageHandler s_next = nullptr;

void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (s_next) s_next(type, ctx, msg); // the log keeps why the program ended
    if (type == QtWarningMsg && isLossMessage(msg)) {
        std::fflush(nullptr);
        std::_Exit(1); // what Qt's exit(1) would return, without destroying anything under running threads
    }
}

} // namespace

bool isLossMessage(const QString& message)
{
    // Qt 6 xcb: "The X11 connection broke (error %d). Did the X11 server die?" and "The X11 connection broke: %s
    // (code %d)"
    return message.startsWith(QLatin1String("The X11 connection broke"));
}

void install()
{
    s_next = qInstallMessageHandler(handler);
}

} // namespace DisplayLoss
