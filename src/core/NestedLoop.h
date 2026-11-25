// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Code that runs events while it waits (a progress dialog's processEvents loop, a drag, a menu's or dialog's exec):
// the window must not build its widgets again meanwhile (MainWindow::rebuildUi) - the code on the stack still uses
// them. A Guard marks such a loop that Qt does not count itself (processEvents, a platform's own drag loop);
// running() also sees every QEventLoop::exec above the main loop.
namespace NestedLoop {

class Guard {
public:
    Guard();
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// A nested loop runs now (a Guard is alive, or the GUI thread is in an event loop inside the main loop)
bool running();
// Only a Guard's loop (a measuring loop's processEvents, a drag): the app's Live server refuses changes meanwhile.
// Not every exec: a modal dialog is asked for on its own, other loops (a test's wait) deliver requests as before.
bool guarded();

} // namespace NestedLoop
