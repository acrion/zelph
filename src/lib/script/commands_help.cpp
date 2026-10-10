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

#include "script/command_executor_impl.hpp"

#include "network/reasoning.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace zelph;

// Alternative spellings of a command. One table drives BOTH the dispatch
// registration and ".help <alias>", so an alias can never exist as a
// runnable command while ".help" claims not to know it.
static const std::map<std::string, std::string> command_aliases = {
    {".why", ".explain"},
};

namespace zelph::console
{
    void CommandExecutor::Impl::cmd_help(const std::vector<std::string>& cmd)
    {
        static const std::vector<std::string> general_help_lines = {
            "zelph Interactive Help",
            "",
            "Available Commands",
            "──────────────────",
            "",
            "Session",
            "  .help [command]                           – Show this help or detailed help for a specific command",
            "  .quit                                     – Exit REPL (quits zelph)",
            "  .licenses                                 – Show third-party libraries and licenses",
            "",
            "Scripts, Loading & Saving",
            "  .import <script> [args...]                – Load a MODULE: quiet (no echo, no focus anchors, one run at the end), .zph/.janet only, falls back to the standard library",
            "  (from the shell) zelph <script> [args...] – Run a script as a SESSION: exactly as if its lines had been typed. Same resolution, any file name",
            "  .provides <id> [id2 ...]                  – Claim module IDs in the import registry",
#ifndef __EMSCRIPTEN__
            "  .load <file>                              – Load a saved network (.bin) or import Wikidata JSON dump (creates .bin cache)",
            "  .load-partial <file|manifest> [...]       – Load selected chunks as a read-only partial view (see .help .load-partial)",
            "  .save <file.bin>                          – Save the current network to a binary file",
            "  .save-predicates <file.bin> <pred> [...]  – Save only the facts of the given predicates (a slice)",
            "  .stat-file <file.bin>                     – Show serialized-file chunk statistics without loading the network",
            "  .index-file <file.bin> <json>             – Emit a JSON byte-offset index for a serialized .bin file",
#endif
            "",
            "Languages & Names",
            "  .lang [code]                              – Show or set current language (e.g. en, de, wikidata)",
            "  .name <node|id> <new_name>                – Set name in current language",
            "  .name <node|id> <lang> <new_name>         – Set name in specific language",
            "  .delname <node|id> [lang]                 – Delete name in current language (or specified language)",
            "",
            "Exploring the Network",
            "  .stat                                     – Show network statistics (nodes, RAM usage, name entries, languages, rules)",
            "  .explain [<fact>] [depth]                 – Reconstruct why a fact holds (proof tree; no arg: last output, 0 = unlimited depth); alias: .why",
            "  .list <count>                             – List first N existing nodes (internal map order, with details)",
            "  .clist <count>                            – List first N nodes named in current language (sorted by ID if feasible)",
            "  .node [<name|id|fact>]                    – Show detailed node information; defaults to last output node",
            "  .out <name|id|fact> [count]               – List details of outgoing connected nodes (default 20)",
            "  .in <name|id|fact> [count]                – List details of incoming connected nodes (default 20)",
            "  .mermaid [<name|id|fact>] [depth] [max_neighbours] – Generate a Mermaid HTML graph; defaults to last output node",
            "  .list-predicate-usage [max]               – Show predicate usage statistics (top N most frequent predicates)",
            "  .list-predicate-value-usage <name|id|fact> [max] – Show object/value usage statistics for a specific predicate (top N most frequent values)",
            "",
            "Inference & Rules",
            "  .run                                      – Run full inference",
            "  .run-once                                 – Run a single inference pass",
            "  .run-delta                                – Run inference seeded only by the facts added since the last run",
#ifndef __EMSCRIPTEN__
            "  .run-export <file>                        – Run inference and write what that run derives to a JSON Lines file (see .help .run-export)",
#endif
            "  .auto-run                                 – Toggle automatic execution of .run after each input; takes no argument (default: on)",
            "  .deductions [all|focus|quiet|off]         – Set the deduction printing mode (default: quiet)",
            "  .list-rules                               – List all defined inference rules",
            "  .strata                                   – Show the negation levels of the rules, and the rules that cannot be stratified",
            "  .remove-rules                             – Remove all inference rules",
            "",
            "Editing & Removing",
            "  .remove <name|id>                         – Remove a node and everything it is a part of (destructive)",
            "  .prune-facts <pattern>                    – Remove all facts matching the query pattern (only statements)",
            "  .prune-nodes <pattern>                    – Remove matching facts AND all involved subject/object nodes",
            "  .prune-nodes <var> (<conditions>)         – ... selected by a conjunction, deleting what <var> binds",
            "  .cleanup                                  – Remove isolated nodes and clean name mappings",
            "  .new                                      – Clear the complete network and re-initialize the core nodes",
            "",
            "Clusters",
            "  .cluster [name]                           – Show clusters, or activate one ('default' = no cluster)",
            "  .cluster-drop <name>                      – Remove a cluster INCLUDING all nodes created in it (rollback)",
            "  .cluster-merge <from> <to>                – Move a cluster's membership into another ('default' = keep nodes, forget cluster)",
#ifndef __EMSCRIPTEN__
            "",
            "Wikidata",
            "  .wikidata-constraints <json> <dir>        – Export property constraints as zelph scripts to a directory",
            "  .wikidata-qualifiers <json> [P1 P2 ...]   – Import statement qualifiers from a Wikidata dump (all, or only listed qualifier properties)",
            "  .export-wikidata <json> <id1> [id2 ...]   – Extract exact JSON lines for Q-IDs (no import)",
#endif
            "",
            "Engine Behaviour",
            "  .parallel                                 – Toggle parallel processing (default: on)",
            "  .anchors [on|off]                         – Show or set anchor-based candidate lookups in unification (default: on)",
            "  .semi-naive [on|off|check]                – Show or set the fixpoint evaluation strategy (default: on)",
            "  .fact-stores [on|off]                     – Show or disable the fact-path acceleration stores (memory vs. speed)",
            "  .contradiction-records [on|off]           – Show or disable writing each contradiction into the graph (memory vs. repeated reports)",
            "",
            "Logging & Profiling",
            "  .log <max-depth>                          – Enable detailed reasoning logging up to given recursion depth (0 = off, -1 = counters only)",
            "  .log-janet                                – Toggle logging of Janet function calls (inputs/outputs)",
            "  .prof [reset]                             – Dump reasoning profiler counters (requires .log -1 or .log N); 'reset' starts a fresh window",
            "",
            "Type \".help <command>\" for detailed information about a specific command.",
            "",
            "Basic Syntax",
            "────────────",
            "Facts:    <subject> <predicate> <object>",
            "          Predicates with spaces must be quoted.",
            "          Example: peter \"is father of\" paul",
            "          Inside a quoted name, \\\" is a quote and \\\\ a backslash;",
            "          a backslash before anything else stands for itself.",
            "          A statement may span lines: a line that is not yet a complete",
            "          statement -- 'a p', or a bare term such as '(a p b)' -- waits,",
            "          and the next statement line is appended to it.",
            "",
            "Comments: '#' at the start of a line, or after a blank, starts a",
            "          comment that runs to the end of the line -- behind a",
            "          statement or a command as well. Inside a name (C#) or a",
            "          quoted name it is text; a name that begins with '#' is quoted.",
            "          A continuation line is still classified by its first",
            "          character, even inside an open quoted name: '#' there",
            "          makes it a comment line, '.' a command.",
            "",
            "Queries:  Statements containing variables (A-Z or starting with _).",
            "          Example:",
            "          _who \"is father of\" paul",
            "          Answer: peter \"is father of\" paul",
            "",
            "Results:  ? <statement> asserts the statement, runs inference quietly,",
            "          then queries its result (the stdlib-wide \"= idiom\"):",
            "          ? (&17 mod &5)              Answer: (&17 mod &5) = &2",
            "          ? :testprime &53            Answer: (:testprime &53) = prime",
            "          ? $( x*x ) diffby x         Answer: ... = (x + x)",
            "          The space is optional before ( and $: ?(&17 mod &5) is the same.",
            "          No answer means no result was derivable (partiality by absence).",
            "",
            "Rules:    (condition1, condition2, ...) => (deduction)",
            "          Example:",
            "          Berlin \"is capital of\" Germany",
            "          Germany \"is located in\" Europe",
            "            (X \"is capital of\" Y,",
            "             Y \"is located in\" Z)",
            "          => (X \"is located in\" Z)",
            "          (Berlin \"is located in\" Europe)",
            "            ⇐ {(Germany \"is located in\" Europe)",
            "               (Berlin \"is capital of\" Germany)}",
            "",
            "Janet Scripting",
            "───────────────",
            "Janet:    %<code> at the START of a line (the whole line is Janet) or bare % (toggle",
            "          block mode until next %). '%' is a LINE escape, never a term: 'S %(f x) O' is",
            "          not a statement. Bind the value in Janet and unquote its name instead.",
            "          Janet generates facts/rules programmatically – then zelph inference runs as usual.",
            "          Example (using the Berlin/Germany facts from above):",
            "          %(zelph/fact \"Berlin\" \"is capital of\" \"Germany\")",
            "          Germany \"is located in\" Europe",
            "          %",
            "          (zelph/build-rule",
            "            (fn []",
            "              (let [cond (zelph/collection",
            "                          (zelph/fact 'X \"is capital of\" 'Y)",
            "                          (zelph/fact 'Y \"is located in\" 'Z))]",
            "                (zelph/fact cond \"~\" \"conjunction\")",
            "                (zelph/fact cond \"=>\" (zelph/fact 'X \"is located in\" 'Z)))))",
            "          %",
            "          (Berlin \"is located in\" Europe)",
            "            ⇐ {(Germany \"is located in\" Europe)",
            "               (Berlin \"is capital of\" Germany)}",
            "",
            "Unquote:  ,janet-var inside zelph lines (after defining in Janet). This is how a",
            "          value COMPUTED in Janet becomes a term of a statement.",
            "          Written without whitespace after the comma: ',pred' unquotes, ', pred'",
            "          is the conjunction separator. It takes a NAME, not an expression.",
            "          Example:",
            "          %(def berlin (zelph/resolve \"Berlin\"))",
            "          ,berlin \"is capital of\" Germany"};

        static const std::map<std::string, std::string> detailed_help = {
            {".help", ".help [command]\n"
                      "Without argument: shows this general help text with syntax and command overview.\n"
                      "With argument: shows detailed help for the specified command."},

            {".quit", ".quit\n"
                      "Exits the interactive REPL session and quits zelph."},

            {".lang", ".lang [language_code]\n"
                      "Without argument: displays the current language used for node names.\n"
                      "With argument: sets the language (e.g., 'zelph', 'en', 'de', 'wikidata')."},

            {".name", ".name <node|id> <new_name>\n"
                      "Sets the name of the node in the current language.\n"
                      ".name <node|id> <lang> <new_name>\n"
                      "Sets the name in the specified language.\n"
                      "The <node|id> can be a name (in current language) or numeric node ID.\n"
                      "Empty <new_name> is not allowed – use .delname to remove a name.\n"
                      "\n"
                      "Giving a node a name another node already holds in that language MERGES\n"
                      "the two, with a warning naming both. That is how one says afterwards that\n"
                      "a node written by hand and an imported entity are the same thing. A node\n"
                      "IS the hash of what it is built from, so every fact built on the one that\n"
                      "disappears is re-created under the id its new components give it, folding\n"
                      "into an equal fact where the graph already holds one. Core nodes are never\n"
                      "the ones that disappear, and a variable and a non-variable cannot merge at\n"
                      "all, nor can a collection of a rule's text and any other node.\n"
                      "\n"
                      "A name a rule's VARIABLE displays is not in the way: the node takes the\n"
                      "name over, the variable goes on rendering under it, and nothing merges.\n"
                      "Many nodes may carry one variable name -- every statement quantifies its\n"
                      "own -- so the name is display-only there."},

            {".delname", ".delname <node|id> [lang]\n"
                         "Removes the name of the node in the current language (or the specified language if provided).\n"
                         "The <node|id> can be a name (in current language) or numeric node ID.\n"
                         "If the node had no name in the target language, nothing happens."},

            {".list", ".list <count>\n"
                      "Lists the first N existing nodes in the network (in internal map iteration order).\n"
                      "For each node: ID, non-empty names in all languages, connection counts, representation, and Wikidata URL if available."},

            {".clist", ".clist <count>\n"
                       "Lists the first N nodes that have a name in the current language.\n"
                       "If the language has a reasonable number of entries (≤ ~50k), nodes are sorted by ID.\n"
                       "For very large languages (e.g. 'wikidata'), order follows the internal map (fast, no full sort)."},

            {".out", ".out <name|id|fact> [count]\n"
                     "Lists detailed information for up to <count> nodes reachable via outgoing connections\n"
                     "from the given node (default 20, sorted by node ID).\n"
                     "The node can be named, given by numeric ID, or written as the FACT it is --\n"
                     "'.out a rel b', with or without parentheses, exactly as the fact prints."},

            {".in", ".in <name|id|fact> [count]\n"
                    "Lists detailed information for up to <count> nodes that have outgoing connections\n"
                    "to the given node (default 20, sorted by node ID).\n"
                    "The node can be named, given by numeric ID, or written as the FACT it is --\n"
                    "'.in a rel b', with or without parentheses, exactly as the fact prints."},

            {".node", ".node [<name|id|fact>]\n"
                      "Displays details for a single node: its ID, non-empty names in all languages,\n"
                      "incoming/outgoing connection counts, and a clickable Wikidata URL if it has a Wikidata ID.\n"
                      "The argument can be a name (in current language), a numeric node ID, or the\n"
                      "FACT itself -- '.node a rel b', with or without parentheses, exactly as the\n"
                      "fact prints. If no argument is given, the node from the last output is used.\n"
                      "That is the node of the statement, query answer or deduction printed last, so\n"
                      "the argument-less form works in the session -- typed, piped, or a script run\n"
                      "as 'zelph script.zph'. Inside a module (.import) nothing is printed, so there\n"
                      "is no last node to fall back to.\n"
                      "Facts ABOUT the node that are engine bookkeeping are reported as properties\n"
                      "instead of being written into the term: 'Negated by a rule' and 'Rule pattern\n"
                      "(not asserted)', the latter for a statement that exists only because a rule\n"
                      "was written with it."},

            {".mermaid", ".mermaid [<name|id|fact>] [depth] [max_neighbours]\n"
                         "Generates a Mermaid HTML file visualizing a node and its connections, and outputs\n"
                         "a clickable file:// link to it. The file is written to the system temp directory\n"
                         "and named after the node.\n"
                         "The node is named the same way as for .node: a name in the current language, a\n"
                         "numeric node ID, or the FACT itself -- '.mermaid a rel b', with or without\n"
                         "parentheses, exactly as the fact prints. With no argument at all, the node from\n"
                         "the last output is used -- see '.help .node' for what that is and where it\n"
                         "exists.\n"
                         "depth defaults to 1, which does not mean one hop: it selects a depth that grows\n"
                         "until the graph holds enough nodes to be worth looking at. max_neighbours caps\n"
                         "how many neighbours each node contributes.\n"
                         "Both numbers are read off the END of the arguments, and only when what stands\n"
                         "before them already denotes a node. '.mermaid a rel b 2' is therefore the fact\n"
                         "'a rel b' at depth 2, while '.mermaid a rel 2' is the fact 'a rel 2', because\n"
                         "'a rel' denotes nothing. .in and .out separate their count the same way."},

            {".run", ".run\n"
                     "Performs full inference: repeatedly applies all rules until no new facts are derived.\n"
                     "Deductions are printed as they are found.\n"
                     "A rule that creates nodes -- through a fresh variable, a nested term or a\n"
                     "collection literal in its consequence -- can keep the run from ending; see\n"
                     "'Fresh Variables' in the logic documentation.\n"
                     "The summary at the end counts matches: the bindings the unification search\n"
                     "delivered for the rules' positive fact conditions, in every pass that scans\n"
                     "the facts (under .semi-naive check, its safety pass as well) and in every\n"
                     "seeded iteration. It measures work, not results, and is comparable only\n"
                     "between runs in one evaluation mode. See 'What the run summary counts' in\n"
                     "the rules documentation."},

            {".run-once", ".run-once\n"
                          "Performs a single inference pass."},

            {".run-delta", ".run-delta\n"
                           "Like .run, but starts from the facts created since the previous run\n"
                           "instead of from a pass over the whole graph.\n"
                           "\n"
                           ".run always begins with one classic pass, because it cannot know what\n"
                           "the graph looked like before. That pass costs time proportional to the\n"
                           "graph, so repeatedly adding a little and running again costs time\n"
                           "proportional to everything accumulated so far, not to what was added.\n"
                           ".run-delta removes that term: it seeds the fixpoint with the new facts\n"
                           "and lets semi-naive evaluation take it from there.\n"
                           "That holds for the rules that can be seeded. A rule with a negated\n"
                           "condition still takes a classic pass over the facts its positive\n"
                           "conditions match, at each negation level of the run, and a rule that\n"
                           "cannot be seeded (a path, neural or nested condition, a condition\n"
                           "without exactly one predicate, or no positive fact condition at all)\n"
                           "takes one in every iteration; their cost follows those facts, not the\n"
                           "addition. With .import math, the simplifier's identity fallback is\n"
                           "such a rule.\n"
                           "\n"
                           "This is only equivalent to .run when the graph already is a fixpoint of\n"
                           "the current rules -- otherwise the missing pass is exactly the one that\n"
                           "would have found the older consequences. The command therefore falls\n"
                           "back to a full pass, with a note, unless a previous run has happened,\n"
                           "the rule set is unchanged since, and .semi-naive is on.\n"
                           "\n"
                           "Typical use is a program driving zelph as a library: run once to\n"
                           "saturate, then assert and .run-delta per unit of new data."},

            {".explain", ".explain [<fact pattern>] [max-depth]   (alias: .why)\n"
                         "Reconstructs a justification for an asserted fact from the graph\n"
                         "and prints it as a proof tree in the '⇐' notation. Without a\n"
                         "pattern, the last output node is explained -- the natural\n"
                         "companion of the '?' prefix:\n"
                         "    .import binary-arithmetic\n"
                         "    ? (&6 + &7)\n"
                         "    .explain\n"
                         "The same three lines work as a script file, because a script\n"
                         "named on the command line is a session (see '.help .node'). A\n"
                         "tree counts as output of its root alone, so a second\n"
                         "argument-less .explain explains the same fact again.\n"
                         "A trailing number is max-depth when the rest of the argument\n"
                         "names a fact the graph holds; otherwise, when the whole argument\n"
                         "names one, it is the fact's last object. Parenthesise the pattern\n"
                         "to say which is meant: .explain (y val 7 3) or .explain (y val 7)\n"
                         "3.\n"
                         "Nothing is recorded during inference: .explain rebuilds a\n"
                         "justification by backward search, from the rules in force and the\n"
                         "facts the graph holds NOW. A derived fact has one as long as its\n"
                         "premises stay and its negated premises stay absent. It loses it\n"
                         "when a premise is removed (.prune-facts, .prune-nodes, .remove),\n"
                         "when its rule is removed (.remove-rules), or when a later fact\n"
                         "makes a negated premise hold (check mode reports that case); the\n"
                         "fact itself stays. A rule that a statement only mentions -- the\n"
                         "template inside a rule generator -- is not in force, here as in\n"
                         "the forward direction. The labels of a tree:\n"
                         "  [axiom]  no consequence of a rule in force matches the fact:\n"
                         "      an input fact, or a derived one whose rule is gone.\n"
                         "  [axiom; negated by a rule]  the same, for a fact that some\n"
                         "      rule reads under a negation.\n"
                         "  [asserted; no derivation found]  the fact holds, a rule's\n"
                         "      consequence matches it, and the search rebuilt no\n"
                         "      instantiation that holds: an input fact that a general\n"
                         "      rule could also derive, a derived fact that lost its\n"
                         "      justification, or a fact on a cycle (below).\n"
                         "  [rule pattern; not asserted]  the queried node exists only\n"
                         "      because a rule was written with it as a ground pattern;\n"
                         "      such a node is never a premise.\n"
                         "  [rule mentioned; not in force]  a queried rule that a\n"
                         "      statement mentions (above). It may have been typed as\n"
                         "      well, since a statement can name a typed rule itself:\n"
                         "      (G => H) => ((G => H) is seen) derives one for each typed\n"
                         "      ground rule, which then no longer fires. As a premise, a\n"
                         "      mentioned rule is labelled like any fact.\n"
                         "  ¬(...) [absent]  a negation-as-failure premise, verified\n"
                         "      against the current graph -- a negated path condition\n"
                         "      by a walk that finds no path.\n"
                         "  [closure]  a transitive path premise: it holds by a walk. A fact\n"
                         "      the walk steps over is printed below it where its proof\n"
                         "      ends at a member of a cycle (below).\n"
                         "  [see above]  a derived fact expanded at an earlier line. Where\n"
                         "      the depth limit cut that expansion, the fact is expanded\n"
                         "      once more where it stands highest in the tree. Repeated\n"
                         "      axioms stay written out: [axiom] is their whole expansion.\n"
                         "  … [depth limit]  a derived fact at the depth limit: not\n"
                         "      expanded.\n"
                         "  [one of several justifications]  on the root only (below).\n"
                         "Facts whose derivations run through each other are resolved\n"
                         "together. When the fact through which the search entered such a\n"
                         "cycle has no derivation that does not run back through the cycle,\n"
                         "it shows the first instantiation found for it on other members,\n"
                         "and those of them without a derivation of their own end the proof\n"
                         "as '[asserted; no derivation found]': in this vocabulary\n"
                         "'asserted' means 'holds in the graph', and every derivation found\n"
                         "for them runs back through the cycle. Without provenance nothing\n"
                         "tells which member was asserted, so such a leaf may be a derived\n"
                         "fact, and an asserted fact asked about shows one step to other\n"
                         "members. No derivation that travels around a cycle is printed,\n"
                         "and no tree shows a fact below itself, at any depth.\n"
                         "A path condition is printed as one '[closure]' line. The facts its\n"
                         "walk steps over count as premises of the step: one that a rule\n"
                         "concludes is searched like any premise, and one being searched\n"
                         "makes the step wait for it, unless a proof of it whose leaves are\n"
                         "all axioms is found already. Such a fact is printed below the\n"
                         "[closure] line where its proof ends at a member of a cycle printed\n"
                         "as '[asserted; no derivation found]' (above), and is not printed\n"
                         "otherwise. It stands two levels below the step, and the depth\n"
                         "limit cuts it like any premise. The walk taken is a shortest one\n"
                         "around the facts being searched. Where every walk needs one of\n"
                         "them, further instantiations take, for each of them, the walk that\n"
                         "leaves it out. A walk over a fact that turns out to be searched\n"
                         "only when the step reaches it, and that makes the step wait, is\n"
                         "followed by the walk around that fact, as long as there is one, at\n"
                         "most 32 times for one path condition; several path conditions with\n"
                         "several walks each make at most 64 instantiations.\n"
                         "The search stops at the first justification it can rebuild,\n"
                         "preferring one whose leaves are all axioms. The root line says\n"
                         "'[one of several justifications]' when a further instantiation\n"
                         "holds whose premises are grounded without the root. It is never\n"
                         "said of one that rests on the root. A premise counts when the\n"
                         "search grounded it without the root, so one that holds only as\n"
                         "asserted and that the root also derives does not count: nothing\n"
                         "tells the two apart. Nor is it said where finding the further\n"
                         "instantiation takes more work than the proof shown took, and at\n"
                         "least a fixed amount; the root then looks no further.\n"
                         "A rule carrying a neural condition is NOT used by the backward\n"
                         "search: a network confidence is not a structural premise, so\n"
                         "a fact derived only through one is reported as 'no derivation\n"
                         "found'. The forward direction is unaffected -- the deduction\n"
                         "line names the tag fact (pattern nn net) as its premise.\n"
                         "max-depth defaults to 4; 0 means unlimited. Under a limit the tree\n"
                         "is the top of the complete proof: a derived fact max-depth levels\n"
                         "below the root is printed with '… [depth limit]' and not expanded,\n"
                         "nothing below it is printed, and every fact above it has the\n"
                         "derivation and the label it has without the limit. The complete\n"
                         "proof is searched once per fact. When finding a proof of the\n"
                         "queried fact whose leaves are all axioms takes more than a fixed\n"
                         "amount of work -- about 0.2 s, what a proof of 5 000 facts on the\n"
                         "arithmetic modules takes -- the search runs again within the limit\n"
                         "instead, and a note above the tree says so. That search does not\n"
                         "go beyond the limit, and a fact met again higher up than any\n"
                         "search of it so far is searched again from there, so it is\n"
                         "expanded as far as the limit allows -- at most max-depth searches\n"
                         "per fact. Three cases keep a result found deeper down: where every\n"
                         "derivation the search finds from the higher position runs through\n"
                         "facts above it; where a result found earlier would show the fact\n"
                         "being searched below itself and the fact has used up its searches;\n"
                         "and where an instantiation found while another fact of the same\n"
                         "cycle was searched holds the earlier result. The fact may then be\n"
                         "cut above the limit, or, when no other derivation is found,\n"
                         "printed as '[asserted; no derivation found]' although it has a\n"
                         "derivation within the limit, in one place of the tree and expanded\n"
                         "in another. A premise at the limit counts as justified there, also\n"
                         "when the annotation is decided.\n"
                         "A proof more than 5000 levels deep is not printed whole: the\n"
                         "command says so, and a max-depth of 5000 or less prints its top\n"
                         "levels.\n"
                         "Term islands work inside the pattern:\n"
                         ".explain $( x*x ) diffby x = D is invalid, but\n"
                         ".explain ($( x*x ) diffby x) = (x + x) resolves as usual.\n"
                         "A collection literal @{...} is the one printed form that cannot\n"
                         "be pasted back: each literal builds a NEW container, so it can\n"
                         "never name an existing one. The command says so and points at\n"
                         "the argument-less form, which takes the last answer's node."},
#ifndef __EMSCRIPTEN__
            {".run-export", ".run-export <file>\n"
                            "Performs full inference and writes what THAT run derives, plus every\n"
                            "contradiction it meets, to <file> as JSON Lines -- one object per line:\n"
                            "    {\"kind\":\"deduction\",\"conclusion\":[SEG,...],\"premises\":[[SEG,...],...]}\n"
                            "A SEG is either a JSON string (literal text of the rendering), the object\n"
                            "{\"core\":\"<name>\"} for a node of zelph's own vocabulary (!, ~, =>, in, ...),\n"
                            "which a converter must not mistake for a domain identifier, or an object\n"
                            "{\"names\":{\"<lang>\":\"<name>\",...}} naming one node in every language it is\n"
                            "known by. A contradiction record carries an extra \"refused\" field when the\n"
                            "engine REFUSED to build the deduced fact rather than finding the knowledge\n"
                            "base contradictory -- the same reason the console prints under the ! line.\n"
                            "Nothing in the file is specific to a target format: which name to\n"
                            "show, what to link, how to group -- all of that is the converter's decision.\n"
                            "dev_scripts/zelph-derivations.py --format md turns the file into the MkDocs\n"
                            "reports zelph used to write directly; --format text gives one flat line\n"
                            "per derivation, the starting point for tokenizer-friendly training data.\n"
                            "Deduction printing is off during the run: rendering to the console dominates\n"
                            "the cost, and the file is the point.\n"
                            "A derivation the graph ALREADY holds is not re-derived, so it is not\n"
                            "written: over a saturated network the deduction side of the file is empty\n"
                            "and the command still exits as if it had worked. Export from the run that\n"
                            "does the deriving, or start from .new. The same property means only the\n"
                            "FIRST derivation of a fact is written -- one justification per fact, not\n"
                            "all of them. Under .parallel, which justification comes first varies between\n"
                            "runs, and with it the premises in the file; the conclusions do not, unless\n"
                            "the program has rules that are not stratifiable or a rule with a fresh\n"
                            "variable. With .parallel off, two runs write the same file.\n"
                            "Contradictions are the exception and repeat, so that a second run does not\n"
                            "hand back an empty file: count them by deduplicating on the premise set,\n"
                            "not by counting lines."},
#endif
            {".list-rules", ".list-rules\n"
                            "Lists all currently defined inference rules in readable format.\n"
                            "A rule is a \"=>\" fact whose CONDITION is a statement -- a fact pattern\n"
                            "or a container of them -- and whose CONSEQUENCE can be asserted: a\n"
                            "statement, or \"!\". \"=>\" is an ordinary relation type as well, so\n"
                            "\"atom_A => atom_B\" is data and the query pattern \"S => O\" is a question;\n"
                            "neither can fire, and neither is listed here or removed by .remove-rules.\n"
                            "Nor is a rule that cannot assert what it concludes -- a container or a\n"
                            "bare name as its consequence."},

            {".strata", ".strata\n"
                        "Shows the negation levels of the current rules: the order in which the\n"
                        "rules with a negated condition are evaluated. A level runs once every\n"
                        "level below it is final, so a negation is tested only after the rules\n"
                        "below have derived all they can. A rule is placed above another when it\n"
                        "negates a fact the other can derive -- at any depth of its conclusion,\n"
                        "and through positive rules in between -- and no lower when it reads such\n"
                        "a fact positively. Rules without a negated condition have no level: they\n"
                        "run whenever there is work, and are not listed under a level.\n"
                        "Rules that negate what they derive themselves have no stratified\n"
                        "reading. They are listed once more under \"Not stratifiable\", positive\n"
                        "rules of the cycle included, and alternate within their level, which is\n"
                        "sound only when each negation is settled within one positive saturation;\n"
                        ".semi-naive check reports a fact whose negated premise has come to hold.\n"
                        "The analysis compares predicates, not patterns, so a cycle that only the\n"
                        "arguments break -- a rule negating (X out 0) that derives (X out 1) --\n"
                        "is reported as well.\n"
                        "A variable in predicate position counts as every predicate, except where\n"
                        "a rule only extends predicates it has read, as (R is transitive, A R B,\n"
                        "B R C) => (A R C) does: whatever derives (R is transitive) is then placed\n"
                        "before every negation. An inverse or sub-property rule takes its\n"
                        "predicate from an argument and counts as every predicate; written as a\n"
                        "rule that derives rules, it keeps the levels. A rule that derives a rule\n"
                        "counts as deriving what the conclusion of the rule it writes derives, and\n"
                        "every predicate where that conclusion has a variable predicate; the\n"
                        "written rule is placed on its own once it exists.\n"
                        "The numbers are those a .run announces as \"negation level N of M\" once\n"
                        ".log -1 is on, and the rules are printed as .list-rules prints them."},

            {".list-predicate-usage", ".list-predicate-usage [max_entries]\n"
                                      "Shows how often each predicate (relation type) is used, sorted by frequency.\n"
                                      "Counted are ASSERTED and derived facts only: the conditions and consequences\n"
                                      "of a rule and the pattern of a query are statements nobody claimed, so they\n"
                                      "are left out and the count agrees with what a query answers.\n"
                                      "If <max_entries> is specified, only the top N most frequent predicates are shown.\n"
                                      "If Wikidata language is active, Wikidata IDs are shown alongside names."},

            {".list-predicate-value-usage", ".list-predicate-value-usage <name|id|fact> [max_entries]\n"
                                            "Shows how often each object (value) is used with the specified predicate, sorted by frequency.\n"
                                            "The predicate can be a name (in the current language), a numeric node ID, or a printed\n"
                                            "FACT such as (a p b) -- a fact in predicate position is addressed the way it prints.\n"
                                            "Rule patterns and query patterns are not values; see .help .list-predicate-usage.\n"
                                            "If <max_entries> is specified, only the top N most frequent values are shown.\n"
                                            "If the Wikidata language is available and active, Wikidata IDs are shown alongside names."},

            {".remove-rules", ".remove-rules\n"
                              "Deletes all inference rules from the network -- exactly what .list-rules\n"
                              "shows, so a \"=>\" fact that is data rather than a rule stays. A rule\n"
                              "that only a removed rule mentioned (the rule inside a rule generator)\n"
                              "goes with it. See .help .list-rules for where the line runs."},

            {".remove", ".remove <name_or_id>\n"
                        "Removes the specified node from the network, cleaning all name mappings.\n"
                        "The argument can be a node name (looked up in the current language)\n"
                        "or a numeric node ID.\n"
                        "Everything the node is a PART of goes with it, and so on upwards:\n"
                        "  - every fact naming it as subject, predicate or object,\n"
                        "  - every fact naming one of THOSE, so nested facts follow,\n"
                        "  - every rule one of them is a condition or a conclusion of.\n"
                        "A fact that merely lost one part could not be told from a different\n"
                        "fact ('a rel b' minus 'b' reads exactly like 'a rel a'), and a rule\n"
                        "that merely lost a condition would keep firing on the remaining ones.\n"
                        "Facts the removed node is NOT part of are untouched, including a\n"
                        "condition that another rule happens to share.\n"
                        "Core nodes of the engine ('~', '=>', '!', 'cons', ...) cannot be removed.\n"
                        "WARNING: This operation is destructive and irreversible!"},

            {".import", ".import <script> [args...]\n"
                        "Loads a MODULE: a script whose purpose is to define things for the session\n"
                        "around it. Its own lines are therefore not echoed, its statements are not\n"
                        "focus anchors for the deduction filter, and inference runs once when the\n"
                        "whole file has been read rather than after every line.\n"
                        "\n"
                        "That is NOT how 'zelph script.zph' runs a script. A file named on the\n"
                        "command line is a SESSION: it behaves exactly as if its lines had been\n"
                        "typed, which is what the shebang '#!/usr/bin/env zelph' promises. It shares\n"
                        "everything below -- resolution, the .janet runner, arguments, the module\n"
                        "registry -- except that it may be called anything, whereas .import accepts\n"
                        "only '.zph' and '.janet'.\n"
                        "\n"
                        "Two script types are supported:\n"
                        "  .zph   – zelph scripts, processed line by line. The extension is optional.\n"
                        "  .janet – Janet programs, run like the janet CLI would: fresh environment\n"
                        "           with the zelph/... API available, relative imports such as\n"
                        "           (use ./foo) resolve against the script's directory, ev/... (threads,\n"
                        "           channels) works, and a main function - if defined - is called.\n"
                        "           The '.janet' extension must be spelled out.\n"
                        "\n"
                        "Anything after the script path is passed to the script as arguments:\n"
                        "  Janet scripts receive them as parameters of main (preceded by the script\n"
                        "  path, CLI convention) and via (dyn :args).\n"
                        "  zelph scripts can read them from Janet code via (dyn :args).\n"
                        "\n"
                        "Resolution order:\n"
                        "  1. The path as given (absolute, or relative to the current working directory).\n"
                        "  2. The zelph standard library. Search locations, in order:\n"
                        "       - $ZELPH_STDLIB (if set)\n"
                        "       - 'stdlib' next to the zelph executable (release archives, build tree)\n"
                        "       - '../share/zelph' relative to the executable (e.g. /usr/share/zelph)\n"
                        "       - /usr/local/share/zelph and /usr/share/zelph (non-Windows)\n"
                        "\n"
                        "Standard library scripts are addressed without path or extension:\n"
                        "  .import sparql\n"
                        "  .import decimal-arithmetic\n"
                        "Subdirectories must be given explicitly:\n"
                        "  .import examples/english\n"
                        "  .import examples/neural/nn-wikidata-demo\n"
                        "\n"
                        "On a partial view (.load-partial) importing is allowed - a query layer such\n"
                        "as sparql only defines functions. A script that ADDS facts to the incomplete\n"
                        "view is reported afterwards, since inference over them is blocked and the\n"
                        "adjacency-index cache is disabled once the graph differs from its file.\n"
                        "\n"
                        "A script that fails halfway leaves what its earlier lines did, releases its\n"
                        "module IDs again and can be imported once more after the fault is removed."},

            {".provides", ".provides <id> [id2 ...]\n"
                          "Claims one or more module IDs (lowercased) in the import registry.\n"
                          "Placed at the top of a script, it declares IDs the script provides\n"
                          "IN ADDITION to its default ID (its lowercase file name without\n"
                          "extension). Scripts claiming the same ID are interchangeable,\n"
                          "mutually exclusive implementations of one capability: whichever is\n"
                          "imported first wins; importing another provider of that ID is\n"
                          "skipped. Example: the arithmetic substrates (decimal-arithmetic,\n"
                          "binary-arithmetic, binary-nand-arithmetic) all declare\n"
                          "'.provides arithmetic' -- loading one satisfies every dependent\n"
                          "script's '.import binary-arithmetic' line, and loading a second\n"
                          "substrate is prevented.\n"
                          "Used interactively, .provides claims an ID without loading\n"
                          "anything, blocking all scripts that provide it.\n"
                          "The registry is cleared by .new."},
#ifndef __EMSCRIPTEN__
            {".load", ".load <file>\n"
                      "Loads a previously saved network state.\n"
                      "- If <file> ends with '.bin': loads the serialized network directly (fast).\n"
                      "- If <file> ends with '.json' or '.json.bz2' (Wikidata dump): imports the data and automatically creates a '.bin' cache file\n"
                      "  in the same directory for faster future loads."},

            {".load-partial", ".load-partial <file.bin|manifest.json> [selectors...] [options...]\n"
                              "\n"
                              "Load selected chunks from a serialized .bin file or from a JSON manifest\n"
                              "that references chunk locations (local paths or remote URIs).\n"
                              "The result is a read-only, incomplete graph view.\n"
                              "Inference (.run), pruning, cleanup, and destructive edits are blocked.\n"
                              "\n"
                              "Chunk selectors (comma-separated indices, default: load all):\n"
                              "  left=0,1,2          – load only left-adjacency chunks 0, 1, 2\n"
                              "  right=5,6           – load only right-adjacency chunks 5, 6\n"
                              "  nameOfNode=0,1      – load only name-of-node chunks 0, 1  (alias: name=)\n"
                              "  nodeOfName=0,1      – load only node-of-name chunks 0, 1  (alias: node-name=)\n"
                              "  <section>=none      – skip that section entirely (also accepts '-')\n"
                              "  (omit a selector to load all chunks of that section)\n"
                              "\n"
                              "Use .index-file to discover chunk indices and byte offsets,\n"
                              "and .stat-file for a quick chunk count overview.\n"
                              "\n"
                              "Route selectors (manifest mode only, require a node route index):\n"
                              "  route-node=<id,...>  – resolve node IDs to the chunks that contain them\n"
                              "                        (sets left, right, and nameOfNode selectors)\n"
                              "  route-name=<name>    – resolve a name to the nodeOfName chunk that contains it\n"
                              "  route-lang=<lang>    – language for route-name lookup (required with route-name)\n"
                              "  Route selectors determine which chunks to load automatically based on\n"
                              "  the manifest's node route index sidecar. They can be combined with\n"
                              "  explicit chunk selectors.\n"
                              "\n"
                              "Options:\n"
                              "  meta-only           – load only the header; skip all chunk payloads\n"
                              "\n"
                              "Manifest mode (for sharded/remote storage):\n"
                              "  Pass a .json manifest as the first argument, or use manifest=<path>.\n"
                              "  source-bin=<path>   – override the .bin path used for the header\n"
                              "  shard-root=<path>   – where the shards are, if not next to the manifest\n"
                              "  A shard is looked up next to the manifest first, then below shard-root,\n"
                              "  and only fetched (hf:// or https://) when no local copy exists; the same\n"
                              "  applies to the .bin the manifest names. A downloaded artifact therefore\n"
                              "  loads offline without further options.\n"
                              "\n"
                              "Examples:\n"
                              "  .load-partial data.bin                          – load everything (partial mode)\n"
                              "  .load-partial data.bin left=0,1 right=none      – two left chunks, no right\n"
                              "  .load-partial data.bin meta-only                 – header only, no graph data\n"
                              "  .load-partial manifest.json shard-root=/data/shards\n"
                              "  .load-partial manifest.json route-node=1        – load only chunks containing node 1\n"
                              "  .load-partial manifest.json route-name=A route-lang=wikidata"},

            {".save", ".save <file.bin>\n"
                      "Saves the current network state to a binary file.\n"
                      "The filename must end with '.bin'."},

            {".save-predicates", ".save-predicates <file.bin> <predicate> [<predicate> ...]\n"
                                 "\n"
                                 "Saves a SLICE of the current network: the facts of the named predicates,\n"
                                 "the nodes those facts connect, and all names of those nodes. Everything\n"
                                 "else is left behind. Predicates are named in the current language\n"
                                 "(.lang) or given as node IDs.\n"
                                 "\n"
                                 "The result is an ordinary network file. Loading it needs a fraction of\n"
                                 "the memory of the source and answers the same questions as long as they\n"
                                 "only involve those predicates -- which is what makes a large graph\n"
                                 "usable on a small machine.\n"
                                 "\n"
                                 "Relation-type declarations and the structure of nested facts travel\n"
                                 "with the slice automatically; a fact of another predicate between two\n"
                                 "retained nodes does not. The declaration travels for EVERY retained\n"
                                 "node that has one, not only for the named predicates -- a rule's\n"
                                 "patterns name predicates of their own, and without their declaration\n"
                                 "the rule arrives structurally complete and unreadable.\n"
                                 "\n"
                                 "EVERY rule travels, complete with its conjunction and negation tags,\n"
                                 "so the slice reasons over the predicates it kept exactly as the\n"
                                 "source does -- contradiction rules included, which used to stay\n"
                                 "behind and take their reports with them. A rule whose conditions\n"
                                 "name a predicate that was left out is carried intact and simply\n"
                                 "never matches. The command says how many rules went into the file.\n"
                                 "\n"
                                 "The weight store (neural synapses, explicitly set probabilities)\n"
                                 "travels whole: its entries are keyed by a PAIR of nodes and a\n"
                                 "synapse need not be an edge, so which of them belong to the slice\n"
                                 "cannot be asked. Ordinary facts have no weight entry, so this is\n"
                                 "free unless the network carries a neural substrate. Clusters are\n"
                                 "session state and are in no .bin at all.\n"
                                 "\n"
                                 "Example (the Wikidata class hierarchy alone):\n"
                                 "  .lang wikidata\n"
                                 "  .save-predicates wikidata-20260309-P279.bin P279"},
#endif
            {".prune-facts", ".prune-facts <pattern>\n"
                             "Removes only the matching facts (statement nodes).\n"
                             "The pattern may contain variables in any position; a pattern WITHOUT\n"
                             "variables denotes one specific fact and removes exactly that one.\n"
                             "A pattern that matches nothing changes nothing -- in particular it does\n"
                             "not create the fact it describes.\n"
                             "Both commands remove CLAIMS. A statement that exists only as part of a\n"
                             "rule's own condition or consequence is graph structure, not data, and is\n"
                             "left alone; delete it with .remove <id> if that is really what you mean.\n"
                             "A statement that is a claim AND part of a rule's condition or consequence\n"
                             "loses the claim and stays as the rule's pattern: the rule stays in force.\n"
                             "Reports how many facts were removed."},

            {".prune-nodes", ".prune-nodes <pattern>\n"
                             ".prune-nodes <variable> (<conditions>)\n"
                             "Removes all matching facts AND the nodes bound to the pattern's variable.\n"
                             "Requirements of the single-fact form:\n"
                             "- The relation (predicate) must be fixed (no variable in predicate position)\n"
                             "- EXACTLY ONE variable, in subject or object position: it names what gets\n"
                             "  deleted. Two variables are rejected rather than silently read as one.\n"
                             "The second form takes a CONJUNCTION and is told which variable names the\n"
                             "victims, which is the one thing a conjunction cannot say by itself:\n"
                             "  .prune-nodes A (A P31 C, C P279∗ Q6999)\n"
                             "deletes every instance of a class at or below Q6999. The other conditions\n"
                             "are the filter that selected them -- their own facts are not removed, and\n"
                             "a transitive path condition (P⁺ / P∗) is what lets one command\n"
                             "replace a hand-written list of subclasses. Any number of variables is\n"
                             "allowed there and the predicates may vary per condition; only the named\n"
                             "variable's bindings die. The pattern has to be parenthesised for this\n"
                             "reading, which is what keeps '.prune-nodes A rel b' the single fact form.\n"
                             "WARNING: This is highly destructive! A deleted node takes everything\n"
                             "it is a PART of with it -- see .help .remove -- including facts and\n"
                             "rules that have nothing to do with the pattern, and its names.\n"
                             "Relation nodes left isolated by the deletion are removed by .cleanup.\n"
                             "Like .prune-facts, it removes CLAIMS only -- see .help .prune-facts.\n"
                             "Reports removed facts and nodes. From 100000 victims upwards it also\n"
                             "names the size of the job before it starts and counts through it, so a\n"
                             "prune of a full dump says where it is instead of running silently for\n"
                             "hours."},

            {".cleanup", ".cleanup\n"
                         "Removes all nodes that have no connections (isolated nodes).\n"
                         "Also cleans up associated entries in name mappings.\n"
                         "The engine's core nodes are exempt: several of them ('!', 'nil',\n"
                         "'conjunction', 'negation') carry no edges until something uses them."},

            {".new", ".new\n"
                     "Clears the complete network, including node names. Re-initializes core nodes.\n"
                     "Also clears the import registry, so all scripts can be imported again."},

            {".stat", ".stat\n"
                      "Shows current network statistics:\n"
                      "- Number of nodes\n"
                      "- RAM usage (in GiB, if available)\n"
                      "- Total entries in name-of-node mappings\n"
                      "- Total entries in node-of-name mappings\n"
                      "- Number of languages\n"
                      "- Number of rules"},
#ifndef __EMSCRIPTEN__
            {".stat-file", ".stat-file <file.bin>\n"
                           "Reads only the serialized zelph header from the given .bin file and prints\n"
                           "file size and chunk counts for left/right adjacency and name maps.\n"
                           "Does not load the network into memory, and does not read a single chunk --\n"
                           "which is what makes it instant on an 88 GB file, and also means that a file\n"
                           "truncated after the header still reports the counts the header declares.\n"
                           "Only a header whose counts cannot fit in the file at all is refused.\n"
                           "Use .index-file when you need the chunks themselves verified."},

            {".index-file", ".index-file <file.bin> <output.json>\n"
                            "Scans a serialized zelph .bin file sequentially and emits a JSON sidecar\n"
                            "containing byte offsets and lengths for the header and each chunk section.\n"
                            "Does not load the graph into the live network. Every chunk is read, so this\n"
                            "is the command that notices a truncated or corrupted file -- at the cost of\n"
                            "one pass over the whole file."},
#endif
            {".licenses", ".licenses\n"
                          "Lists all third-party software embedded in zelph, including their versions and licenses."},

            {".log", ".log <max-depth>\n"
                     "Enables detailed reasoning logging up to the given recursion depth.\n"
                     "-1 collects counters without log output - dump them with .prof\n"
                     "0 disables it.\n"
                     "Every line is correctly indented according to depth."},

            {".log-janet", ".log-janet\n"
                           "Toggles detailed logging of inputs and outputs for all zelph/* Janet functions.\n"
                           "Logs inputs at function entry and both inputs and output at exit."},

            {".prof", ".prof [reset]\n"
                      "Dumps the reasoning profiler on demand: the counter block plus the\n"
                      "top-10 relations by scanned candidate facts, relations by successful\n"
                      "matches, rules by application count, and rules by created facts.\n"
                      "Counters only accumulate while logging is active; the counter-only\n"
                      "mode \".log -1\" enables them WITHOUT any per-deduction log output.\n"
                      "While logging is active, counters accumulate across statements and\n"
                      "imports (the per-statement epoch reset is deliberately a no-op\n"
                      "then), so \".prof\" after an import shows the totals for the whole\n"
                      "import. \".prof reset\" additionally zeroes the counters, starting\n"
                      "a fresh measurement window -- use it between two phases."},

            {".auto-run", ".auto-run\n"
                          "Toggles the automatic execution of the inference engine (.run) after every input.\n"
                          "Default is ON. Automatically switches to OFF when .load is used.\n"
                          "It is a TOGGLE and takes no argument -- unlike its neighbours .anchors,\n"
                          ".semi-naive and .fact-stores, which read [on|off]. \".auto-run off\" is\n"
                          "therefore an error rather than a way to switch it off twice."},

            {".deductions", ".deductions [all|focus|quiet|off]\n"
                            "Sets the deduction printing mode for reasoning runs; without an\n"
                            "argument, shows the current mode (default: quiet).\n"
                            "  all    print every deduction (full derivation trace)\n"
                            "  focus  print only deductions about your own input, and say after\n"
                            "         each run how many were hidden:\n"
                            "           Note: 130 deductions were hidden.\n"
                            "  quiet  the same filter and the same lines, but silent about the\n"
                            "         rest: the PROMPT carries a '+' after a run that hid any.\n"
                            "         The default, because the count is worth one mark and not a\n"
                            "         sentence after every answer.\n"
                            "  off    print no deductions; a run says how many it hid, and the\n"
                            "         prompt carries the '+' as in quiet.\n"
                            "\n"
                            "WHAT 'own input' MEANS: the deduced fact's subject stems from a\n"
                            "statement of the SESSION -- typed, piped, or a line of a script run\n"
                            "as 'zelph script.zph'. It is the entered fact itself, or its subject,\n"
                            "or one of its objects. Anchors accumulate over the session; a module\n"
                            "loaded with .import contributes none, so a loaded library stays silent\n"
                            "about its internals. Switching modes resets the collected anchors.\n"
                            "\n"
                            "THE PROMPT MARK is the character '+'. It is the fact, not the count\n"
                            "-- a number in a prompt invites being read as a running total, and\n"
                            "this is a per-run figure. It clears after a run that hid nothing, and\n"
                            "it sits beside the '-' that marks a disabled auto-run, e.g.\n"
                            "\"zelph+-> \" -- where the part before the marks is the current\n"
                            "language and changes with .lang.\n"
                            "\n"
                            "'zelph script.zph' shows no prompt, so there quiet falls back to the\n"
                            "notice: a mark with nowhere to appear would leave a log that does not\n"
                            "say it is incomplete.\n"
                            "\n"
                            "All facts are derived and stored regardless of the mode. Query\n"
                            "answers and warnings are always printed. A contradiction's '!' line\n"
                            "is printed in every mode but off, whatever its subject; off prints\n"
                            "only the line \"Found one or more contradictions!\". The notice of\n"
                            "hidden deductions is printed on the same channel as the deduction\n"
                            "lines it accounts for, so a redirected transcript cannot keep the\n"
                            "gap and lose it, and a deduction written to a file by .run-export is\n"
                            "not withheld and is not counted.\n"
                            "\n"
                            "Heavy computations run several times faster with focus/quiet/off:\n"
                            "rendering large derived terms dominates the cost."},

            {".parallel", ".parallel\n"
                          "Toggles parallel processing on/off.\n"
                          "Default is on for performance.\n"
                          "\n"
                          "'on' is a permission, not an instruction: a bulk removal decides per\n"
                          "batch whether spreading its read phase over the pool is worth it, and\n"
                          "on a graph that fits in memory it is not -- with no page faults to\n"
                          "overlap, the extra threads only contend for the same locks and burn\n"
                          "CPU for the same wall time. It measures its own major faults and\n"
                          "keeps the pool exactly while the graph is being paged in, which is\n"
                          "the case that made the read phase parallel in the first place.\n"
                          "'off' means one thread everywhere, unconditionally."},

            {".anchors", ".anchors [on|off]\n"
                         "Controls the unification engine's anchor-based candidate lookups:\n"
                         "subject/object-driven snapshots, grounded-subject anchoring, and\n"
                         "partial-pattern anchoring. Without argument: shows the current state.\n"
                         "  on   – (default) conditions with a concrete anchor node scan only that\n"
                         "         node's adjacency instead of the whole relation extent. This is a\n"
                         "         pure index shortcut: results are identical to 'off'.\n"
                         "  off  – every condition scans the full relation extent (naive reference\n"
                         "         mode). Used by tests as an anchor-free completeness reference and\n"
                         "         for diagnosing suspected anchor bugs. Expect drastic slowdowns on\n"
                         "         arithmetic workloads.\n"
                         "Independent of .parallel: anchoring is active in both parallel and\n"
                         "single-core evaluation."},

            {".semi-naive", ".semi-naive [on|off|check]\n"
                            "Controls the fixpoint evaluation strategy of the reasoning engine.\n"
                            "Without argument: shows the current mode.\n"
                            "  on    – (default) delta-driven semi-naive evaluation: after a classic\n"
                            "          first pass, each further iteration seeds the rules with the\n"
                            "          facts created in the previous iteration: one condition is\n"
                            "          bound to a new fact, the others are matched against the graph.\n"
                            "          Rules with a negated condition run as classic passes whenever\n"
                            "          the delta has drained, one negation level at a time (see\n"
                            "          .strata), and the few rules whose conditions cannot be seeded\n"
                            "          (path, neural or nested conditions) are applied classically in\n"
                            "          every iteration. Results are identical to 'off', except where\n"
                            "          rules negate what they derive (see .strata); typically much\n"
                            "          faster on rule-heavy workloads such as the arithmetic modules.\n"
                            "  off   – classic naive evaluation: every iteration re-evaluates all\n"
                            "          rules against the whole graph.\n"
                            "  check – like 'on', but after the delta drains, classic verification\n"
                            "          passes run until quiescence. If any of them derives a fact the\n"
                            "          delta path missed, the run completes the fixpoint classically\n"
                            "          and then fails with a completeness-violation error. It also\n"
                            "          re-tests every negation a newer fact could have refuted, and\n"
                            "          fails when a fact rests on a negated premise that now holds\n"
                            "          and nothing else derives it -- a statement made after the\n"
                            "          question, or rules that negate what they derive. Not\n"
                            "          re-tested: a rule with a neural condition, a negated path\n"
                            "          condition, or a path condition binding a variable that no\n"
                            "          fact condition or conclusion binds. It does not run 'off'\n"
                            "          separately and compare. Intended for tests and debugging;\n"
                            "          the test binary enables it by default.\n"
                            "Single-pass runs (.run-once) and queries are unaffected by this setting;\n"
                            "after a .run-once, or a run with check mode or semi-naive evaluation\n"
                            "off, the next check run re-tests every recorded negation."},

            {".contradiction-records", ".contradiction-records [on|off]\n"
                                       "Shows or disables writing each detected contradiction into the graph.\n"
                                       "A contradiction is recorded as the refuted SET of the facts that\n"
                                       "matched -- \"these statements do not hold together\". Nothing is\n"
                                       "retracted: every one of them stays asserted and keeps answering\n"
                                       "queries. The record is what makes a contradiction reported ONCE\n"
                                       "instead of again on every later reasoning run, the same way a\n"
                                       "derived fact is quiet the second time because the graph holds it.\n"
                                       "  (no argument) – show the current state\n"
                                       "  off – report every contradiction on every run, and add nothing to\n"
                                       "        the graph. One set node per DISTINCT contradiction is the\n"
                                       "        cost; on a Wikidata-scale audit that is six figures.\n"
                                       "  on  – the default\n"
                                       "See also: .run-export, which records every contradiction a run\n"
                                       "encounters whether or not the line was printed."},
            {".fact-stores", ".fact-stores [on|off]\n"
                             "Shows or disables the fact-path acceleration stores: the genuine-\n"
                             "structure store (exact triple per created fact node) and the\n"
                             "template-variable store (exact variable set per rule-template node).\n"
                             "They make structure and variable lookups O(1) but grow by roughly\n"
                             "150 bytes per fact created through the normal fact() path.\n"
                             "  (no argument) – show the current state\n"
                             "  off – disarm and free the stores; all lookups fall back to the\n"
                             "        slower, semantically identical reconstruction walks. Use\n"
                             "        BEFORE bulk-building large graphs through the fact() API,\n"
                             "        e.g. a Janet mass importer. Trusted imports and binary\n"
                             "        loads (.load) disable the stores automatically.\n"
                             "  on  – valid only while the stores are still enabled: they cannot\n"
                             "        be re-armed retroactively, because ABSENCE of an entry is\n"
                             "        meaningful while a store is authoritative.\n"
                             "Restart zelph to start with stores enabled again."},
#ifndef __EMSCRIPTEN__
            {".wikidata-constraints", ".wikidata-constraints <json_file> <output_dir>\n"
                                      "Processes the Wikidata dump and exports constraint scripts\n"
                                      "to the specified output directory."},

            {".wikidata-qualifiers", ".wikidata-qualifiers <wikidata-dump.json[.bz2]> [P-id1 P-id2 ...]\n"
                                     "Imports statement qualifiers from a Wikidata JSON dump into the current network.\n"
                                     "Without P-ids: imports all qualifiers. With P-ids: imports only qualifiers whose\n"
                                     "property is listed (e.g. P11260); a statement is only materialized if it\n"
                                     "contributes at least one matching qualifier.\n"
                                     "\n"
                                     "For each such statement, reified structures are created as ordinary nodes and\n"
                                     "facts in the 'wikidata' language (matching the RDF statement layer):\n"
                                     "  <subject>   p:<P>          <statement>   – links entity to its statement node\n"
                                     "  <statement> ps:<P>         <main value>  – the statement's main value\n"
                                     "  <statement> pq:<Pq>        <value>       – one fact per imported qualifier\n"
                                     "  <statement> wikibase:rank  wikibase:{Normal|Preferred|Deprecated}Rank\n"
                                     "Statement nodes are named by their Wikidata statement ID (contains '$').\n"
                                     "\n"
                                     "IMPORTANT: Load the base network first (.load <all.bin>), so that subjects and\n"
                                     "entity values attach to the existing Q/P nodes via their 'wikidata' names.\n"
                                     "The import is idempotent and incremental: re-running it (e.g. with additional\n"
                                     "qualifier properties) extends the loaded network. Persist the combined result\n"
                                     "with .save <file.bin>.\n"
                                     "\n"
                                     "Value handling: entity values use their Q/P ID; time, quantity, string and\n"
                                     "monolingual text values become nodes named by their raw value (e.g.\n"
                                     "'+2020-01-01T00:00:00Z', '+42'). novalue/somevalue and coordinates are skipped.\n"
                                     "\n"
                                     "Example (disjointness analysis, 'list item' qualifiers on P2738):\n"
                                     "  .load wikidata-20260309-all.bin\n"
                                     "  .wikidata-qualifiers wikidata-20260309-all.json.bz2 P11260\n"
                                     "  .save wikidata-20260309-all-P11260.bin"},

            {".export-wikidata", ".export-wikidata <wikidata-dump.json> <Qid1> [Qid2 ...]\n"
                                 "Extracts the exact JSON line for each given Wikidata ID (Q…)\n"
                                 "from the dump and writes it to <id>.json in the current directory.\n"
                                 "The dump can be .json or .json.bz2.\n"
                                 "No import, no .bin cache, no network – pure extraction."},
#endif
            {".cluster", ".cluster [name]\n"
                         "Without argument: lists all clusters with node counts and shows the active one.\n"
                         "With argument: activates the named cluster (created if needed). All nodes and\n"
                         "facts created from now on are recorded in it — including relation nodes,\n"
                         "rule definitions, query patterns, and facts deduced by .run. Facts that\n"
                         "already existed before are never recorded -- with one exception a drop has\n"
                         "to undo all the same: claiming a statement that was only a rule's ground\n"
                         "pattern revokes that marking, and .cluster-drop puts it back. The node\n"
                         "existed, so nothing was created and nothing would otherwise be rolled back,\n"
                         "and the experiment would have turned the rule's patterns into data for good.\n"
                         "'.cluster default' deactivates cluster tracking.\n"
                         "Note: clusters are session state and are not persisted by .save."},

            {".cluster-drop", ".cluster-drop <name>\n"
                              "Removes every node recorded in the cluster, with its names (rollback\n"
                              "semantics). Pre-existing knowledge is untouched EXCEPT where it is built\n"
                              "on a cluster node: a fact created outside the cluster that names one goes\n"
                              "with it, and so does a rule one of its conditions belongs to -- see\n"
                              ".help .remove. A fact that merely lost a part could not be told from a\n"
                              "different fact.\n"
                              "The reported count is what actually went, which is more than the cluster\n"
                              "recorded whenever such a fact existed.\n"
                              "Dropping the ACTIVE cluster falls back to 'default', i.e. tracking stops.\n"
                              "An unknown name is an error, not a no-op.\n"
                              "WARNING: destructive and irreversible."},

            {".cluster-merge", ".cluster-merge <from> <to>\n"
                               "Moves the membership bookkeeping of <from> into <to> (commit semantics).\n"
                               "No nodes or edges are touched. If <to> is 'default', the nodes simply\n"
                               "become ordinary nodes."},
        };

        if (cmd[0] == ".help")
        {
            if (cmd.size() == 1)
            {
                for (const auto& l : general_help_lines)
                    _n->out(l, true);
            }
            else if (cmd.size() == 2)
            {
                // The listing prints every command with its dot, so that is
                // what gets pasted -- but typing the bare name is at least as
                // natural, and ".help deductions" used to answer "Unknown
                // command: deductions" about a command that exists.
                std::string topic = cmd[1];
                if (!topic.empty() && topic.front() != '.') topic.insert(topic.begin(), '.');

                if (const auto alias = command_aliases.find(topic); alias != command_aliases.end())
                    topic = alias->second;

                auto it = detailed_help.find(topic);
                if (it != detailed_help.end())
                {
                    _n->out(it->second, true);
                }
                else
                {
                    _n->error("Unknown command: " + cmd[1] + ". Use \".help\" for a list of all commands.", true);
                }
            }
            else
            {
                throw std::runtime_error("Usage: .help [command]");
            }
        }
    }
}
