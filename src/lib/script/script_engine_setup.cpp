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
#include "string/string_utils.hpp"

#include <janet.h>

#include <exception>
#include <string>
#include <thread>

namespace
{
    // Every zelph function reachable from Janet is registered through this.
    //
    // A C++ exception must never leave a Janet C function. Janet unwinds with
    // setjmp/longjmp, so an exception travelling out through its frames skips
    // Janet's own cleanup and leaves the VM in a state whose NEXT failure has
    // nothing to do with what caused it. That is not a theoretical concern: a
    // statement refused by fact() -- "R is transitive, A R B, B R C => A R C",
    // which the engine correctly declines -- poisoned the session, and a later
    // ".import math" then died three imports down with a bare "Janet error"
    // and a Janet stack frame, naming a line of the standard library that was
    // perfectly correct. The session had to be restarted to make it go away.
    //
    // The message is turned into a Janet value BEFORE the panic and the catch
    // block is left first, so nothing with a destructor is alive across the
    // longjmp -- and the message survives as a proper string, which is what
    // turns "Janet error" back into what actually went wrong.
    template <JanetCFunction Fn>
    Janet guarded(int32_t argc, Janet* argv)
    {
        Janet message = janet_wrap_nil();
        try
        {
            return Fn(argc, argv);
        }
        catch (const std::exception& ex)
        {
            message = janet_cstringv(ex.what());
        }
        catch (...)
        {
            message = janet_cstringv("unknown error");
        }
        janet_panicv(message);
    }
}

namespace zelph
{
    ScriptEngine::Impl::Impl(network::Reasoning* n) : _n(n)
    {
        s_instance = this;
    }

    ScriptEngine::Impl::~Impl()
    {
        if (s_instance == this)
        {
            s_instance = nullptr;
            // The keywords go with the engine that has registered
            // them.
            string::set_keyword_spellings({}, {});
        }
        if (_janet_env)
        {
            for (auto& [kw, entry] : _keyword_handlers)
                janet_gcunroot(entry.handler);
            _keyword_handlers.clear();
            janet_gcunroot(_zelph_peg);
            if (_err_sink != nullptr) janet_gcunroot(janet_wrap_buffer(_err_sink));
            janet_deinit();
        }
    }

    void ScriptEngine::Impl::init()
    {
        _main_thread_id = std::this_thread::get_id();
        janet_init();
        _janet_env = janet_core_env(nullptr);
        _err_sink  = janet_buffer(256);
        janet_gcroot(janet_wrap_buffer(_err_sink));
        janet_setdyn("err", janet_wrap_buffer(_err_sink));
        register_zelph_functions();
        setup_module_paths();
        setup_script_runner();
        setup_rule_macro();
        setup_peg();
        setup_numbers();
    }

