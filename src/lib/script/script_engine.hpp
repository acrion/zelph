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

#pragma once

#include "network/network_types.hpp" // For network::Node

#include <functional>
#include <string>
#include <vector>

struct JanetAbstractType;
struct JanetBuffer;

// This condition mirrors the state in which janet.h defines Janet as a union: on every target
// where it employs NaN-boxing, which is JANET_NANBOX_64 on 64-bit platforms listed in janet.h
// (including aarch64 since Janet 1.42.0) and JANET_NANBOX_32 when using Emscripten. Refer to
// _deps/janet-src/src/include/janet.h.
#if defined(__x86_64__) || defined(_WIN64) || defined(__riscv) || defined(__aarch64__) || defined(_M_ARM64) || defined(__EMSCRIPTEN__)
union Janet;
#else
struct Janet;
#endif

namespace zelph
{
    namespace network
    {
        class Reasoning;
    }

    class ScriptEngine
    {
    public:
        // Pass the reasoning engine so the script can operate on the graph
        explicit ScriptEngine(network::Reasoning* reasoning);
        ~ScriptEngine();

        // Setup the Janet environment and PEG grammar
        void initialize();

        static std::string get_janet_version();

        // Parse zelph syntax to Janet AST
        //
        // `pattern` indicates that the text represents a fact pattern
        // associated with a command (.node, .explain, .prune-*, .mermaid)
        // rather than a statement: it names a node, so a condition operator
        // written out within it is not refused as one that a statement
        // would assert.
        std::string parse_zelph_to_janet(const std::string& input, bool pattern = false) const;

        // Whether a statement writes a condition operator anywhere outside
        // the rules it mentions -- `¬`, `≈`, `⁺`/`∗`, or any of these
        // expressed explicitly as the fact they construct. A text that fails
        // to parse writes none, and neither does a rule typed as the
        // statement itself: its operators occupy its own slots.
        bool carries_condition_operator(const std::string& input) const;

        // Execute Janet code (either raw or transformed zelph AST)
        // is_zelph_ast determines how the output is handled/printed
        void process_janet(const std::string& code, bool is_zelph_ast);

        // Execute a Janet source file as a whole program, mirroring the janet
        // CLI: the file is evaluated in a fresh environment (which inherits
        // all zelph/... bindings), then its main function - if defined - is
        // called with the script path followed by args. Runs as the root
        // task of the Janet event loop, so ev/... (spawn-thread, channels,
        // timers) works. Relative imports like (use ./foo) resolve against
        // the script's directory.
        void run_janet_script(const std::string& path, const std::vector<std::string>& args = {});

        // Evaluate an expression and return a single Node (used for patterns/pruning).
        // With quiet = true, Janet's own stack trace is suppressed and the
        // failure is reported only through the thrown exception. Needed
        // wherever a failing evaluation is a normal outcome that the caller
        // handles (.explain TRIES several readings of its argument);
        // janet_dostring prints the trace before it returns, so no catch
        // downstream can take it back.
        /// `resolving_pattern` says the code denotes a node to LOOK UP rather
        /// than a statement to assert -- what every command that takes a
        /// printed fact pattern needs, including the destructive ones. Without
        /// it a pattern the graph holds as known-wrong cannot be addressed at
        /// all, because building it is an assertion and Zelph::fact refuses to
        /// contradict the graph.
        network::Node evaluate_expression(const std::string& janet_code, bool quiet = false, bool resolving_pattern = false);

        // Inject arguments into the script environment (for script files with args)
        void set_script_args(const std::vector<std::string>& args);

        // Forget the variables the current block has named, ensuring that any
        // subsequent reference to a symbol creates a new one. A load operation
        // that supersedes the graph calls it: the ids of those variables might
        // correspond to nodes within the loaded graph.
        void clear_scoped_variables();

        // Forget the nets generated by zelph/nn-compile, rendering any prior
        // handle acquisition invalid. A load operation that supersedes the
        // graph invokes this behaviour: the compiled net's neurons are node
        // identifiers, potentially mapping to different nodes inside the
        // loaded graph. A thread currently evaluating such a net concludes
        // its execution with that net.
        void discard_compiled_nets();

