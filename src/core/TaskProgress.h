// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Progress and cancel of a long step that runs deep inside a command (op mixer target_lufs measures the whole mix
// several times): the caller that shows a progress dialog (the Studio page's "Lautheit auf Ziel" through the user
// runner) opens a Scope on its thread; the step reports through report() and stops when it returns false (the op
// then fails with CANCELED, the user runner takes the open undo step back). Without a Scope (the command line, MCP)
// report() does nothing and always goes on.

#include <functional>

namespace TaskProgress {

using Fn = std::function<bool(double)>; // 0..1; false = cancel

namespace detail {
inline thread_local const Fn* t_current = nullptr;
}

struct Scope {
    explicit Scope(const Fn& fn) : m_fn(fn), m_old(detail::t_current) { detail::t_current = &m_fn; }
    ~Scope() { detail::t_current = m_old; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Fn m_fn;
    const Fn* m_old;
};

// True when a caller on this thread follows the progress
inline bool active() { return detail::t_current && *detail::t_current; }
// Reports `fraction` (0..1); false = the caller canceled
inline bool report(double fraction) { return !active() || (*detail::t_current)(fraction); }

} // namespace TaskProgress