    void ScriptEngine::Impl::register_zelph_functions() const
    {
// Helper to handle platform-specific Janet definitions
// On Linux/x64, it's a macro expecting void*.
// On macOS, it's a function expecting JanetCFunction.
#ifdef JANET_NANBOX_64
        auto wrap = [](JanetCFunction f)
        { return janet_wrap_cfunction((void*)f); };
#else
        auto wrap = [](JanetCFunction f)
        { return janet_wrap_cfunction(f); };
#endif
        janet_def(_janet_env, "zelph/fact", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_fact>), "(zelph/fact s p o)\nCreate fact. A refuted marking of a ground fact, (zelph/fact F \"~\" \"refuted\"), is refused outside a rule build: write that claim with zelph/refute. A marking whose fact holds a variable is a pattern and stays allowed. "
                                                                                                    "A => fact the call creates whose consequence has a predicate that no condition binds is checked when the outermost Janet evaluation ends "
                                                                                                    "(a block, a statement with its islands, a keyword handler, a .janet file) and before any run or save within it: "
                                                                                                    "if it is still a rule in force then (neither a generator nor a statement holds it), it is removed and the block, the run or the save fails with \"zelph/fact: ...\". "
                                                                                                    "So is a rule zelph/mention wrote down that the call claims into force; its refusal takes back the claim, and the rule stays a mention. "
                                                                                                    "Main thread only.");
        janet_def(_janet_env, "zelph/refute", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_refute>), "(zelph/refute s p o)\nClaim that the fact does NOT hold: same node as zelph/fact, created with probability 0 so that Answer::is_wrong holds for it. This is what the statement \"¬(s p o)\" means outside a rule condition, and what \"(s p o) ~ refuted\" means on a line of its own. Refused if the graph already claims the fact; a fact that a rule or a query only writes down as a pattern is no claim and is refuted. Refuting a refuted fact again changes nothing. Main thread only.");
        janet_def(_janet_env, "zelph/path-guard", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_path_guard>), "(zelph/path-guard from to)\nRefuse a path marker whose two ends are both concrete, outside a rule and outside a Janet block: reachability is walked, not asserted. Emitted by the \"P⁺\"/\"P∗\" sugar BEFORE the pattern is built, since building it is what would assert the one step. Main thread only.");

        janet_def(_janet_env, "zelph/list", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_list>), "(zelph/list nodes...)\nCreate list from nodes (a Lisp-style cons list with the first node as outermost cell). Main thread only.");

        janet_def(_janet_env, "zelph/list-chars", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_list_chars>), "(zelph/list-chars str)\nCreate list from string characters.\nCharacters are reversed before building the cons list so that the least-significant\ncharacter (rightmost in the string) is the outermost cons cell.\nThis matches the compact <...> syntax and enables LSB-first arithmetic via recursion.\nEvery character is the node of that name in language \"zelph\", whatever .lang says, so a character list is the same node in every language. Main thread only.");
        janet_def(_janet_env, "zelph/set", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_set>), "(zelph/set nodes...)\nCreate a SET CONSTANT from elements, the `{...}` literal. Identified by its members, so the same elements always yield the same node, and membership cannot be extended. Main thread only.");

        janet_def(_janet_env, "zelph/collection", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_collection>), "(zelph/collection nodes...)\nCreate a COLLECTION from elements, the `@{...}` literal. A container with its own identity: two calls with the same elements yield two different nodes, and (member in collection) adds to it. With no elements it returns nil, the core node of the empty list, as zelph/set and zelph/list do. Main thread only.");

        janet_def(_janet_env, "zelph/conjunction", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_conjunction>), "(zelph/conjunction conditions...)\nCreate a rule's condition set, the `(cond, cond, ...)` comma list: a collection tagged `~ conjunction`. "
                                                                                                                  "Every condition must be readable as a fact pattern, or as a nested condition set; anything else is refused, "
                                                                                                                  "because a rule holding it can never fire. Main thread only.");

        janet_def(_janet_env, "zelph/resolve", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_resolve>), "(zelph/resolve name &opt lang)\nResolve a string to its node, creating it if needed. "
                                                                                                          "lang defaults to the current language (as set by .lang). On a thread other than the main thread it only looks the name up, and a name that has no node yet is refused there.");

        janet_def(_janet_env, "zelph/lookup", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_lookup>), "(zelph/lookup name &opt lang)\nThe node that already carries the name in lang (default: the current language), or nil. Unlike zelph/resolve it creates nothing. "
                                                                                                        "A variable carrying the name is not its node, and a core spelling (\"~\", \"=>\") gives the core node in every language. Allowed on every thread.");

        janet_def(_janet_env, "zelph/lookup-all", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_lookup_all>), "(zelph/lookup-all name &opt lang)\nEvery node that carries the name in lang (default: the current language), as a tuple of nodes, "
                                                                                                                "in the order of the ranking that elects the node the name resolves to: a Wikidata property before an item before any other node, then the lower Wikidata number, then the lower node id. "
                                                                                                                "A name that only one node carries gives that node alone, a name no node carries the empty tuple; never a variable or a core node. "
                                                                                                                "After a .name or set_name takeover, the node zelph/lookup returns need not come first. "
                                                                                                                "The keyword :incomplete instead, in every language, while the name sections of the network were loaded partially "
                                                                                                                "(a .load-partial that selected fewer name chunks than the file holds), until a load replaces the graph or .new. Creates nothing. Allowed on every thread.");

        janet_def(_janet_env, "zelph/var", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_var>), "(zelph/var &opt name)\nCreate a FRESH variable node and return it. "
                                                                                                  "A variable SYMBOL passed to zelph/fact is scoped to one evaluation of a Janet block; this node is a value, "
                                                                                                  "so the caller's own binding decides how far it reaches -- which is how conditions built in separate blocks "
                                                                                                  "join instead of forming a cross product. The optional name is display only (many variables may carry one name). Main thread only.");

        janet_def(_janet_env, "zelph/import", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_import>), "(zelph/import path & args)\nLoad and execute a script through the same machinery as the .import "
                                                                                                        "command: the path is resolved against the working directory first, then the zelph standard library; "
                                                                                                        "the .zph extension is optional. args are passed to the script as strings, available via (dyn :args). "
                                                                                                        ".janet files are rejected (use Janet's import/use/dofile). Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/save", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_save>), "(zelph/save file)\nSave the current network to a binary file, like the .save command. "
                                                                                                    "The filename must end with '.bin'. Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/load", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_load>), "(zelph/load file)\nLoad a saved network (.bin) or import a Wikidata JSON dump "
                                                                                                    "(.json/.json.bz2), like the .load command. "
                                                                                                    "A network file replaces the graph: every node, name, rule and cluster of the session goes, and with them the display "
                                                                                                    "registrations, the handles of compiled nets and the import registry. A dump is read from the .bin cache next to it when "
                                                                                                    "the graph is empty, which replaces the graph in the same way; otherwise the dump is added to the graph, and only an "
                                                                                                    "import into an empty graph writes the cache. The Janet environment is kept; a node a Janet value held "
                                                                                                    "names whatever has that id in the loaded graph, or nothing. Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/epoch", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_epoch>), "(zelph/epoch)\nAn integer that only grows within the process: every new engine (.new) advances it, as does every load that replaces the graph "
                                                                                                      "and every Wikidata dump imported into the running session. A Janet value that holds node ids or labels is current while the epoch "
                                                                                                      "is still the one it was computed under. Allowed on every thread.");

        janet_def(_janet_env, "zelph/run", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_run>), "(zelph/run &opt :quiet :derived)\nRun forward chaining to a fixed point, like the .run command. "
                                                                                                  "Needed when driving zelph as a library: facts and rules created from Janet only take effect once the engine has run, "
                                                                                                  "and outside the REPL neither .run nor auto-run is reachable. Returns nil. "
                                                                                                  "With :quiet the run prints nothing -- no banner, deduction, contradiction, note or finding -- and returns "
                                                                                                  "{:contradictions C :naf-final N}: C a tuple with one struct {:rule :conditions :conclusion :record :bindings} per distinct "
                                                                                                  "contradiction the run met, in the order first met (:record is :new, :existing or :none); N a tuple of {:fact F :rule R}, "
                                                                                                  "one per fact found no longer justified while negation records are kept (check mode or .negation-records), "
                                                                                                  "which check mode then does not raise for. With :derived as well (in either order), the struct adds :derived D: a tuple, in the order derived, "
                                                                                                  "of {:fact :rule :premises :absent :bindings} per derived fact, :premises the facts its positive conditions matched (by node id), "
                                                                                                  ":absent the patterns of its negated conditions, found absent; :derived without :quiet is refused. "
                                                                                                  "Any other argument is refused. Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/run-once", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_run_once>), "(zelph/run-once &opt :quiet :derived)\nRun a single inference pass, like the .run-once command. "
                                                                                                            "Derives what one application of the rules yields instead of iterating to a fixed point. Returns nil. "
                                                                                                            "With :quiet (and :derived), what (zelph/run :quiet) (and :derived) returns; a single pass never re-tests the negation records, so :naf-final is always empty. "
                                                                                                            "Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/run-delta", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_run_delta>), "(zelph/run-delta &opt :quiet :derived)\nRun inference seeded by the facts created since the previous run, like the .run-delta command. "
                                                                                                              "Skips the pass over the whole graph for the rules that can be seeded, which is what makes assert-then-reason loops practical; "
                                                                                                              "a rule with a negated condition still takes a classic pass over the facts its positive conditions match at each negation level, and a rule that cannot be seeded takes one in every iteration. "
                                                                                                              "Requires an earlier run, an unchanged rule set and semi-naive evaluation; otherwise it falls back to a full pass. Returns nil. "
                                                                                                              "With :quiet (and :derived), what (zelph/run :quiet) (and :derived) returns, and no note about falling back to a full pass; a full pass returns all it derived. "
                                                                                                              "Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/auto-run?", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_auto_run_p>), "(zelph/auto-run?)\nWhether the host runs inference after the input that is being processed: the effective value of the .auto-run switch. "
                                                                                                               "False while a module is imported (.import, zelph/import), whose single run follows its last line, "
                                                                                                               "and after .load or .load-partial, which switch auto-run off. Main thread only.");

        janet_def(_janet_env, "zelph/strata", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_strata>), "(zelph/strata)\nThe negation levels of the rules in force, as the .strata command shows them, without running anything. "
                                                                                                        "Returns an array with one table per rule, in no particular order: :rule the rule node, :level its negation level counted from 1 "
                                                                                                        "(absent for a rule without a negated condition), :component a number shared by the rules of one strongly connected component, "
                                                                                                        "and :stratifiable, false when that component negates what it derives. "
                                                                                                        "A rule over predicates -- one that only extends the predicates it reads, as (R is transitive, A R B, B R C) => (A R C) does -- "
                                                                                                        "has :transparent true; while one is in force, the levels compare predicates alone, not predicate and object, "
                                                                                                        "so two rules that share a predicate depend on each other whatever their objects. Main thread only.");
        janet_def(_janet_env, "zelph/rules", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_rules>), "(zelph/rules)\nThe rules in force with their parts, as the .list-rules command lists them, without running anything. "
                                                                                                      "Returns a tuple sorted by rule node, one struct per rule: :rule the rule node, :conditions its plain positive conditions, "
                                                                                                      ":negated the patterns its negated conditions negate (P for the condition ¬(P)), :guards its != conditions, :paths its path conditions, "
                                                                                                      ":neural its ≈ conditions and :heads its consequences (the core node ! for a denial), each a tuple of nodes sorted by node. "
                                                                                                      "A rule that a statement only mentions is not in force and is not listed. Creates nothing. Main thread only.");
        janet_def(_janet_env, "zelph/defeated", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_defeated>), "(zelph/defeated)\nReturn an array of [fact rule] tuples, one per fact the latest run found no longer justified: "
                                                                                                            "derived by that rule under a negated premise that has since come to hold, and derived by nothing else. "
                                                                                                            "These are the facts the run's warning names while .negation-records is on, or its error in check mode; "
                                                                                                            "the facts stay in the graph. Empty after a run that found none, after every run while neither is on, and after a load that replaced the graph. "
                                                                                                            "(zelph/negation-records true) switches the records on. Main thread only.");
        janet_def(_janet_env, "zelph/negation-records", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_negation_records>), "(zelph/negation-records &opt on)\nRead, or with true or false set, whether a run records each fact a rule with a negated condition derives, "
                                                                                                                            "as the .negation-records command does. With the records on, a run warns when a later fact defeats such a conclusion, and (zelph/defeated) returns what it named. "
                                                                                                                            "Returns the state afterwards. Off by default, because every record costs memory (see .help .negation-records); "
                                                                                                                            "switching off drops the records, unless check mode keeps them. The switch is a setting of the session: a load keeps it, "
                                                                                                                            "and discards the records, which name the nodes of the graph it replaced. Main thread only.");
        janet_def(_janet_env, "zelph/explain", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_explain>), "(zelph/explain fact)\n(zelph/explain s p o & objs)\nThe proof .explain prints, as data, or nil when there is no such fact. "
                                                                                                          "The fact is resolved without creating anything and taken exactly as given; a statement whose text holds a comma list, such as a rule with several conditions, can only be named by its node. "
                                                                                                          "Each step is a struct: :fact the node; :status one of :derived :axiom :unfounded :rule-pattern :mentioned :rule-mentioned :truncated; :rule the => fact of a derived step; "
                                                                                                          ":premises the steps of its positive conditions; :absent the patterns of its negated conditions, found absent; :walked the patterns of its path conditions, "
                                                                                                          "and :walk-edges, per path condition, the steps of the derived facts its walk passes; :cycle-leaf; :more, true when the root has a second justification; "
                                                                                                          ":bindings the rule's variables as the step binds them. :unfounded is a fact that rules could derive but that no derivation supports now: "
                                                                                                          "a premise was removed, or a negated premise has come to hold since. "
                                                                                                          "The proof is the complete one while its search stays within the session's .explain-budget; otherwise it is the proof found within the default depth of .explain, "
                                                                                                          "cut there by :truncated steps, and the root carries :fallback true. Main thread only.");
        janet_def(_janet_env, "zelph/prune-fact", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_prune_fact>), "(zelph/prune-fact fact)\n(zelph/prune-fact s p o & objs)\nWithdraw the claim of one fact, as the ground form of .prune-facts does. "
                                                                                                                "The fact is resolved without creating anything and taken exactly as given, so a fact with further objects is another fact. "
                                                                                                                "Returns :removed when the node went, together with the statements that hold it (see zelph/holders); :unclaimed when it stays as part of a rule or as a mention, "
                                                                                                                "while what holds it outside rule text goes; nil when nothing claims it: no such fact, only a rule's pattern or a mention, or a refuted fact. "
                                                                                                                "The relation-type declaration of a core predicate (~ ~ ->) is kept, nil, with the line .prune-facts prints for it. "
                                                                                                                "What was derived from the fact stays (see .help .prune-facts). A removed node loses its annotations, a node that stays keeps them. "
                                                                                                                "Dropping the active cluster does not bring a removed fact back. "
                                                                                                                "Refused for a node that is no fact and for a fact that holds a variable (a pattern is the business of .prune-facts). Blocked on a partially loaded graph, as .prune-facts is. Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/holders", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_holders>), "(zelph/holders fact)\n(zelph/holders s p o & objs)\nThe statements that claim the fact by nesting it, as a tuple of nodes sorted by node, each once; "
                                                                                                          "the empty tuple when nothing holds it, nil when there is no such fact. The fact is resolved as zelph/prune-fact resolves it. "
                                                                                                          "These are what zelph/prune-fact takes with the fact, or withdraws where a holder is part of a rule. "
                                                                                                          "Left out: rule text and mentions (a rule, an opaque statement or a zelph/mention that names the fact), the engine's bookkeeping, a pattern with a variable, "
                                                                                                          "and the fact's own markings. Only direct holders: a statement that nests a holder goes with the holder. "
                                                                                                          "A membership in a container the prune takes along (a set constant; an atom or a collection that no rule or mention names) is passed over: listed are the statements that name the container, which go with it, and the container goes too; so is a membership that is itself asked about. "
                                                                                                          "A membership in a container that a rule or a mention names is listed, as it is all that goes, also beside a container the prune takes along. "
                                                                                                          "A core predicate's relation-type declaration, which zelph/prune-fact keeps with whatever holds it, gives the empty tuple. Creates nothing. "
                                                                                                          "Refused, and blocked on a partially loaded graph, as zelph/prune-fact is. Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/dependents", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_dependents>), "(zelph/dependents fact)\n(zelph/dependents s p o & objs)\nThe facts that a rule in force derives from the fact in one step, as a tuple of nodes sorted by node, each once: "
                                                                                                                "a justification of each uses the fact, as a condition or as an edge a path condition walks over, while its other conditions, guards and negations hold. "
                                                                                                                "The empty tuple when nothing rests on the fact, nil when there is no such fact. The fact is resolved as zelph/prune-fact resolves it. "
                                                                                                                "Only direct dependents: a fact that rests on the fact only through another dependent is not listed. A fact with another justification is listed too, "
                                                                                                                "and so is one a statement also asserted, since zelph records no derivation. A denial and a rule that the fact blocks through a negation derive nothing from it. "
                                                                                                                "The search does .explain's work and is bounded by the session's .explain-budget: when the budget runs out first, the tuple ends with :truncated after the dependents found so far. "
                                                                                                                "Creates nothing. Refused, and blocked on a partially loaded graph, as zelph/prune-fact is. Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/cluster", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_cluster>), "(zelph/cluster &opt name)\nActivate a named cluster, or with nil / \"default\" deactivate cluster tracking; without an argument only report. "
                                                                                                          "Returns the name of the cluster that is active afterwards, or nil for the default. "
                                                                                                          "Nodes CREATED while a cluster is active are recorded in it, which is what makes zelph/cluster-drop a rollback. "
                                                                                                          "Unlike the .cluster command this prints nothing, so it can be used per question inside a loop. "
                                                                                                          "A name beginning with \"__\" is refused: those are the engine's scratch clusters. "
                                                                                                          "Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/cluster-drop", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_cluster_drop>), "(zelph/cluster-drop name)\nRemove every node recorded in the cluster, with its edges and names, and return how many were removed. "
                                                                                                                    "Nodes that already existed when the cluster was activated were never recorded, so a drop cannot remove them - which is what makes this safe as scratch space over a loaded graph. "
                                                                                                                    "A fact OUTSIDE the cluster that names a cluster node goes with it, as does every fact naming one of those and every rule one of them is a condition or a conclusion of (see .help .remove). "
                                                                                                                    "The default cluster cannot be dropped. Main thread only, and refused while a rule is being built.");

        janet_def(_janet_env, "zelph/clusters", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_clusters>), "(zelph/clusters)\nReturn an array of [name node-count] tuples, one per existing cluster. Main thread only.");

        janet_def(_janet_env, "zelph/query", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_query>), "(zelph/query node)\nExecute a query and return results as an array of tables.\nEach table maps variable symbols to their bound zelph/node values.\nTakes a zelph/fact containing variables.\n"
                                                                                                      "Resets the variable scope, so it is refused while a rule is being built: there the scope binds the rule's variables. Main thread only.");

        janet_def(_janet_env, "zelph/exists", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_exists>), "(zelph/exists s p o & objs)\nCheck whether the fact was CLAIMED -- asserted or derived -- without creating it. Returns boolean.\n"
                                                                                                        "A statement that only occurs as a rule's pattern is not claimed; ask zelph/mentioned for that.\n"
                                                                                                        "Nor is the engine's own bookkeeping, which no rule condition sees either: the mark of a rule's pattern, a contradiction record or one of its members.\n"
                                                                                                        "A fact carrying further objects satisfies the question, as it satisfies a rule condition. With a trailing :exact only the fact exactly as given counts: "
                                                                                                        "one hash probe, also when the answer is false, where the plain form walks every relation of the subject. Allowed on every thread.");

        janet_def(_janet_env, "zelph/mentioned", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_mentioned>), "(zelph/mentioned s p o & objs)\nCheck whether the fact NODE is present in the graph, whether or not anybody claimed it.\n"
                                                                                                              "True for a rule's own conditions and consequences, which zelph/exists reports as absent. Use this to\n"
                                                                                                              "inspect rule structure; use zelph/exists to ask about the data. Takes a trailing :exact as zelph/exists does.");

        janet_def(_janet_env, "zelph/name", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_name>), "(zelph/name node &opt lang :strict)\nReturn the name of a node in lang (default: the current language) as a string, or nil if unnamed. "
                                                                                                    "Without a name in lang it falls back to the node's name in English, then in zelph, then in any other language; "
                                                                                                    "with a trailing :strict it does not, and a node without a name in lang gives nil. A core node answers with its spelling in every language.");

        janet_def(_janet_env, "zelph/lang", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_lang>), "(zelph/lang &opt lang)\nThe current language, as .lang shows it. With an argument, set it as .lang <lang> does and return the previous one. "
                                                                                                    "The language decides what names resolve to and in which language zelph/resolve creates a node. Main thread only.");

        janet_def(_janet_env, "zelph/sources", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_sources>), "(zelph/sources predicate target)\nFind all subjects connected to target via predicate. Read-only.");

        janet_def(_janet_env, "zelph/targets", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_targets>), "(zelph/targets subject predicate)\nFind all objects connected from subject via predicate. Read-only.");

        janet_def(_janet_env, "zelph/degree", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_degree>), "(zelph/degree node &opt side)\nThe number of edges of node in the graph's adjacency, read without copying it: :in the incoming ones, :out the outgoing ones, "
                                                                                                        "both without a side. A fact points to its subject and its predicate, and its subject and objects point to it, so the :in of a node counts "
                                                                                                        "the facts it is the subject or the predicate of, and its :out those it is the subject or an object of. 0 for a node that does not exist. Allowed on every thread.");

        janet_def(_janet_env, "zelph/annotate", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_annotate>), "(zelph/annotate node key value)\nAttach the string value to node under the string key, after the values the key already holds; a value the key holds is not added again. "
                                                                                                            "Returns true when the value was added. Annotations are no part of the graph: no query, rule or name sees them, and annotating a rule's node is no mention of the rule. "
                                                                                                            ".save writes them and a .load of the file reads them back; a removal of the node erases them, and a name merge carries them to the node that remains. "
                                                                                                            "node is a node, an id or a name, and is never created. Main thread only.");

        janet_def(_janet_env, "zelph/annotations", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_annotations>), "(zelph/annotations node &opt key)\nThe annotations of node: a struct from each key to the tuple of its values, in the order they were added, "
                                                                                                                  "or with key the tuple of that key's values. Empty when there are none, also for a node that does not exist. Allowed on every thread.");

        janet_def(_janet_env, "zelph/unannotate", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_unannotate>), "(zelph/unannotate node key &opt value)\nRemove value from the values of key, or without value the key and all its values. "
                                                                                                                "Returns how many values were removed. Main thread only.");

        janet_def(_janet_env, "zelph/negate", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_negate>), "(zelph/negate pattern)\nThe negated condition over a fact pattern: creates and returns the fact\n((pattern) ~ negation), which a rule names as its condition.\nEquivalent to ¬(pattern) and to ((pattern) ~ negation) in zelph syntax.\nMain thread only.");

        janet_def(_janet_env, "zelph/rule*", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_rule_star>), "(zelph/rule* conditions & consequences)\nCreate an inference rule: the plain function behind the zelph/rule macro.\n"
                                                                                                          "conditions: array of fact nodes (the conjunction).\n"
                                                                                                          "consequences: one or more fact nodes to deduce.\n"
                                                                                                          "Returns the condition set node.\n"
                                                                                                          "Its arguments are evaluated before it runs, so it cannot tell which of them the call built: it marks nothing, "
                                                                                                          "and a collection among them is a value the rule refers to. Unlike zelph/rule it is a value that apply or map can take. "
                                                                                                          "Refused while a rule is being built. Main thread only.");

        janet_def(_janet_env, "zelph/build-rule", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_build_rule>), "(zelph/build-rule thunk)\nRun a thunk that builds one rule, as a parsed rule is built, and return what it returns. "
                                                                                                                "What the thunk builds into the rule is the rule's text: a collection it writes into it is the rule's own, and the ground parts of the rule it returns "
                                                                                                                "(every => fact over the condition set it returns, or over the condition of the => fact it returns) are rule patterns, not claims. "
                                                                                                                "Unlike a parsed rule, a duplicate is not rolled back; a thunk that fails leaves nothing of what it built. "
                                                                                                                "Outside a parsed statement, the => fact it returns is asserted, so a rule zelph/mention wrote with the same text comes into force. "
                                                                                                                "The zelph/rule macro builds its rule the same way (zelph/build-rule-parts); "
                                                                                                                "wrap a rule written by hand with zelph/fact in it. Refused while another rule is being built. Main thread only.");

        janet_def(_janet_env, "zelph/build-rule-parts", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_build_rule_parts>), "(zelph/build-rule-parts thunk)\nWhat the zelph/rule macro expands to: run a thunk that returns a rule's conditions and consequences as one tuple, "
                                                                                                                            "[conditions ;consequences], and build the rule from them inside the same build, as zelph/build-rule builds a rule. "
                                                                                                                            "Returns the condition set node. If the thunk or the build fails, nothing the thunk built is kept. "
                                                                                                                            "Refused while another rule is being built. Not needed in hand-written Janet: use zelph/rule. Main thread only.");

        janet_def(_janet_env, "zelph/dedup-rule", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_dedup_rule>), "(zelph/dedup-rule thunk &opt :focus)\nRun a thunk that builds one whole rule and returns its => fact, and return the rule node. "
                                                                                                                "Every zelph/var of the rule is created inside the thunk, so that the rollback of a duplicate keeps none of them. "
                                                                                                                "If the graph already holds a rule that is the same up to renaming of its variables, "
                                                                                                                "the newly built one is rolled back and the existing node returned instead. "
                                                                                                                "A thunk that returns anything but a => fact is refused, and nothing it built is kept: "
                                                                                                                "a plain fact, and also a condition set such as zelph/rule* returns. "
                                                                                                                "With :focus, the thunk returns a tuple of the => fact and one part of it -- its condition, the =>, or one of its consequences -- "
                                                                                                                "and the same part of the rule kept is returned, the existing rule's when the new one is rolled back: "
                                                                                                                "the parser's form for a rule with the focus operator, *{(X p Y)} => (X q Y). "
                                                                                                                "Emitted around every parsed `... => ...` statement, and the one route that deduplicates a rule built in Janet. "
                                                                                                                "Outside a parsed statement, the rule kept is asserted, so a rule zelph/mention wrote with the same text comes into force. "
                                                                                                                "Refused while another rule is being built. Main thread only.");

        janet_def(_janet_env, "zelph/rule-text", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_rule_text>), "(zelph/rule-text thunk)\nRun a thunk that writes a rule which another statement mentions, and return what it returns. "
                                                                                                              "Mentioning a rule claims none of its parts: the ground parts of every rule the thunk creates are rule patterns, as a typed rule's are, "
                                                                                                              "and the collections it writes are that rule's own. A part that existed before keeps its status, "
                                                                                                              "and a mentioned rule is marked only as a part of another rule the thunk creates: unlike the rule zelph/mention returns, "
                                                                                                              "a rule zelph/rule-text returns and no statement holds is in force. "
                                                                                                              "In a statement that asks (it has a variable outside the rules it mentions), nothing is marked. "
                                                                                                              "zelph/fact calls in the thunk claim nothing, even in a Janet block. "
                                                                                                              "Emitted automatically around every parsed `... => ...` that stands inside another statement; "
                                                                                                              "wrap a mention written by hand with zelph/fact in it. Main thread only.");

        janet_def(_janet_env, "zelph/mention", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_mention>), "(zelph/mention s p o & objs)\n(zelph/mention thunk)\nWrite a statement without claiming it, and return it: the first form builds one fact, the second runs a thunk that builds the statement and returns it. "
                                                                                                          "What the call creates and the returned statement reaches is marked as written down, not asserted: no query or rule condition takes it as data "
                                                                                                          "until a claim of the same statement (a typed statement, zelph/fact in a Janet block, a firing, zelph_fact) makes it data. "
                                                                                                          "What existed before keeps its status, so a mention of an asserted statement still answers. "
                                                                                                          "A rule (a => fact) the call creates as the statement it returns is a mention too: it is not in force, and a rule over rules does not read it, "
                                                                                                          "until a claim of the same rule (a typed rule, zelph/fact in a Janet block, zelph_fact, zelph/dedup-rule, zelph/build-rule) puts it in force. "
                                                                                                          "A claim by zelph/fact or zelph_fact of a rule whose consequence predicate no condition binds is refused as a => fact the call creates is, and the rule stays a mention. "
                                                                                                          "A rule the thunk writes inside the statement it returns is rule text, "
                                                                                                          "a collection it writes is a value, and a refutation marking it writes refutes nothing. zelph/fact calls in the thunk claim nothing, even in a Janet block. "
                                                                                                          "In a parsed statement that asks, nothing is marked. A thunk that returns no node is refused, and nothing it built is kept. "
                                                                                                          "A prune of a claimed statement that a mention holds withdraws the claim and keeps the statement, as for a rule's pattern. Main thread only.");

        janet_def(_janet_env, "zelph/aside", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_aside>), "(zelph/aside fact)\nReturn fact, and record it as the fact a focus writes beside the node it hands on. "
                                                                                                      "Emitted by the parser around that fact. Within a parsed statement, the statement claims it as it claims what it writes, "
                                                                                                      "also when it was a rule's pattern before; within a rule or a statement that another statement mentions, or in an argument "
                                                                                                      "of an opaque predicate, it is marked with what the mention wrote, and a fact that existed before keeps its status. "
                                                                                                      "A fact that holds a variable is not recorded. Outside a parsed statement, as in a Janet block, it only returns fact. "
                                                                                                      "Main thread only.");

        janet_def(_janet_env, "zelph/query-text", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_query_text>), "(zelph/query-text thunk)\nRun the code of a query and return what it returns. "
                                                                                                                "Emitted by the parser. Within the parsed statement, what the query creates beneath an operator -- the operand, everything in it, "
                                                                                                                "the operator's own fact -- is a pattern, marked before the query is evaluated; a node that existed before keeps its status, "
                                                                                                                "and if the thunk fails, nothing it built is kept. What it creates that holds one of its variables is removed once the query "
                                                                                                                "is answered; what it creates without one stays. Outside a parsed statement, as in a Janet block, it only runs the thunk. "
                                                                                                                "Main thread only.");

        janet_def(_janet_env, "zelph/claim-text", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_claim_text>), "(zelph/claim-text thunk)\nRun the code of a claimed statement that carries a refutation or takes a node by reference, and return what it returns. "
                                                                                                                "Emitted by the parser. Within the parsed statement, a statement that fails -- in its code or in its claim at the end of the line, "
                                                                                                                "a refutation refused for a fact the graph or the statement claims, a rule's marking pattern of a claimed fact handed in by reference -- "
                                                                                                                "keeps nothing it built, and the rule patterns and refutations it changed are as before. "
                                                                                                                "Outside a parsed statement, as in a Janet block, it only runs the thunk. Main thread only.");

        janet_def(_janet_env, "zelph/operand", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_operand>), "(zelph/operand thunk)\nRun the code that builds a condition operator and its operand, and return what it returns. "
                                                                                                          "A fact the graph holds, a refuted one included, is looked up rather than asserted; anything else is built as zelph/fact builds it, "
                                                                                                          "and whether it is a pattern is decided by the rule or query around it. "
                                                                                                          "Emitted by the parser around every condition operator. Main thread only.");

        janet_def(_janet_env, "zelph/car", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_car>), "(zelph/car cell)\nReturn the first element (car) of a cons cell, or nil if not a cons cell.");
        janet_def(_janet_env, "zelph/cdr", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_cdr>), "(zelph/cdr cell)\nReturn the rest (cdr) of a cons cell. Returns the nil node for the last cell.");
        janet_def(_janet_env, "zelph/parse", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_parse>), "(zelph/parse fact)\nThe parts of a fact node as the tuple [subject predicate objects], objects a tuple of nodes in the order of their node ids, "
                                                                                                      "so the same fact always gives the same tuple. Every fact decomposes, a cons cell and the => fact of a rule included; "
                                                                                                      "a negated condition is the fact ((P) ~ negation), whose parts are P, ~ and (negation). nil for an atom, a variable, or anything else that is not a fact. "
                                                                                                      "Allowed on every thread.");

        janet_def(_janet_env, "zelph/register-keyword", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_register_keyword>), "(zelph/register-keyword keyword handler)\n(zelph/register-keyword open close handler)\n"
                                                                                                                            "Two-argument form: register a REPL syntax keyword. After the keyword is entered, subsequent "
                                                                                                                            "lines are accumulated verbatim until an empty line, then passed as a single string to handler.\n"
                                                                                                                            "Three-argument form: register an inline keyword (expression island). Whenever `open` occurs "
                                                                                                                            "inside a zelph statement, the text up to `close` is passed to handler, which must return a "
                                                                                                                            "zelph/node; the node replaces the island in the statement. Returning :incomplete extends the "
                                                                                                                            "island to the next occurrence of `close`, so nested delimiters work -- handlers must be free "
                                                                                                                            "of graph side effects until they accept their input. Main thread only.");

        janet_def(_janet_env, "zelph/closure", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_closure>), "(zelph/closure start predicate &opt include-start)\nTransitive closure following predicate "
                                                                                                          "forward (subject to object). include-start true gives the reflexive closure (SPARQL *).");

        janet_def(_janet_env, "zelph/closure-sources", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_closure_sources>), "(zelph/closure-sources target predicate &opt include-target)\nTransitive closure following "
                                                                                                                          "predicate backward (object to subject). include-target true gives the reflexive closure.");

        janet_def(_janet_env, "zelph/nn-connect", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_connect>), "(zelph/nn-connect from to &opt weight)\nCreate a raw weighted edge (synapse) from -> to, creating nodes as needed. "
                                                                                                                "Synapses live solely in the weight store and never appear in the graph's adjacency, so they are invisible to the reasoning engine by construction; any node is a safe neuron, including fact nodes and structural numbers. Main thread only.");

        janet_def(_janet_env, "zelph/weight", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_weight>), "(zelph/weight from to)\n"
                                                                                                        "Weight of the directed node pair from -> to. Returns the stored value if a "
                                                                                                        "synapse (or an explicitly stored fact probability) exists for the pair; "
                                                                                                        "1 if the pair is a real graph edge without a stored entry (the canonical "
                                                                                                        "default, e.g. for facts asserted with probability 1); nil if neither exists.");

        janet_def(_janet_env, "zelph/set-weight", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_set_weight>), "(zelph/set-weight from to w)\nSet the weight of an existing synapse or edge. Main thread only.");

        janet_def(_janet_env, "zelph/nn-compile", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_compile>), "(zelph/nn-compile layers &opt activation)\nCompile a feed-forward view of a sub-graph. layers: array of layer nodes, "
                                                                                                                "input first, output last. Neurons are the subjects of (neuron in layer) facts, ordered by node id. "
                                                                                                                "Returns an integer handle. The compiled net is a discardable cache; the graph stays the source of truth. Main thread only.");

        janet_def(_janet_env, "zelph/nn-nodes", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_nodes>), "(zelph/nn-nodes handle layer)\nNeurons of a compiled layer in index order (defines input/output vector order).");

        janet_def(_janet_env, "zelph/nn-eval", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_eval>), "(zelph/nn-eval handle inputs)\nForward pass; inputs/outputs are arrays of numbers in zelph/nn-nodes order. "
                                                                                                          "Hidden layers use ReLU, the output layer is linear.");

        janet_def(_janet_env, "zelph/nn-train", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_train>), "(zelph/nn-train handle inputs targets &opt learning-rate)\nOne SGD step on a single sample; returns the loss "
                                                                                                            "(0.5 * sum of squared errors) before the update. learning-rate defaults to 0.01.");

        janet_def(_janet_env, "zelph/nn-write-back", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_write_back>), "(zelph/nn-write-back handle)\nWrite the compiled net's weights back into the graph's edge-weight store, "
                                                                                                                      "so they survive .save and are picked up by future zelph/nn-compile calls. Allowed on any thread, like evaluation: it only overwrites the weights of the net's own synapses.");

        janet_def(_janet_env, "zelph/nn-snapshot", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_snapshot>), "(zelph/nn-snapshot handle)\nCopy the compiled net's weights out as an array of arrays of numbers, "
                                                                                                                  "one per layer transition, each row-major by post-synaptic unit: input i to unit j is at (+ (* j n-pre) i). "
                                                                                                                  "Use it to keep the best epoch of a training run: the criterion that says a run has passed its "
                                                                                                                  "optimum can only fire afterwards, so without a snapshot the saved weights are always some epochs past the good ones.");

        janet_def(_janet_env, "zelph/nn-restore", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_restore>), "(zelph/nn-restore handle snapshot)\nPut a zelph/nn-snapshot back into the compiled net. "
                                                                                                                "Shapes must match; synapses absent from the graph stay absent, since the mask belongs to the graph and not to the weights.");

        janet_def(_janet_env, "zelph/nn-connect-layers", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_connect_layers>), "(zelph/nn-connect-layers from-layer to-layer &opt scale seed)\nCreate raw synapses between all members of two layers "
                                                                                                                              "((neuron in layer) facts, ascending node id). Weights are uniform in [-scale, scale]; scale defaults to 0.1, scale 0 gives exact zeros. "
                                                                                                                              "seed defaults to 42 for reproducible initialization. Existing edges are left untouched, so trained weights survive re-wiring. "
                                                                                                                              "Returns the number of edges created. Main thread only.");

        janet_def(_janet_env, "zelph/nn-train-nodes", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_train_nodes>), "(zelph/nn-train-nodes handle inputs targets &opt learning-rate)\nOne SGD step, addressing neurons by node instead of by index. "
                                                                                                                        "inputs/targets are arrays whose elements are nodes (activation 1) or [node activation] pairs; all other neurons are 0. "
                                                                                                                        "A typical call encodes one fact: inputs [S P], targets [O]. Returns the loss before the update. learning-rate defaults to 0.01.");

        janet_def(_janet_env, "zelph/nn-eval-nodes", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_nn_eval_nodes>), "(zelph/nn-eval-nodes handle inputs &opt top-k)\nForward pass with node-addressed multi-hot input. Returns an array of [node score] "
                                                                                                                      "tuples for the output layer, sorted by descending score (ties by ascending node id), limited to top-k if given.");

        janet_def(_janet_env, "zelph/approx", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_approx>), "(zelph/approx pattern net-name)\nTag a fact pattern as a neural rule condition: creates (pattern nn net). "
                                                                                                        "Desugared form of ≈net(pattern). Returns the pattern node. Main thread only.");

        janet_def(_janet_env, "zelph/path", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_path>), "(zelph/path pattern mode)\nTag a one-step fact pattern as a transitive path condition: creates "
                                                                                                    "(pattern closure mode), where mode is \"one-or-more\" (P⁺) or \"zero-or-more\" (P∗). Desugared form of "
                                                                                                    "(X P⁺ Y). Returns the tag node, which is what a rule uses as its condition. Main thread only.");

        janet_def(_janet_env, "zelph/set-number-digits", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_set_number_digits>), "(zelph/set-number-digits digits)\nRegister the digit alphabet of the loaded number representation, as an "
                                                                                                                              "array of digit nodes or names in ascending order of value (e.g. [\"0\" \"1\"] for binary). "
                                                                                                                              "node_to_string then displays every canonical (no leading zero digit) nil-terminated cons list consisting solely of these digit "
                                                                                                                              "nodes as a decimal &-literal -- the inverse of the &-input syntax (zelph/number). All other "
                                                                                                                              "cons lists keep the generic <...> display. An empty array disables the feature. Names resolve in the current "
                                                                                                                              "language, whereas zelph/list-chars builds its lists from the characters in \"zelph\": register the digits of "
                                                                                                                              "that representation under .lang zelph, or pass the digit nodes. Main thread only.");

        janet_def(_janet_env, "zelph/no-selffact-sugar", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_no_selffact_sugar>), "(zelph/no-selffact-sugar preds...)\nExclude predicates from the self-fact display sugar: facts (X pred X) "
                                                                                                                              "with a registered predicate always render verbose as \"X pred X\", never as \":pred X\". "
                                                                                                                              "Additive across calls, so stacked modules can each register their own operators. "
                                                                                                                              "Input sugar (\":pred X\") keeps working regardless. Intended for term-forming "
                                                                                                                              "operators (+, eml, nand, ...), where subject == object is a hash-consing "
                                                                                                                              "coincidence rather than a request marker. Main thread only.");

        janet_def(_janet_env, "zelph/register-display-scheme", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_register_display_scheme>), "(zelph/register-display-scheme name open close &opt options)\nDeclare how this script's own notation is written, so node_to_string can "
                                                                                                                                          "render terms the way the script's parser reads them back. open/close enclose a rendering that DEVIATES from the default form "
                                                                                                                                          "(elided parentheses, a different numeral prefix); they are emitted verbatim, so use \"$( \" / \" )\" for padded output. "
                                                                                                                                          "options is a struct: :numeral-prefix replaces the default \"&\" of number literals inside the scheme; :name-first and "
                                                                                                                                          ":name-chars declare which characters a leaf name may start with and consist of. A term containing anything the scheme cannot "
                                                                                                                                          "write -- a foreign predicate, a set, a name outside that grammar -- is rendered in the default form instead, so the output "
                                                                                                                                          "always stays re-readable. Re-registering a name updates the scheme. Main thread only.");

        janet_def(_janet_env, "zelph/set-infix-display", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_set_infix_display>), "(zelph/set-infix-display scheme entries)\nRegister infix operators into a scheme declared by zelph/register-display-scheme. "
                                                                                                                              "entries is an array of [predicate precedence &opt associativity]; associativity is :left (default), :right or :none. Higher "
                                                                                                                              "precedence binds tighter; node_to_string omits the parentheses around an operand whose operator binds tightly enough. Additive "
                                                                                                                              "across calls. A predicate already claimed by any scheme is an error -- otherwise a term's rendering would depend on load order. "
                                                                                                                              "Registered operators are excluded from the self-fact display sugar (see zelph/no-selffact-sugar). Main thread only.");

        janet_def(_janet_env, "zelph/set-application-display", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_set_application_display>), "(zelph/set-application-display scheme predicates)\nRegister predicates whose facts are written in call notation: (S P O) renders as "
                                                                                                                                          "\"S(O)\", and the predicate name does not appear. The result is self-delimiting -- it never takes parentheses, and its "
                                                                                                                                          "argument never needs any. The head S must render as a bare name matching the scheme's leaf grammar; a composite head has "
                                                                                                                                          "no call notation, so such a term falls back to the default rendering. Shares the one-scheme-per-predicate namespace with "
                                                                                                                                          "zelph/set-infix-display, and excludes the predicates from the self-fact display sugar. Main thread only.");

        janet_def(_janet_env, "zelph/out", wrap((JanetCFunction)&guarded<&janet_cfun_zelph_out>), "(zelph/out text)\nEmit text through zelph's output pipeline (Out channel). Unlike Janet's "
                                                                                                  "print (raw stdout), the text reaches the REPL, the playground and test collectors, and is "
                                                                                                  "not subject to the input-echo suppression inside imported scripts -- use it for import-time notices.");
    }

    void ScriptEngine::Impl::setup_module_paths() const
    {
        const char* code = R"janet(
            (when-let [jp (os/getenv "JANET_PATH")]
              (each p (string/split (if (= :windows (os/which)) ";" ":") jp)
                (when (and p (not= p ""))
                  (array/push module/paths [(string p "/:all:.jimage") :image])
                  (array/push module/paths [(string p "/:all:.janet") :source])
                  (array/push module/paths [(string p "/:all:/init.janet") :source])
                  (array/push module/paths [(string p "/:all:.so") :native]))))
        )janet";
        Janet       out;
        janet_dostring(_janet_env, code, "module-paths", &out);
        flush_err_trace();
    }

    void ScriptEngine::Impl::setup_script_runner() const
    {
        // janet-CLI-compatible script runner: evaluate the file in a fresh
        // environment (inheriting the zelph bindings via the core env
        // prototype chain), then call its main function - if defined - with
        // the script path followed by the arguments.
        const char* code = R"janet(
                (defn zelph/run-script
                  `Run a Janet source file the way the janet CLI would: evaluate it in a fresh environment and call its main function (if defined) with the script path and arguments. Relative imports such as (use ./foo) resolve against the script's directory.`
                  [path & args]
                  # Fresh-process semantics per run: require caches modules
                  # process-wide, so without this, edits to files pulled in via
                  # (use ./foo) would be invisible to a repeated .import within
                  # the same session.
                  (loop [k :in (keys module/cache)]
                    (put module/cache k nil))
                  (def env (make-env))
                  (def subargs [path ;args])
                  (put env *args* subargs)
                  (dofile path :env env)
                  (when-let [entry (get env 'main)
                             main (or (get entry :value) (get (get entry :ref) 0))]
                    (when (function? main)
                      (main ;subargs)))
                  nil)
            )janet";

        Janet out;
        int   status = janet_dostring(_janet_env, code, "script-runner", &out);
        flush_err_trace();
        if (status != JANET_SIGNAL_OK) janet_stacktrace(nullptr, out);
    }

    void ScriptEngine::Impl::setup_rule_macro() const
    {
        // zelph/rule is a macro that ensures its argument forms execute within
        // the context of the rule's build process: since a function's
        // arguments are evaluated before execution, the function cannot know
        // which parts of the rule were constructed via its invocation. The
        // function is unquoted into the expansion, preventing a caller's own
        // binding of zelph/build-rule-parts from capturing the call. The thunk
        // only evaluates the argument forms; the rule is assembled inside that
        // function, based on the output the thunk returns.
        const char* code = R"janet(
                (defmacro zelph/rule
                  `Create an inference rule, built as a parsed rule is built. conditions: array of fact nodes (the conjunction). consequences: one or more fact nodes to deduce. Returns the condition set node. The argument forms run inside the rule's build (zelph/build-rule-parts, which builds as zelph/build-rule does), so what they build into the rule is the rule's text: a collection they write into it is the rule's own, and a ground fact they build into it is a rule pattern, not a claim. Once the call succeeds, a fact they build that the rule does not hold is a claim; a call that fails leaves nothing of what they built. A node built before the call stays what it was. A macro is no value that apply or map can take: use zelph/rule* there. Main thread only.`
                  [conditions & consequences]
                  ~(,zelph/build-rule-parts (fn [] [,conditions ,;consequences])))
            )janet";

        Janet out;
        int   status = janet_dostring(_janet_env, code, "rule-macro", &out);
        flush_err_trace();
        if (status != JANET_SIGNAL_OK) janet_stacktrace(nullptr, out);
    }

    void ScriptEngine::Impl::setup_peg()
    {
        // zelph Grammar:
        // 1. :atom -> Alphanumeric or Symbols (excluding reserved)
        // 2. :list-compact -> <123> Compact list: split into individual chars
        // 3. :list-nodes -> < a b > Node list: space-separated elements
        // 4. :set -> { ... }
        // 5. :nested -> ( ... ) Recursive statements inside ( ... )
        // 6. :quoted -> "..."
        // 7. :focused -> *Element (Returns the element instead of the container)
        // 8. :unquote -> ,identifier (Reference to a Janet variable)
        // 9. :selffact -> :pred X (self-fact sugar: desugars to (X pred X))
        // Returns tagged tuples like [:atom "val"], [:list-compact "val"] or [:nested sub-stmt...] for C++ processing
        std::string peg_setup = R"zph(
            # A group in parentheses and a whole line are read alike: one or
            # more statements separated by commas. A single statement gets
            # `tag` (:nested for a group, :root for a line), several are a
            # comma list of conditions (:conjunction). The grammar reads the
            # statements once and this names the result afterwards, in the
            # tree each case has always had -- see :tag-nested for what
            # deciding it beforehand cost.
            #
            # A group around one focused value is identical to that focus: a
            # group of one value is the value itself, thus `x says (*(a p b))`
            # is `x says *(a p b)` and evaluates to `a p b`. Kept as a group,
            # the focus was silently discarded at every position the group
            # stood (transform_arg), and no check that refuses a focus saw it:
            # `(*k) says (*(l p m))` asserted `k says (l p m)`, and a rule
            # that a statement holds treated `(*(X q Y))` as its plain
            # consequence.
            (defn zelph-comma-list [tag]
              (fn [head & tail]
                (cond
                  (not (empty? tail)) (array :conjunction head ;tail)
                  (and (= tag :nested) (= (length head) 2) (= (get-in head [1 0]) :focused)) (get head 1)
                  (array tag ;(slice head 1)))))

            # The value located under foci and parentheses that contain a
            # single value: `*x`, `**x`, `(x)` and `*((x))` all give x. A
            # caller concerned with whether the value was focused examines
            # the outer layer directly.
            (defn zelph-peel [value]
              (var inner value)
              (while (or (= (get inner 0) :focused)
                         (and (= (get inner 0) :nested) (= (length inner) 2)))
                (set inner (get inner 1)))
              inner)

            # A focus atop additional foci, or atop parentheses enclosing a
            # solitary value, equates to a focus on that value alone: `**x`,
            # `*(x)` and `*((x))` each denote `*x`, since a focus with nothing
            # around it is dropped and a group of one value is identical to
            # that value itself. This returns the value beneath the focus, or
            # nil if the value carries no focus. The arrow and the whole line
            # are read through it: matched solely in the shape `*x`, one
            # additional layer left the rule unchecked for a twin and its
            # ground parts claimed.
            (defn zelph-under-focus [value]
              (when (= (get value 0) :focused)
                (zelph-peel (get value 1))))

            # A focus on the arrow of a statement, "(a p b) *=> (c q d)", names
            # the rule: the node the statement evaluates to when no focus is
            # applied. The arrow is read as itself in this context, so every
            # reading of the tree takes the line as the rule it writes, which
            # is checked for a twin and keeps its ground parts as patterns. Read
            # as a focus, the line built the `=>` fact via `zelph/fact`: a rule
            # that nobody checked for a twin, with its ground parts claimed.
            # When paired with a second focus, the arrow keeps its own focus,
            # and the statement is refused as one bearing two foci.
            #
            # The arrow in parentheses or quotation marks, `(=>)` or `"=>"`,
            # refers to the node `=>` and is read as the bare arrow, either
            # with or without a focus. Written in this manner, the line missed
            # the parser's rule test, which demands the bare arrow: when
            # variables were present, it formed a query that left no rule,
            # while a ground one built the `=>` fact through `zelph/fact`, with
            # its parts claimed.
            (defn zelph-arrow [statement]
              (def written (get statement 2))
              (def arrow (zelph-peel written))
              (defn focused? [value] (= (get value 0) :focused))
              (when (and (>= (length statement) 4)
                         (= (get arrow 0) :atom)
                         (index-of (string (get arrow 1)) ["=>" "\"=>\""])
                         (or (not (focused? written))
                             (= 1 (count focused? (slice statement 1)))))
                (put statement 2 @[:atom "=>"]))
              statement)

            # A line that is one rule under a focus, "*((X p Y) => (X q Y))",
            # serves as the rule statement it holds: the focus lacks an
            # accompanying statement to hand a part to, and the line
            # evaluates to the rule regardless. Read as a value, the rule was
            # recorded as a mention that no entity names, a rule in force
            # without anyone having checked for a twin, thus each typing added
            # one.
            (defn zelph-line [line]
              (def rule (when (and (= (get line 0) :root) (= (length line) 2))
                          (zelph-under-focus (get line 1))))
              (if (and (= (get rule 0) :nested)
                       (>= (length rule) 4)
                       (= (get-in rule [2 0]) :atom)
                       (= (string (get-in rule [2 1])) "=>"))
                (array :root ;(slice rule 1))
                line))

            (def zelph-grammar
              ~{:ws (set " \t\r\f\n\0\v")
                :s* (any :ws)
                :s+ (some :ws)

                # > and < are reserved to act as delimiters.
                # , is reserved for unquoting Janet variables.
                # To use them as atoms, we define specific rules below.
                # A set matches single BYTES, hence the two characters beyond
                # ASCII stand outside it, each considered as a whole. `¬`
                # corresponds to the bytes C2 AC, and inside the set it
                # reserved both: every character from U+0080 to U+00BF starts
                # with C2 (°, µ, ½), and numerous others carry AC as a
                # continuation byte (€, the 京 in 北京), so none of them could
                # stand in a bare name. The no-break space U+00A0 also uses
                # C2 and stays reserved, just as previously: a line
                # containing one in place of a blank is rejected instead of
                # being interpreted as a single name that holds the blanks.
                :reserved (choice (set " \t\r\n\0\v<\"(){}*>,") "¬" "\u00A0")

                # Identifiers
                :symchars (if-not :reserved 1)
                :var-underscore (* "_" (any :symchars))
                :var-uppercase  (* (range "AZ") (not :symchars))

                # A variable must start with underscore or be a single uppercase letter
                :var-token (choice :var-underscore :var-uppercase)

                # Atoms
                # A quoted atom knows exactly two escapes, \" and \\, so that
                # every name is writable: without them a Wikidata label
                # carrying a quote could not be entered at all, and the line
                # zelph printed for it read back as several atoms. A
                # backslash in front of anything else stays an ordinary
                # character (Windows paths, LaTeX).
                :quoted (capture (* "\"" (any (choice (* "\\" 1) (if-not "\"" 1))) "\""))

                # Normal atoms (sequences of non-reserved chars)
                :raw-atom (capture (some :symchars))

                # Multi-char Arrows containing reserved chars (must be checked before raw-atom/ops)
                :arrow-multi (capture (choice "=>" "->" "-->" "<=>" "<=" ">="))

                # Single-char Operators (from reserved set)
                :op-single (capture (choice ">" "<"))

                # Structure Tags
                :tag-var    (group (* (constant :var)  (capture :var-token)))

                # Unquote: ,identifier references a Janet variable
                :tag-unquote (group (* (constant :unquote) "," (capture (some :symchars))))

                # Neural condition sugar: ≈net(pattern). Syntax only -- desugars to
                # (zelph/approx pattern "net"), which tags the pattern in the graph.
                :tag-approx (group (* (constant :approx) "≈" (capture (some :symchars)) :s* :val-any))

                # Number literal: &<token>. Syntax only -- the interpretation is
                # delegated to the redefinable Janet function zelph/number, so the
                # internal number representation is defined by scripts, not by C++.
                # (& as prefix is a nod to BBC BASIC / Amstrad CPC number literals.)
                :tag-number (group (* (constant :number) "&" (capture (some :symchars))))

                # Self-fact sugar: :pred X. Syntax only -- desugars to the
                # self-fact (X pred X), the stdlib marker idiom (:simplify T
                # for (T simplify T)). ':' stays an ordinary symchar
                # elsewhere, so atoms with inner colons (URLs, wd:Q5) are
                # unaffected; only a LEADING colon on a value position
                # triggers the sugar. The predicate is a single plain token;
                # a variable token (e.g. :R) keeps variable semantics.
                :tag-selffact (group (* (constant :selffact) ":" (capture (some :symchars)) :s* :val-any))

                # A prefix form -- :pred X, ≈net X, *X, @{...} -- is
                # attempted before the atom that the prefix might also be
                # read as. If the form fails due to its operand failing, and
                # the content following the prefix cannot end a value, then
                # the atom is rejected: interpreted as an atom, the prefix
                # leaves the same operand to the next value, which fails the
                # same way, so the line has no reading either way. Finding
                # that out read the operand twice, and each prefix form
                # contained within it twice per reading -- 2^n for a
                # malformed line nested n levels deep.
                :value-end (choice :ws (set ",)}>") -1)
                :spent-prefix (choice (* (choice ":" "≈") (some :symchars) :s* (not :value-end)) "@{")

                # Atom Definition Order:
                # 1. Quoted (always safe)
                # 2. Multi-char arrows (e.g. "=>"). Must be before raw-atom because "=" is a symchar.
                # 3. Raw atoms (e.g. "abc", "=")
                # 4. Single ops (e.g. ">"). Checked last to prefer longer matches or delimiters.
                :tag-atom   (group (* (constant :atom) (not :spent-prefix) (choice :quoted :arrow-multi :raw-atom :op-single)))

                :star-atom  (group (* (constant :atom) (capture "*") (> 0 :value-end)))

                # 1. Compact List: <abc> — no spaces between chars, split into individual character nodes.
                #    Characters are stored reversed internally (LSB-first for numbers).
                :tag-list-compact (group (* (constant :list-compact) (* "<" (capture (some (if-not (set "> \t\r\n") 1))) ">")))

                # Recursive definitions need forward declaration in PEG if simple recursive descent isn't enough,
                # but Janet PEG handles this via the :val-any choice reference.

                # Focused Value: *Value (e.g. *A or *{...} or *(...))
                # Returns [:focused value-node]
                :tag-focused (group (* (constant :focused) "*" :val-any))

                # Negation sugar: ¬Value  (e.g. ¬(A is green))
                # Returns [:negation value-node]
                :tag-negation (group (* (constant :negation) "¬" :s* :val-any))

                # Conjunction sugar: comma-separated conditions inside parentheses
                :conj-cond (/ (group (* (constant :condition) :stmt-any)) ,zelph-arrow)
                :comma-sep (* :s* "," (not :symchars) :s*)
                :comma-list (* :conj-cond (any (* :comma-sep :conj-cond)))

                # Nested Facts: ( A B C ), or a comma list ( A B C, D E F ).
                # ONE pattern reads both, and zelph-comma-list tells them
                # apart by the number of conditions it found. Two
                # alternatives -- the comma list first, the plain statement
                # when no comma turned up -- read a group without a comma
                # twice, and every group inside it twice per reading, so a
                # term nested n deep was read 2^n times. A PEG does not
                # remember what it has read, and Janet collects no garbage
                # during a match: depth 20 took six seconds and a gigabyte
                # for one line.
                :tag-nested (/ (* "(" :s* :comma-list :s* ")") ,(zelph-comma-list :nested))

                # Sets: { A B C }
                :set-content (any (sequence :s* :val-any))
                :tag-set    (group (* (constant :set) "{" :set-content :s* "}"))

                # Collection literal: @{...}. A container with its own
                # identity whose membership can grow, as opposed to the set
                # constant {...} which IS its members. The marker follows
                # Janet, where {...} is the immutable struct and @{...} the
                # mutable table -- and it costs no reserved character: "@"
                # stays an ordinary symchar, only "@{" is special.
                :tag-collection (group (* (constant :collection) "@{" :set-content :s* "}"))

                # 2. Node List: < a b > — space-separated, stored as cons list (last element outermost).
                #    The user writes elements in the order they should be displayed; node_to_string reverses
                #    the internal order back for output. For numbers, write digits in reverse: <3 2 1>
                #    represents the number 123 (same internal form as the compact <123>).
                # The loop (if-not ">" :val-any) ensures we don't consume the closing delimiter.
                :list-content (any (sequence :s* (if-not ">" :val-any)))
                # A node list must have its closing ">" before the termination
                # of the group it belongs to, and :list-probe checks that
                # first, scanning only characters and leaving no capture.
                # Absent this check, "<" as an operator -- as in a < b or
                # x <= y -- was initially interpreted as the start of a list:
                # the attempt read the rest of the statement, including nested
                # groups, and failed for want of a ">", and :stmt-any then
                # re-read the entire expression, so (a < (a < ... x)) doubled
                # per level, just as :tag-nested did. The probe skips content
                # that might conceal a ")" or a ">": strings, compact lists
                # (<=)> is one) and balanced groups and sets. Every list that
                # successfully parses passes this check, so it only saves
                # attempts that could not succeed.
                #
                # Five forms passed an earlier probe all the same, and each
                # doubled at every level in the same fashion:
                # - a "(" or "{" that does not close inside the probe, read as
                #   a plain character, as in (a < (a < x})) -- the probe now
                #   refuses it, since a group or set that parses is balanced;
                # - a ">" positioned after a comma separating two conditions,
                #   as in (x < a, b > c) -- the probe halts at such a comma, as
                #   a node list never contains one;
                # - the ">" of a nested list, as in (a < <b c> (a < ...)) --
                #   the probe tallies every "<" it encounters and allows each
                #   ">" to close one such opening before its own. It tallies
                #   rather than diving recursively into the nested list,
                #   because a recursive probe descends one level per "<", and a
                #   long sequence a < a < ... exhausts the recursion limit of
                #   peg/match;
                # - the ">" of an arrow, as in (a < -> (a < ...)) -- a value
                #   commencing with -->, -> or => is read as that arrow in
                #   full. Where the reading stands at the beginning of a value
                #   -- immediately after a "<" or a blank that cannot belong to
                #   an atom, and after a string, a group, a set, a list,
                #   another arrow or a prefix form -- the probe skips the
                #   whitespace and subsequent arrows. There every whitespace
                #   character separates values, the form feed included, so in
                #   "q"\f-> the arrow constitutes a value by itself. After an
                #   atom the probe skips no arrow: in x-> the "-" terminates
                #   the atom x- and the ">" can close a list, and an atom
                #   absorbs a form feed following it, so a\f-> is the atom a\f-
                #   and a ">". The probe reads a run of blanks as a whole,
                #   regardless of whether an arrow comes after or not: re-read
                #   from each blank passed, a run of n blanks cost n^2;
                # - the ">" in the operand of a prefix form, as in
                #   (a < ¬-> (a < ...)) -- ¬X, *X, :p X and ≈n X read X as a
                #   value, and an X starting with a ">" includes it: ¬->
                #   denotes the negation of the arrow ->, :p > the self-fact of
                #   the atom >, and :p-> the same under the name p-. Where a
                #   value commences, the probe skips a run of prefixes, each
                #   accompanied by the blanks its form consumes after it, and
                #   then the -->, ->, =>, >= or > that the operand begins with.
                #   It skips the prefixes irrespective of whether such an
                #   operand follows, so it reads a run of them just once. ¬ and
                #   * are reserved, so either one starts a value wherever it
                #   appears, as in x¬->; a ":" or "≈" initiates a prefix only
                #   where a value starts: in x:p > it belongs to the atom x:p,
                #   and the ">" closes the list. The focus * reads its operand
                #   with no blank in between, and a form feed right behind it
                #   begins that operand, an atom: in *\f-> the atom is \f- and
                #   the ">" closes the list, so the probe does not treat that *
                #   as a prefix.
                :probe-quoted (* "\"" (any (choice (* "\\" 1) (if-not "\"" 1))) "\"")
                :probe-compact (* "<" (some (if-not (set "> \t\r\n") 1)) ">")
                :probe-item (choice :probe-quoted :probe-compact :probe-group :probe-set)
                :probe-group (* "(" (any (choice :probe-item (if-not (set "(){}") 1))) ")")
                :probe-set (* "{" (any (choice :probe-item (if-not (set "(){}") 1))) "}")
                :probe-open (cmt (* (backref :probe-depth) "<") ,inc :probe-depth)
                :probe-close (cmt (* (backref :probe-depth) ">") ,(fn [depth] (if (pos? depth) (dec depth))) :probe-depth)
                :probe-mark (choice (* "¬" :s*) (* "*" (not "\f")))
                :probe-prefix (choice :probe-mark (* (choice ":" "≈") (some :symchars) :s*))
                :probe-prefixed (* (some :probe-prefix) (? (choice "-->" "->" "=>" ">=" ">")))
                :probe-gap (any (choice (some :ws) "-->" "->" "=>" :probe-prefixed))
                :probe-arrow (* (some (set " \t\r\n\0\v")) :probe-gap)
                :list-probe (drop (* "<" (constant 0 :probe-depth) :probe-gap (any (choice (* :probe-item :probe-gap) :probe-arrow (* :probe-open :probe-gap) (* :probe-close :probe-gap) (* (> 0 :probe-mark) :probe-prefixed :probe-gap) (if-not (choice (set "(){}<>") (* "," (not :symchars))) 1))) ">"))
                # After the probe locates the ">", the content must terminate
                # at the point where the list closes or where a value can
                # end: at ">", a comma separator, ")", "}" or the line's end.
                # In any other position, the line has no reading: read as an
                # operator, "<" leaves the same content for the next value,
                # which similarly fails. Determining this required reading
                # the content twice, and each nested list within it twice per
                # reading -- resulting in 2^n for a malformed line nested n
                # levels deep. The match ceases at that point with an error,
                # and the caller reports the line as unparsable.
                :list-can-close (choice ">" (* "," (not :symchars)) (set ")}") -1)
                :tag-list-nodes (* (> 0 :list-probe) (group (* (constant :list-nodes) "<" :list-content)) :s* (choice ">" (if-not :list-can-close (error (constant "list")))))

                # Value order:
                # Check lists first so "<" starts a list if possible.
                :val-any (choice :tag-focused :tag-negation :tag-approx :tag-selffact :tag-var :tag-unquote :tag-number :tag-list-compact :tag-list-nodes :tag-collection :tag-atom :star-atom :tag-nested :tag-set)

                # A statement is a sequence of values separated by whitespace
                # Used inside ( ... ) and at top level for facts
                :stmt-any (sequence :val-any (any (sequence :s+ :val-any)))

                # Top Level Parsing
                # Everything is captured into a :root group, or a :conjunction if comma separated.
                # C++ logic decides if it's a single value or a fact (S P O) based on element count.
                # Read once, like a group in parentheses (see :tag-nested).
                :main (sequence :s* (/ :comma-list ,(comp zelph-line (zelph-comma-list :root))) :s* -1)})

            (defn zelph-safe-parse [peg text]
               (peg/match peg text))
        )zph";

        Janet out;
        int   status = janet_dostring(_janet_env, peg_setup.c_str(), "setup", &out);
        flush_err_trace();
        if (status != JANET_SIGNAL_OK) janet_stacktrace(nullptr, out);

        janet_dostring(_janet_env, "(def zelph-peg (peg/compile zelph-grammar))", "init", &out);
        flush_err_trace();
        _zelph_peg = out;
        janet_gcroot(_zelph_peg);
    }

    void ScriptEngine::Impl::setup_numbers() const
    {
        const char* code = R"janet(
            (defn zelph/number
              "Fallback for &-literals: no number representation is loaded."
              [s]
              (error (string "number literal &" s " has no representation - "
                             "load a script that defines zelph/number "
                             "(e.g. decimal-arithmetic.zph or binary-arithmetic.zph)")))
        )janet";

        Janet out;
        janet_dostring(_janet_env, code, "default-numbers", &out);
        flush_err_trace();
    }
}
