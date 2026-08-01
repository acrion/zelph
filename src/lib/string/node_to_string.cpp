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

#include "node_to_string.hpp"

#include "network/fact_structure.hpp"
#include "network/zelph.hpp"
#include "string/string_utils.hpp"

#include <atomic>
#include <mutex>

// #define DEBUG_FORMAT_FACT

namespace zelph::string
{
    int                  format_fact_level = 0;
    std::recursive_mutex mtx;
}

using namespace zelph::string;

bool zelph::string::is_var(std::string token)
{
    // Legacy helper, might still be useful outside of PEG context
    static const std::string variable_names("ABCDEFGHIJKLMNOPQRSTUVWXYZ_");
    if (token.empty()) return false;
    if (token.size() == 1) return variable_names.find(*token.begin()) != std::string::npos;
    return *token.begin() == '_';
}

// The self-fact sugar ":pred subject" is only used where it reads back
// as the fact it renders, and that is a property of the PREDICATE's
// name. Two conditions, each for its own reason:
//
//   - the sugar's own rule captures the predicate as `(some :symchars)`,
//     so a name carrying a reserved character ends it early -- and that
//     rules out the bare atoms `>` or `=>`, which needs_quotes exempts;
//   - the name has to print BARE, since there is no way to quote it
//     inside the sugar. That is exactly !needs_quotes, so the sugar
//     follows the quoting rules instead of restating them: a predicate
//     named "&12" or "≈net" keeps the verbose form.
//
// Whether a predicate is suppressed by a module is graph state and stays
// with the caller.
bool zelph::string::selffact_sugar_safe(const std::string& name)
{
    if (name.empty() || is_var(name)) return false;

    for (const char ch : name)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= ' ') return false; // whitespace and ASCII control characters
        switch (c)
        {
        case '<':
        case '>':
        case '(':
        case ')':
        case '{':
        case '}':
        case '*':
        case ',':
        case '"':
            return false; // PEG-reserved structure characters
        default:
            break;
        }
        if (c == 0xC2) return false; // UTF-8 lead byte of '¬', '«', '»'
    }

    return prints_bare(name);
}

bool zelph::string::is_inside_node_to_wstring()
{
    return format_fact_level > 0;
}

namespace zelph::string
{
    namespace
    {
        std::atomic<network::Node> _last_node_to_string_node{};
    }

    network::Node last_node_to_string_node()
    {
        return _last_node_to_string_node.load(std::memory_order_relaxed);
    }

    void reset_last_node()
    {
        _last_node_to_string_node.store(network::Node{}, std::memory_order_relaxed);
    }
}

namespace
{
    // Mark the NAME of a node as a leaf, so that quoting and the derivation
    // export can tell it from the structure around it -- unless the node is
    // a VARIABLE, whose name is meant to be read back as a variable and
    // therefore has to stay bare. Which of the two it is cannot be seen in
    // the string: a node can genuinely be named "A" or "_x", and printing
    // that bare made the line read back as a variable instead.
    std::string mark_leaf(const zelph::network::Node node, const std::string& name)
    {
        if (zelph::network::Zelph::is_var(node)) return name;
        return zelph::string::mark_identifier(name);
    }

    // In-place: dec (MSB-first decimal digit string) := dec * base + add.
    // Pure string arithmetic, so arbitrarily large numbers work. Used to
    // convert a registered-digit cons list (any base) to its decimal
    // &-literal display.
    void mul_add_decimal(std::string& dec, const uint64_t base, const uint64_t add)
    {
        uint64_t carry = add;
        for (auto it = dec.rbegin(); it != dec.rend(); ++it)
        {
            const uint64_t v = static_cast<uint64_t>(*it - '0') * base + carry;
            *it              = static_cast<char>('0' + v % 10);
            carry            = v / 10;
        }
        while (carry != 0)
        {
            dec.insert(dec.begin(), static_cast<char>('0' + carry % 10));
            carry /= 10;
        }
    }

    // A leaf name is writable in the scheme iff it matches the declared
    // identifier grammar. A scheme that declares no grammar can never
    // deviate -- which is the safe default, not a limitation.
    bool scheme_name_ok(const zelph::network::DisplayScheme& scheme, const std::string& name)
    {
        if (name.empty() || scheme.name_first.empty() || scheme.name_chars.empty()) return false;
        if (scheme.name_first.find(name.front()) == std::string::npos) return false;
        for (std::size_t i = 1; i < name.size(); ++i)
            if (scheme.name_chars.find(name[i]) == std::string::npos) return false;
        return true;
    }
}

