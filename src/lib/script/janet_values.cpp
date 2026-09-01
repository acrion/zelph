/*
Copyright (c) 2025, 2026 acrion innovations GmbH
Authors: Stefan Zipproth, s.zipproth@acrion.ch

This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org

zelph is offered under a commercial and under the AGPL license.
For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.

AGPL licensing:

zelph is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

zelph is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with zelph. If not, see <https://www.gnu.org/licenses/>.
*/

#include "script/script_engine_impl.hpp"

#include "network/reasoning.hpp"

#include <janet.h>

#include <mutex>
#include <sstream>
#include <string>

namespace zelph
{
    // What Janet wrote about the call that just failed, and an empty sink
    // afterwards. Returns nothing when the sink was never installed, which is
    // the honest answer: then the trace went to stderr as it always did.
    std::string ScriptEngine::Impl::take_err_trace() const
    {
        if (_err_sink == nullptr) return {};
        std::string text(reinterpret_cast<const char*>(_err_sink->data), static_cast<size_t>(_err_sink->count));
        _err_sink->count = 0;
        return text;
    }

    // The default, and what every Janet entry point that is not deciding for
    // itself has to call: the sink is one buffer for the whole engine, so a
    // trace left standing in it is prepended to whatever fails next. Passing
    // it on is also what these callers did before there was a sink.
    void ScriptEngine::Impl::flush_err_trace() const
    {
        const std::string trace = take_err_trace();
        if (!trace.empty()) std::fputs(trace.c_str(), stderr);
    }

    // Call a Janet function on a fiber of its own, with that fiber rooted for
    // the duration of the call. Use this instead of janet_pcall anywhere the
    // call can happen while the VM is already running -- which is every call
    // site here, because zelph/import and zelph/run-script let a Janet script
    // re-enter statement processing.
    //
    // janet_pcall does NOT root the fiber it creates, and janet_collect marks
    // only janet_vm.root_fiber plus the explicit roots, reaching nested fibers
    // through the parent's `child` link -- which janet_pcall does not set.
    // janet_continue_no_check adopts a fiber as root_fiber only while there is
    // none, so a nested call runs on a fiber that no root can reach: the first
    // collection inside it frees the fiber that is currently running, and
    // everything read afterwards is a use-after-free. The crash then surfaces
    // wherever the callee next touches its own stack, arbitrarily far from
    // here, and moves or disappears with any change to allocation timing.
    //
    // Found as a segfault on `.import decimal-arithmetic` followed by
    // `.import symbolic-core`: building a rule allocates enough to trigger a
    // collection inside the thunk. run_janet_script roots its fiber by hand
    // for exactly this reason.
    JanetSignal ScriptEngine::Impl::pcall_rooted(JanetFunction* const fun, const int32_t argc, const Janet* const argv, Janet* const out)
    {
        // Nothing is rooted between creating the fiber and rooting it, so no
        // allocation may happen in between either.
        const int         gc_handle = janet_gclock();
        JanetFiber* const fiber     = janet_fiber(fun, 64, argc, argv);
        if (!fiber)
        {
            janet_gcunlock(gc_handle);
            *out = janet_cstringv("could not create fiber");
            return JANET_SIGNAL_ERROR;
        }
        janet_gcroot(janet_wrap_fiber(fiber));
        janet_gcunlock(gc_handle);

        const JanetSignal sig = janet_continue(fiber, janet_wrap_nil(), out);

        // `out` is reachable through the fiber (last_value), so the caller
        // must not allocate before reading it -- the same contract janet_pcall
        // has, and the reason run_janet_script extracts its error text first.
        janet_gcunroot(janet_wrap_fiber(fiber));
        return sig;
    }

    bool ScriptEngine::Impl::echo_enabled() const
    {
        return !_echo_predicate || _echo_predicate();
    }

    void ScriptEngine::Impl::clear_scoped_variables()
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        _scoped_variables.clear();
    }

    bool ScriptEngine::Impl::has_scoped_variables()
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        return !_scoped_variables.empty();
    }

    std::string ScriptEngine::Impl::format_janet(Janet j)
    {
        JanetString   desc = janet_description(j);
        JanetByteView view = {desc, janet_string_length(desc)};
        return std::string(reinterpret_cast<const char*>(view.bytes), view.len);
    }

    void ScriptEngine::Impl::log_janet_call(const std::string& func_name, int32_t argc, Janet* argv, bool is_entry, Janet ret) const
    {
        if (!_log_janet_functions) return;

        std::ostringstream oss;
        oss << func_name << " inputs: ";
        for (int32_t i = 0; i < argc; ++i)
        {
            if (i > 0) oss << " ";
            oss << format_janet(argv[i]);
        }

        if (!is_entry)
            oss << " output: " << format_janet(ret);

        _n->diagnostic(oss.str());
    }

    // Converts Janet Types to Nodes.
    network::Node ScriptEngine::Impl::resolve_janet_arg(Janet arg)
    {
        if (janet_checktype(arg, JANET_ABSTRACT) || janet_checktype(arg, JANET_NUMBER))
        {
            return zelph_unwrap_node(arg);
        }
        else if (janet_checktype(arg, JANET_STRING))
        {
            // It's a standard named Node (Atom)
            const uint8_t* str  = janet_unwrap_string(arg);
            std::string    wstr = reinterpret_cast<const char*>(str);
            return _n->node(wstr, _n->lang());
        }
        else if (janet_checktype(arg, JANET_SYMBOL))
        {
            // It's a Variable
            const uint8_t* sym   = janet_unwrap_symbol(arg);
            std::string    s_sym = reinterpret_cast<const char*>(sym);

            std::lock_guard<std::mutex> lock(_state_mutex);
            if (_scoped_variables.count(s_sym)) return _scoped_variables[s_sym];

            network::Node v = _n->var();
            _n->set_name(v, s_sym, _n->lang(), false);
            _scoped_variables[s_sym] = v;
            return v;
        }
        return 0;
    }

    // Read-only variant: resolves strings to existing nodes without creating new ones.
    // Returns 0 if the node does not exist. Used by zelph/exists, zelph/sources, zelph/targets.
    network::Node ScriptEngine::Impl::resolve_janet_arg_no_create(Janet arg) const
    {
        if (janet_checktype(arg, JANET_ABSTRACT) || janet_checktype(arg, JANET_NUMBER))
        {
            return zelph_unwrap_node(arg);
        }
        else if (janet_checktype(arg, JANET_STRING))
        {
            const uint8_t* str  = janet_unwrap_string(arg);
            std::string    wstr = reinterpret_cast<const char*>(str);

            // Check regular named nodes
            network::Node n = _n->get_node(wstr, _n->lang());
            if (n) return n;

            // Check core nodes (e.g. "~", "=>", "in", "..")
            return _n->get_core_node(wstr);
        }
        return 0;
    }
}