        // Handler backing the Janet function zelph/import. It delegates to
        // the REPL's .import implementation (path resolution including the
        // standard library, .zph line processing, argument passing). Set by
        // Interactive once the CommandExecutor exists.
        using ImportHandler = std::function<void(const std::string& path, const std::vector<std::string>& args)>;
        void set_import_handler(ImportHandler handler);

        // Handler backing zelph/save and zelph/load. Receives a full REPL
        // command vector (e.g. {".save", "file.bin"}) and delegates to
        // CommandExecutor::execute, so the Janet functions share every check
        // and side effect with the interactive commands. Set by Interactive
        // once the CommandExecutor exists.
        using CommandHandler = std::function<void(const std::vector<std::string>& cmd)>;
        void set_command_handler(CommandHandler handler);

        // Predicate consulted before echoing input back: the parsed form of a
        // zelph statement and the result value of inline Janet code. Set by
        // Interactive to suppress these echoes inside a MODULE while keeping
        // them for the session -- typed, piped, or a script named on the
        // command line (see ScriptRole in repl_state.hpp). Query answers
        // (apply_rule), deductions, keyword-handler output and explicit
        // printing (print, ...) are not affected. Unset means: always echo.
        //
        // The echo is also what records the node a bare `.node`, `.mermaid` or
        // `.explain` falls back to (string::node_to_string stores it), so
        // suppressing it removes that fallback as well.
        using EchoPredicate = std::function<bool()>;
        void set_echo_predicate(EchoPredicate predicate);

        // What `zelph/auto-run?` answers: whether the host performs
        // inference following the processing of the input line, a behaviour
        // that gets disabled upon module import. Configured by Interactive; if
        // not set, there is no host that runs, and the binding answers
        // false.
        using AutoRunPredicate = std::function<bool()>;
        void set_auto_run_predicate(AutoRunPredicate predicate);

        // Before a binding that removes graph content (zelph/prune-fact)
        // proceeds to remove anything, it asks this, which throws while the
        // session holds a partial load; in such a case, the commands that
        // remove content are blocked as well. The bindings that tell what
        // such a removal takes or leaves (`zelph/holders`,
        // `zelph/dependents`) also ask it. Set through Interactive; unset,
        // nothing is refused.
        using FullGraphGuard = std::function<void(const std::string& name)>;
        void set_full_graph_guard(FullGraphGuard guard);

        void toggle_janet_logging();

        std::string get_janet_logging_status() const;

        bool has_keyword(const std::string& keyword) const;

        bool invoke_keyword(const std::string& keyword, const std::string& text, const bool force);

        // Whether an inline `%` expression is complete, as determined by
        // Janet's reader, which processes one line at a time:
        // start_inline_janet takes the first line, and continue_inline_janet
        // handles each subsequent line until one returns true. That is false
        // exclusively during the time when a form or a string remains
        // unclosed; a syntax error is treated as complete, thus reported at
        // runtime. The reader is dropped once processing finishes, while
        // drop_inline_janet drops it if the expression is abandoned. Executes
        // on the thread whose Janet VM this engine owns.
        bool start_inline_janet(const std::string& line);
        bool continue_inline_janet(const std::string& line);
        void drop_inline_janet();

        // Whether a statement is complete, or is pending the next line.
        bool is_zelph_complete(const std::string& code) const;

        ScriptEngine(const ScriptEngine&)            = delete;
        ScriptEngine& operator=(const ScriptEngine&) = delete;

    private:
        class Impl;
        Impl* const _pImpl;

        // The statement that `parse_zelph_to_janet` processes, after its
        // islands have been expanded.
        std::string transform_statement(const std::string& expanded, bool pattern) const;

        static const JanetAbstractType zelph_node_type;
        static void                    zelph_node_tostring(void* p, JanetBuffer* buffer);
        static int                     zelph_node_compare(void* p1, void* p2);
        static int                     zelph_node_hash(void* p, size_t size);
        static Janet                   zelph_wrap_node(network::Node n);
        static network::Node           zelph_unwrap_node(Janet val);
    };
}