void zelph::string::node_to_string(const network::Zelph* const z, std::string& result, const std::string& lang, network::Node node, const int max_objects, const network::Variables& variables, network::Node parent, std::shared_ptr<std::unordered_set<network::Node>> history, SchemeContext* ctx)
{
    // Formats a node into a string representation.

    struct IncDec
    {
        explicit IncDec(int& n)
            : _n(n) { ++_n; }
        ~IncDec() { --_n; }
        int& _n;
    };

    std::lock_guard lock(mtx);

    IncDec incDec(format_fact_level);

    // One table snapshot per top-level rendering: the recursion reads it
    // through a raw pointer, so nested nodes pay neither a lock nor an
    // atomic refcount. Without a registered scheme the tables are empty and
    // every check below is a single empty() test.
    std::shared_ptr<const network::DisplayTables> tables_owner;
    SchemeContext                                 root_ctx;
    if (!ctx)
    {
        tables_owner    = z->display_tables();
        root_ctx.tables = tables_owner.get();
        ctx             = &root_ctx;
    }
    const bool scheme_enabled = ctx->tables && !ctx->tables->operators.empty() && !ctx->no_scheme;

    if (!history)
    {
        // Record what this rendering is ABOUT (consumed by ".node" and
        // ".explain" without an argument). A query answer renders a
        // PATTERN plus its bindings -- "(&6 + &7) = _Result" with
        // _Result = &13 -- so the pattern node is NOT what the user just
        // read; resolve the bindings to the fact it denotes. Pure hash
        // lookups, and skipped entirely when there are no bindings, which
        // is every plain fact and every deduced consequence.
        _last_node_to_string_node.store(network::resolve_pattern_node(z, node, variables), std::memory_order_relaxed);
    }

#ifdef DEBUG_FORMAT_FACT
    std::string indent(format_fact_level * 2, ' ');
    z->diagnostic_stream() << indent << "[DEBUG node_to_string] ENTRY node=" << node << " parent=" << parent << std::endl;
#endif

    if (!history) history = std::make_shared<std::unordered_set<network::Node>>();

    // Helper to resolve variables
    auto resolve_var = [&](network::Node n) -> network::Node
    {
        int           limit = 0;
        network::Node curr  = n;
        while (network::Zelph::is_var(curr) && limit++ < 100)
        {
            auto it = variables.find(curr);
            if (it == variables.end() || it->second == 0 || it->second == curr) break;
            curr = it->second;
        }
        return curr;
    };

    // 1. Variable Substitution
    network::Node resolved = resolve_var(node);

    if (history->find(resolved) != history->end())
    {
#ifdef DEBUG_FORMAT_FACT
        z->diagnostic_stream() << indent << "[DEBUG node_to_string] HIT HISTORY for node=" << resolved << " -> returning '?'" << std::endl;
#endif
        result = "?";
        return;
    }

    auto is_statement_node = [&](network::Node nd) -> bool
    {
        if (nd == 0 || !z->exists(nd)) return false;

        network::Node pred = z->parse_relation(nd);
        if (pred == 0) return false;

        // Cons cells are formatted earlier as lists; still a statement node structurally,
        // but we don't want to force "(...)" around list syntax.
        if (pred == z->core.Cons) return true;

        // Predicate must be a relation type
        if (!z->check_fact(pred, z->core.IsA, {z->core.RelationTypeCategory}).is_correct())
            return false;

        const auto& r = z->get_right(nd);
        const auto& l = z->get_left(nd);

        // Must actually link to its predicate
        if (r.count(pred) == 0) return false;

        // Must have at least one bidirectional neighbor besides the predicate (= subject candidate)
        for (network::Node x : r)
            if (x != pred && l.count(x) != 0)
                return true;

        return false;
    };

    const bool resolved_is_stmt = is_statement_node(resolved);

    // 2. Name Check
    // If the node has a direct name, use it.
    std::string name = z->get_formatted_name(resolved, lang);
    if (!name.empty())
    {
#ifdef DEBUG_FORMAT_FACT
        z->diagnostic_stream() << indent << "[DEBUG node_to_string] Found name '" << name << "' for node " << resolved << std::endl;
#endif
        if (ctx->active && scheme_name_ok(ctx->tables->schemes[ctx->scheme], name))
        {
            ctx->expressible = true;
            ctx->atomic      = true;
        }

        result = mark_leaf(resolved, name);
        return;
    }

    // 2b. Unnamed variables must never be expanded structurally: a
    // variable node's edges only reflect the rule patterns it occurs
    // in, so the structural formatting below would dump rule topology
    // instead of a value.
    if (network::Zelph::is_var(resolved))
    {
        result = string::mark_identifier("??");
        return;
    }

    // 3. Cons List Detection (Sequence)
    // Check if 'resolved' is a cons cell (relation node whose predicate is Cons).
    // If so, walk the cons chain and format as < e1 e2 ... en >.
    if (z->exists(resolved))
    {
        network::Node rel_type = z->parse_relation(resolved);
        if (rel_type == z->core.Cons)
        {
#ifdef DEBUG_FORMAT_FACT
            z->diagnostic_stream() << indent << "[DEBUG node_to_string] DETECTED CONS LIST starting at " << resolved << std::endl;
#endif
            auto child_history = std::make_shared<std::unordered_set<network::Node>>(*history);
            child_history->insert(resolved);

            std::vector<network::Node>        list_elements;
            network::Node                     current = resolved;
            std::unordered_set<network::Node> visited_cells;

            while (current != 0 && current != z->core.Nil && z->exists(current))
            {
                if (visited_cells.count(current)) break; // cycle protection
                visited_cells.insert(current);

                if (z->parse_relation(current) != z->core.Cons) break; // not a cons cell

                network::adjacency_set objs;
                network::Node          car = z->parse_fact(current, objs, 0);
                if (car != 0) car = resolve_var(car);
                if (car != 0)
                    list_elements.push_back(car);

                // Get cdr (rest of list) — the single object of this cons cell
                network::Node cdr = z->core.Nil;
                for (network::Node o : objs)
                {
                    cdr = resolve_var(o);
                    break;
                }
                current = cdr;
            }

            // Terminated chains (ending at nil) render as lists below.
            // A chain ending anywhere else -- a variable (rule patterns
            // like (A cons R)), an atom, or a fact node -- is NOT a proper
            // list: rendering only the collected cars would silently drop
            // the tail (the historical behavior turned the rule pattern
            // (A cons R) into <A>). Such chains render in explicit cons
            // input syntax instead, which round-trips through the parser
            // and shows rule patterns exactly as their author wrote them.
            const bool proper_list = (current == z->core.Nil);

            if (!list_elements.empty() && !proper_list)
            {
                auto child_history = std::make_shared<std::unordered_set<network::Node>>(*history);
                child_history->insert(resolved);

                // Render right-associatively: (a cons (b cons tail)).
                std::string tail_str;
                node_to_string(z, tail_str, lang, current, max_objects, variables, resolved, child_history);
                if (tail_str.empty()) tail_str = string::mark_identifier("?");

                const std::string cons_name = string::mark_identifier(z->get_formatted_name(z->core.Cons, lang));

                for (auto it = list_elements.rbegin(); it != list_elements.rend(); ++it)
                {
                    std::string car_str;
                    node_to_string(z, car_str, lang, *it, max_objects, variables, resolved, child_history);
                    if (car_str.empty()) car_str = string::mark_identifier("?");

                    tail_str = "(" + car_str + " " + cons_name + " " + tail_str + ")";
                }

                result = tail_str;

                // The outermost wrap is already provided by the loop above;
                // strip it when this node is NOT embedded in a parent, to
                // match the top-level convention of the S-P-O formatter.
                if (parent == 0 && result.size() >= 2 && result.front() == '(' && result.back() == ')')
                    result = result.substr(1, result.size() - 2);

                return;
            }

            if (!list_elements.empty())
            {
                // Number display: if a digit alphabet is registered via
                // zelph/set-number-digits (see stdlib/decimal-arithmetic.zph), a
                // properly nil-terminated cons list consisting solely of
                // registered digit nodes is rendered as a decimal &-literal
                // -- the exact inverse of the &-input syntax (zelph/number).
                // Any other cons list (unterminated chains, variables,
                // unregistered elements) falls through to the generic <...>
                // display below, so cons lists remain general-purpose.
                if (const auto digit_values = z->number_digit_values())
                {
                    if (current == z->core.Nil) // walk must have ended at the nil terminator
                    {
                        // Canonicity gate: a multi-cell list whose most
                        // significant cell (the LAST element in LSB-first
                        // storage) holds the zero digit is value-correct but
                        // non-canonical -- e.g. the zero-extended raw product
                        // <00> or the raw difference <007>. Rendering such
                        // nodes as &-literals made them indistinguishable
                        // from the canonical node of the same value, which is
                        // exactly how the &3 * &0 zero-extension bug stayed
                        // invisible in REPL traces. They fall through to the
                        // raw <...> display instead. The single-cell zero <0>
                        // IS the canonical zero and keeps its &0 rendering.
                        bool canonical = true;
                        if (list_elements.size() > 1)
                        {
                            const auto msb = digit_values->find(resolve_var(list_elements.back()));
                            if (msb != digit_values->end() && msb->second == 0)
                                canonical = false;
                        }

                        if (canonical)
                        {
                            bool           all_digits = true;
                            std::string    dec        = "0";
                            const uint64_t base       = static_cast<uint64_t>(digit_values->size());

                            // list_elements is LSB-first; iterate MSB-first and
                            // accumulate dec = dec * base + digit_value.
                            for (auto it = list_elements.rbegin(); it != list_elements.rend(); ++it)
                            {
                                const network::Node eff = resolve_var(*it);
                                const auto          dv  = digit_values->find(eff);
                                if (network::Zelph::is_var(eff) || dv == digit_values->end())
                                {
                                    all_digits = false;
                                    break;
                                }
                                mul_add_decimal(dec, base, dv->second);
                            }

                            if (all_digits)
                            {
                                if (ctx->active)
                                {
                                    // The scheme declares how it writes numbers.
                                    const network::DisplayScheme& sch = ctx->tables->schemes[ctx->scheme];
                                    result                            = sch.numeral_prefix + dec;
                                    ctx->expressible                  = true;
                                    if (sch.numeral_prefix != "&") ctx->deviated = true;
                                }
                                else
                                {
                                    result = "&" + dec;
                                }
                                return;
                            }
                        }
                    }
                }

                // The reversal below is the NUMERAL convention (LSB-first
                // storage, MSB-first display) and is correct only for
                // numerals. A list of single-character nodes that are not
                // registered digits is an ordinary node list and must echo
                // in storage order, like every other one.
                bool       all_single_char = true;
                bool       has_var         = false;
                bool       all_digits      = true;
                const auto digit_values    = z->number_digit_values();

                for (network::Node e : list_elements)
                {
                    network::Node eff = resolve_var(e);
                    std::string   nm  = z->get_formatted_name(eff, lang);
                    if (nm.length() != 1)
                    {
                        all_single_char = false;
                    }
                    if (network::Zelph::is_var(eff) || string::is_var(nm))
                    {
                        has_var = true;
                    }
                    if (!digit_values || digit_values->find(eff) == digit_values->end())
                    {
                        all_digits = false;
                    }
                }

                if (all_single_char && all_digits && !has_var)
                {
                    // Reverse for display: stored order is LSB-first (e.g.[3,2,1] for 123),
                    // conventional display is MSB-first. Omit spaces to match input syntax.
                    std::vector<network::Node> display_elements(list_elements.rbegin(), list_elements.rend());

                    result = "<";
                    for (network::Node e : display_elements)
                    {
                        std::string elem_str;
                        string::node_to_string(z, elem_str, lang, resolve_var(e), max_objects, variables, resolved, child_history);
                        result += zelph::string::trim_any_of(elem_str, {"«", "»"});
                    }
                    result += ">";
                }
                else
                {
                    std::string content;
                    bool        first = true;
                    for (network::Node e : list_elements)
                    {
                        if (!first) content += " ";
                        std::string elem_str;
                        string::node_to_string(z, elem_str, lang, e, max_objects, variables, resolved, child_history);

                        // Wrap in parentheses if it's a composite expression (not a simple name).
                        if (!elem_str.empty()
                            && elem_str.find(' ') != std::string::npos
                            && elem_str.front() != '('
                            && elem_str.front() != '<'
                            && elem_str.front() != '{')
                        {
                            network::Node eff_e = resolve_var(e);
                            // The rendering is marked, so the name it is
                            // compared against has to be marked too -- else a
                            // plain name with a space came out as ("a b").
                            if (elem_str != mark_leaf(eff_e, z->get_formatted_name(eff_e, lang)))
                                elem_str = "(" + elem_str + ")";
                        }

                        content += elem_str;
                        first = false;
                    }

                    // A list whose content carries no whitespace is read
                    // back by the COMPACT rule, which makes one node per
                    // CHARACTER: <item2> re-reads as <2 m e t i>, silently.
                    // Only a one-element list can get there -- a separator
                    // is whitespace -- and only when its element is longer
                    // than one character, since for a single character both
                    // readings mean the same list. That is what keeps <7>
                    // and the digit lists untouched. The ambiguous case is
                    // padded out to the input form that produces it.
                    //
                    // Judged on the PRINTED form, because the identifier
                    // markers are still in `content` and would count as
                    // characters of their own -- which made even <7> look
                    // ambiguous.
                    const std::string printed = string::unmark_identifiers(content);
                    const bool        compact = printed.size() > 1
                                      && printed.find_first_of(" \t\r\n>") == std::string::npos;

                    result = compact ? "< " + content + " >" : "<" + content + ">";
                }
                return;
            }
        }
    }

    // 4. Container Detection (Set)
    // Check if 'resolved' acts as a container (Object in a PartOf relation).
    std::unordered_set<network::Node> elements;

    if (z->exists(resolved))
    {
        for (network::Node rel : z->get_right(resolved))
        {
            if (rel == parent) continue;

            network::Node p = z->parse_relation(rel);
            if (p == z->core.PartOf)
            {
                network::adjacency_set objs;
                network::Node          s = z->parse_fact(rel, objs, 0);
                if (s != 0) s = resolve_var(s);

                if (s != 0 && objs.count(resolved) > 0)
                {
                    elements.insert(s);
                }
            }
        }
    }

    if (!elements.empty())
    {
#ifdef DEBUG_FORMAT_FACT
        z->diagnostic_stream() << indent << "[DEBUG node_to_string] DETECTED SET with " << elements.size() << " elements." << std::endl;
#endif
        auto child_history = std::make_shared<std::unordered_set<network::Node>>(*history);
        child_history->insert(resolved);

        std::vector<network::Node> sorted_elements(elements.begin(), elements.end());
        std::sort(sorted_elements.begin(), sorted_elements.end());

        result     = "{";
        bool first = true;

        // A rendering that already carries a scheme's own delimiters is
        // self-delimiting; wrapping it again would produce "($( ... ))".
        SchemeContext elem_ctx;
        elem_ctx.tables = ctx->tables;

        for (network::Node e : sorted_elements)
        {
            if (!first) result += " ";
            std::string elem_str;
            elem_ctx.self_delimited = false;
            node_to_string(z, elem_str, lang, e, max_objects, variables, resolved, child_history, &elem_ctx);

            if (!elem_ctx.self_delimited
                && !elem_str.empty()
                && elem_str.find(' ') != std::string::npos
                && elem_str.front() != '('
                && elem_str.front() != '<'
                && elem_str.front() != '{')
            {
                network::Node eff_e = resolve_var(e);
                // Compare against the MARKED name, as the S-P-O formatter
                // does: the rendering of a plain name is marked.
                if (elem_str != mark_leaf(eff_e, z->get_formatted_name(eff_e, lang)))
                {
                    elem_str = "(" + elem_str + ")";
                }
            }

            result += elem_str;
            first = false;
        }
        result += "}";

        return;
    }

    // 5. Proxy / Instance Detection
    // An ANONYMOUS, STRUCTURELESS node that is an instance of a concept is
    // shown as that concept -- a node a rule created for a fresh variable
    // has nothing else to show.
    //
    // A node with a structure of its OWN is never substituted: (x + y)
    // stays (x + y) after (x + y) ~ t, because "is an instance of" is not
    // "is". Substituting it hid every tagged term behind its concept and
    // echoed the statement as "t ~ t", which is not re-enterable input.
    bool is_negation = false;
    if (z->exists(resolved))
    {
        // Exactly the condition under which the structural path below can
        // produce a rendering (it falls back on parse_relation too).
        const bool has_own_structure = z->parse_relation(resolved) != 0;

        for (network::Node rel : z->get_right(resolved))
        {
            if (z->parse_relation(rel) == z->core.IsA)
            {
                network::adjacency_set type_objs;
                network::Node          type_subj = z->parse_fact(rel, type_objs, 0);

                // Ensure 'resolved' is the subject (The Instance)
                if (type_subj == resolved && !type_objs.empty())
                {
                    network::Node concept_node = *type_objs.begin();

                    // Avoid self-reference loops
                    if (concept_node != resolved)
                    {
                        if (concept_node == z->core.Negation)
                        {
                            is_negation = true;
                            continue; // Skip metadata, we will format it structurally below
                        }

                        if (has_own_structure) continue; // structure wins over the concept

                        string::node_to_string(z, result, lang, concept_node, max_objects, variables, parent, history);

                        if (!result.empty() && result != "?")
                        {
                            return;
                        }
                    }
                }
            }
        }
    }

    // 6. Standard Fact Formatting (S P O)
    // Only if it wasn't a container or a simple proxy do we treat it as a structural fact.

#ifdef DEBUG_FORMAT_FACT
    z->diagnostic_stream() << indent << "[DEBUG node_to_string] Standard path (Statement/Fact)." << std::endl;
#endif

    network::adjacency_set objects;
    network::Node          subject            = 0;
    network::Node          recorded_predicate = 0;

    // Prefer the RECORDED structure over any reconstruction: the genuine-
    // structure store holds the exact triple fact() was called with, so
    // there is no ambiguity from the facts this node is merely PART of.
    // Without it, z->parse_fact's candidate filter discards a structured
    // subject as soon as it carries predicates of its own (a term that
    // topoly has compiled acquires aspoly/needstopoly/mul), and the
    // hand-rolled walk below then picks an arbitrary bidirectional
    // neighbour -- including the very parent being rendered, which the
    // history check turns into "?".
    {
        const network::FactStructure fs = network::get_preferred_structure(z, resolved, 3);
        if (fs.predicate != 0 && fs.subject != 0 && fs.subject != parent && !fs.objects.empty())
        {
            subject            = fs.subject;
            objects            = fs.objects;
            recorded_predicate = fs.predicate;
        }
    }

    // The predicate comes from the same recorded triple as subject and
    // objects. z->parse_relation only recognises a predicate that is a
    // DECLARED relation type, so a fact or cons node used as a predicate
    // ("x (a p b) y", "deep_nesting ~ (Level1 (Level2 ...) Level1Object)")
    // resolved to 0 and rendered as "??" -- a line that cannot be entered
    // again, although the very same statement parses and matches as input.
    // Where parse_relation succeeds it returns exactly fs.predicate, so
    // nothing else changes; where no triple was recorded (a subject ==
    // predicate fact, or a network whose stores a .load has disarmed) the
    // reconstruction is still the only source.
    const auto fact_predicate = [&]() -> network::Node
    {
        return recorded_predicate != 0 ? recorded_predicate : z->parse_relation(resolved);
    };

    if (subject == 0) subject = z->parse_fact(resolved, objects, parent);

    // A node is only readable as a fact if it points at a PREDICATE. The
    // subject <-> fact link is symmetric, so parse_fact run on a node that
    // merely IS some fact's subject hands back that very fact as the node's
    // own "subject", and the triple built from it came out as "(?? ?? ??)" --
    // an answer line nobody can enter again. The `parent` argument suppresses
    // it while the containing fact is being rendered, which is why the same
    // node printed as "??" in a deduction line and as "(?? ?? ??)" in the
    // answer to a query. A generated node (a fresh variable's witness) hits
    // this whenever it is the subject of anything, i.e. as a rule.
    if (subject != 0 && fact_predicate() == 0)
    {
        result = string::mark_identifier("??");
        return;
    }

    bool is_condition = false;

#ifdef DEBUG_FORMAT_FACT
    z->diagnostic_stream() << indent << "[DEBUG node_to_string] z->parse_fact result: subject=" << subject << ", objects_count=" << objects.size() << ", is_condition=" << is_condition << std::endl;
#endif

    if (subject == 0 && !is_condition)
    {
        // z->parse_fact failed to identify the subject. This happens when the subject is itself
        // a hash node (e.g. a cons cell) whose outgoing edges contain structural predicates,
        // causing z->parse_fact's candidate filter to discard it. We fall back to direct graph
        // inspection: the subject is the unique node that is bidirectionally connected to
        // `resolved` (i.e. present in both get_right and get_left), which is the defining
        // invariant established by fact() via connect(subject, relation) + connect(relation, subject).
        network::Node fallback_pred = z->parse_relation(resolved);
        network::Node fallback_subj = 0;

        if (fallback_pred != 0)
        {
            for (network::Node nd : z->get_right(resolved))
            {
                // nd is the subject iff it is bidirectional with resolved (in both left and right)
                // and is neither the predicate itself nor the fact currently being rendered.
                // The parent exclusion mirrors z->parse_fact: `resolved` is bidirectional with
                // EVERY fact it is the subject of, including its own parent, so without this
                // guard the walk can pick the ancestor -- which the history check then renders
                // as '?'.
                if (nd != fallback_pred && nd != parent && z->has_left_edge(resolved, nd))
                {
                    fallback_subj = nd;
                    break;
                }
            }
        }

        if (fallback_subj == 0)
        {
#ifdef DEBUG_FORMAT_FACT
            z->diagnostic_stream() << indent << "[DEBUG node_to_string] INVALID: Subject is 0 after fallback. Returning '??'" << std::endl;
#endif
            result = string::mark_identifier("??");
            return;
        }

#ifdef DEBUG_FORMAT_FACT
        z->diagnostic_stream() << indent << "[DEBUG node_to_string] Fallback subject found: " << fallback_subj << std::endl;
#endif

        // Reconstruct objects: nodes in get_left(resolved) that are neither the subject
        // nor bidirectionally connected (predicates and parents appear in get_right too).
        subject = fallback_subj;
        objects.clear();
        for (network::Node nd : z->get_left(resolved))
        {
            if (nd != fallback_subj && !z->has_right_edge(resolved, nd))
            {
                objects.insert(nd);
            }
        }
        if (objects.empty())
        {
            // Self-referential fact: subject is its own object.
            objects.insert(fallback_subj);
        }
    }

    // Self-fact display sugar: a fact whose subject and object are the same
    // node renders as ":pred subject" -- the display inverse of the ":pred X"
    // input sugar. Restricted to predicate names for which the sugar form
    // ROUND-TRIPS through the parser: only PEG symchars (no whitespace, no
    // reserved structure characters such as '*', '<', '>', ',', quotes, or
    // '¬'), and not shaped like a variable (":A x" would re-parse with
    // variable semantics). Everything else -- including hash-consed numeric
    // self-facts on '*' such as (&9 * &9) -- keeps the verbose "S P S" form.
    bool          self_fact_sugar = false;
    std::string   self_fact_pred;
    network::Node self_fact_rel = 0;
    if (subject != 0 && objects.size() == 1
        && resolve_var(*objects.begin()) == resolve_var(subject))
    {
        const network::Node rel_node = resolve_var(fact_predicate());
        const std::string   rel_name = z->get_formatted_name(rel_node, lang);
        if (string::selffact_sugar_safe(rel_name) && !z->selffact_sugar_suppressed(rel_node))
        {
            self_fact_sugar = true;
            self_fact_pred  = rel_name;
            self_fact_rel   = rel_node;
        }
    }

    // --- Script-registered display scheme -------------------------------
    // An operator fact renders under its scheme only if the WHOLE subtree
    // is writable in that scheme; otherwise the default form is produced
    // unchanged. The check is not a separate traversal: children report
    // back through the context, and a boundary node that learns its subtree
    // is not writable re-renders itself once with the scheme disabled.
    const network::OperatorDisplay* op = nullptr;
    if (scheme_enabled && !is_negation && subject != 0 && objects.size() == 1 && !self_fact_sugar)
    {
        const network::Node pred = resolve_var(fact_predicate());
        const auto          it   = ctx->tables->operators.find(pred);
        if (it != ctx->tables->operators.end()) op = &it->second;
    }

    const bool in_scheme     = op && ctx->active && ctx->scheme == op->scheme;
    const bool scheme_render = op != nullptr;
    const bool application   = op && op->form == network::OperatorDisplay::Form::Application;

    auto child_history = std::make_shared<std::unordered_set<network::Node>>(*history);
    child_history->insert(resolved);

    SchemeContext child_ctx;
    child_ctx.tables     = ctx->tables;
    child_ctx.scheme     = op ? op->scheme : 0;
    child_ctx.precedence = op ? op->precedence : 0;
    child_ctx.assoc      = op ? op->assoc : -1;
    child_ctx.active     = scheme_render;
    child_ctx.enclosed   = application; // the call notation brings its own
    bool children_ok     = true;
    bool subject_atomic  = false;

    std::string subject_name, relation_name;

    if (!is_condition || subject)
    {
        std::string s_str;
        child_ctx.expressible    = false;
        child_ctx.self_delimited = false;
        child_ctx.right_side     = false;
        child_ctx.atomic         = false;
        node_to_string(z, s_str, lang, subject, max_objects, variables, resolved, child_history, &child_ctx);
        children_ok            = children_ok && child_ctx.expressible;
        subject_atomic         = child_ctx.atomic;
        const bool s_delimited = child_ctx.self_delimited;

        bool needs_parens = false;
        if (!scheme_render && !s_delimited
            && !s_str.empty()
            && s_str.find(' ') != std::string::npos
            && s_str.front() != '('
            && s_str.front() != '<'
            && s_str.front() != '{')
        {
            network::Node eff_subj = resolve_var(subject);
            std::string   raw_name = z->get_formatted_name(eff_subj, lang);
            // Compare the formatted string with the MARKED raw name
            if (s_str != mark_leaf(eff_subj, raw_name))
            {
                needs_parens = true;
            }
        }

        if (needs_parens)
            subject_name = "(" + s_str + ")";
        else
            subject_name = s_str.empty() ? (is_condition ? "" : string::mark_identifier("?")) : s_str;

        network::Node relation = fact_predicate();
        // Recursion for Relation (usually just get name, but handle complex relations)
        // Here we can assume relations are mostly named or simple, preventing deep noise
        relation = resolve_var(relation);

        std::string raw_rel_name = z->get_formatted_name(relation, lang);
        if (!raw_rel_name.empty())
        {
            // Relation has a name -> mark it manually, as we didn't recurse
            relation_name = mark_leaf(relation, raw_rel_name);
        }
        else
        {
            // Recurse -> returns marked string
            std::string r_str;
            node_to_string(z, r_str, lang, relation, max_objects, variables, resolved, child_history);
            relation_name = r_str.empty() ? string::mark_identifier("?") : r_str;

            // Wrap complex unnamed relations in parens too.
            // For consistency with subject/object, we usually assume relations are simple,
            // but if r_str has spaces and isn't a container, wrap it.
            if (relation_name.find(' ') != std::string::npos
                && relation_name.front() != '('
                && relation_name.front() != '<'
                && relation_name.front() != '{')
                relation_name = "(" + relation_name + ")";
        }
    }

    std::string objects_name;

    if (self_fact_sugar)
    {
        // The object equals the subject; the sugar form renders it only once.
    }
    else if (objects.size() > max_objects)
    {
        // Not a name but an elision, so it stays unmarked: a reader of the
        // rendering must not mistake it for a node it could look up.
        objects_name = "(... " + std::to_string(objects.size()) + " objects ...)";
    }
    else
    {
        child_ctx.right_side = true;
        for (network::Node object : objects)
        {
            std::string o_str;
            child_ctx.expressible    = false;
            child_ctx.self_delimited = false;
            child_ctx.atomic         = false;
            node_to_string(z, o_str, lang, object, max_objects, variables, resolved, child_history, &child_ctx);
            children_ok            = children_ok && child_ctx.expressible;
            const bool o_delimited = child_ctx.self_delimited;

            // Wrap object only if it's a composite fact, not a named atom
            if (!scheme_render && !o_delimited
                && !o_str.empty()
                && o_str.find(' ') != std::string::npos
                && o_str.front() != '('
                && o_str.front() != '<'
                && o_str.front() != '{')
            {
                network::Node eff_obj  = resolve_var(object);
                std::string   raw_name = z->get_formatted_name(eff_obj, lang);
                // Compare formatted string with MARKED raw name
                if (o_str != mark_leaf(eff_obj, raw_name))
                {
                    o_str = "(" + o_str + ")";
                }
            }

            if (o_str.empty()) o_str = string::mark_identifier("?");

            if (!objects_name.empty()) objects_name += " ";
            objects_name += o_str;
        }
        if (objects_name.empty()) objects_name = string::mark_identifier("?");
    }

    // An application's head must be a bare name: "f(u)" is readable, a
    // composite head is not. Falling back is the honest outcome -- the term
    // simply is not writable in this scheme.
    if (application && !subject_atomic) children_ok = false;

    // The components (subject_name, relation_name, objects_name) are already marked.
    if (self_fact_sugar)
        // The colon is the SUGAR, the predicate is a NAME -- marking them
        // as one leaf made the two indistinguishable downstream. The
        // derivation export had to split the colon off again by hand to
        // keep the predicate a node reference, and the quoting rules could
        // not say anything about a name that starts with a colon, since
        // every self-fact looked like one.
        result = ":" + mark_leaf(self_fact_rel, self_fact_pred) + " " + subject_name;
    else if (application)
        result = subject_name + "(" + objects_name + ")";
    else
        result = subject_name + " " + relation_name + " " + objects_name;

    if (is_negation)
    {
        result = "¬(" + result + ")";
    }
    else if (scheme_render && !children_ok)
    {
        // Something in the subtree is not writable in this scheme. Render
        // the node exactly as an unregistered engine would; children that
        // open a scheme region of their own are unaffected (no_scheme is a
        // one-shot flag for this node).
        SchemeContext plain;
        plain.tables    = ctx->tables;
        plain.no_scheme = true;
        node_to_string(z, result, lang, resolved, max_objects, variables, parent, history, &plain);
        return;
    }
    else if (scheme_render && in_scheme)
    {
        if (application)
        {
            // "f(u)" is self-delimiting at every precedence, and the default
            // renderer would have printed parentheses -- a deviation by
            // construction.
            ctx->deviated = true;
        }
        else
        {
            const bool need = !ctx->enclosed
                           && (op->precedence < ctx->precedence
                               || (op->precedence == ctx->precedence
                                   && (ctx->assoc == 0
                                       || (ctx->assoc < 0 && ctx->right_side)
                                       || (ctx->assoc > 0 && !ctx->right_side))));
            if (need)
                result = "(" + result + ")";
            else
                ctx->deviated = true; // the default renderer would have printed them
        }

        if (child_ctx.deviated) ctx->deviated = true;
        ctx->expressible = true;
    }
    else if (scheme_render)
    {
        // Scheme boundary: seal a deviating rendering with the scheme's own
        // delimiters, so it stays readable by the scheme's parser. Without a
        // deviation the result IS the default form and keeps its rules.
        //
        // An application deviates by construction: call notation is never
        // what the default renderer produces. Its own deviation is reported
        // upwards through ctx, which has no scheme parent here -- so it must
        // be accounted for separately from the children's.
        if (child_ctx.deviated || application)
        {
            const network::DisplayScheme& sch = ctx->tables->schemes[op->scheme];
            result                            = sch.open + result + sch.close;
            ctx->self_delimited               = true;
        }
        else if (parent != 0 && resolved_is_stmt)
        {
            result = "(" + result + ")";
        }
    }
    // If this is a statement node used as a value inside another structure,
    // wrap the whole triple in parentheses to make it valid input syntax.
    else if (parent != 0 && resolved_is_stmt)
    {
        network::Node pred = fact_predicate();
        if (pred != z->core.Cons) // lists are handled earlier; don't wrap "<...>"
            result = "(" + result + ")";
    }

    string::replace_all(result, "\r\n", " --- ");
    string::replace_all(result, "\n", " --- ");
    string::trim_in_place(result);
#ifdef DEBUG_FORMAT_FACT
    z->diagnostic_stream() << indent << "[DEBUG node_to_string] EXIT result='" << result << "'" << std::endl;
#endif
}
