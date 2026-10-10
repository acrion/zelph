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
#include "network/rule_identity.hpp"
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
    }

    // The two reserved characters beyond ASCII, matched whole: testing their
    // common lead byte C2 refused `°` and `µ` as well.
    if (name.find("¬") != std::string::npos || name.find("\xC2\xA0") != std::string::npos) return false;

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

    void set_last_node(const network::Node node)
    {
        _last_node_to_string_node.store(node, std::memory_order_relaxed);
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

    // What a part prints as when nothing could be rendered for it. It is
    // not the name of a node, thus it carries no identifier marks: if
    // marked, it would be judged as the NAME "?" -- the result-query prefix
    // at the start of a line, which needs_quotes quotes -- and each absent
    // part would print as `"?"`. The "?" used in the history check is
    // formatted identically. The "??" for a node lacking any content to
    // render stays marked: needs_quotes leaves it bare, so it displays
    // exactly as it has historically, whereas .node, which compares its
    // rendering against the UNmarked "??", would cease displaying
    // "Representation: ??" for such a node.
    const std::string missing_part = "?";

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

    // Whether this call renders the node the CALLER requested, or one accessed
    // from within another node's rendering. Only the recursion can say that a
    // node stands inside a statement, and a rule's condition set is written in
    // the surface syntax "(A, B) => C" exactly there -- see as_rule_conditions
    // below.
    const bool rendered_as_top = !history;

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
        // parse_relation asks which NEIGHBOUR of the node is a declared
        // relation type, and a cons cell whose own list is used as a PREDICATE
        // somewhere has two -- `cons`, and the list head, which being a
        // predicate declared. It then reports the ambiguity as "no relation",
        // and the very same node printed `<b>` in a graph where no list
        // happens to be a predicate and `b cons nil` in one where some list
        // is. The exact structure settles it, but only where it has to: on the
        // ambiguity, and only for a node that could be a cell at all. Asking
        // it FIRST costs 2 % of the Jacobian import, because every atom the
        // renderer passes then pays for a lookup that parse_relation answers
        // by failing fast.
        network::Node rel_type = z->parse_relation(resolved);
        if (rel_type == 0 && network::Zelph::is_hash(resolved))
            rel_type = network::get_preferred_structure(z, resolved, 3).predicate;

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

                // The EXACT decomposition, not the adjacency reading. A cell's
                // object set as parse_fact returns it is polluted by every
                // fact that merely USES the cell, and a list in PREDICATE
                // position collects two of those: the fact it is the predicate
                // of, and the `~ ->` declaration that being a predicate
                // creates. The walk then left the chain at one of them instead
                // of at nil, the list was reported improper, and `x <a b> y`
                // printed as `x (a cons b cons nil) y` -- which the parser
                // rejects, so the printed line did not read back as input.
                const network::FactStructure cell = network::get_preferred_structure(z, current, 3);
                if (cell.predicate != z->core.Cons || cell.subject == 0) break; // not a cons cell

                network::Node car = resolve_var(cell.subject);
                if (car != 0)
                    list_elements.push_back(car);

                // Get cdr (rest of list) — the single object of this cons cell
                network::Node cdr = z->core.Nil;
                for (network::Node o : cell.objects)
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
                if (tail_str.empty()) tail_str = missing_part;

                const std::string cons_name = string::mark_identifier(z->get_formatted_name(z->core.Cons, lang));

                for (auto it = list_elements.rbegin(); it != list_elements.rend(); ++it)
                {
                    std::string car_str;
                    node_to_string(z, car_str, lang, *it, max_objects, variables, resolved, child_history);
                    if (car_str.empty()) car_str = missing_part;

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
                            && elem_str.front() != '{'
                            && !elem_str.starts_with("@{"))
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
    std::unordered_set<network::Node> variable_elements;

    if (z->exists(resolved))
    {
        for (network::Node rel : z->get_right(resolved))
        {
            // The membership fact being rendered is NOT skipped. It is the one
            // that establishes the very element the reader is looking at, so
            // leaving it out printed `b in {a}` for the fact `b in {a b}` --
            // and a set constant IS its members, so `{a}` is a different node.
            // Recursion is not a concern here: an element is rendered with
            // `resolved` as its parent and inside `child_history`, and a member
            // never reaches back into the container through this branch.
            network::Node p = z->parse_relation(rel);
            if (p == z->core.PartOf)
            {
                network::adjacency_set objs;
                network::Node          s = z->parse_fact(rel, objs, 0);
                if (s != 0) s = resolve_var(s);

                // An UNBOUND variable is not an element of the container:
                // it is there because a rule pattern named it, since
                // `(X in @{...})` makes X a member by construction. Printing
                // it made a DERIVED fact read `a in @{a c Y X}`, with the
                // rule's own template variables among the elements.
                // resolve_var above has already substituted every variable
                // that HAS a binding.
                //
                // Set apart instead of discarded: within the rule's text, a
                // rule's own collection prints them with its other members,
                // and a collection lacking a ground member that no firing
                // built prints them alone -- the `@{Y}` in
                // `(X p Y) => (X likes @{Y})` is exactly what that rule says.
                //
                // A ground member of a collection a firing built (a value
                // recipe, see below) whose membership is a pattern of a
                // rule's text, and that nothing has derived, is no member of
                // the data either: the `m` in a rule that names the
                // collection within `(m in C) noted X`. This same principle
                // applies when the membership is rendered: a value recipe is
                // the same node regardless of its content, in contrast to a
                // set constant.
                if (s != 0 && objs.count(resolved) > 0)
                {
                    if (network::Zelph::is_var(s))
                        variable_elements.insert(s);
                    else if (!network::Zelph::is_value_recipe(resolved) || !z->is_rule_pattern(rel))
                        elements.insert(s);
                }
            }
        }
    }

    // A rule's own collection -- a template, as indicated by its id -- holds
    // what the rule's text has written into it: the members its literal
    // lists, whether ground or variable, and the subject of each membership
    // the rule asserts within it, the Y in `(X p Y) => (Y in @{X})`. A firing
    // writes the collection's term, never the collection itself. Within the
    // rule's text, those members constitute the statement, and the collection
    // prints all of them: `(X p Y) => (X likes @{Y k})` prints as `@{k Y}`
    // and re-enters as the identical rule, where the variables alone, `@{Y}`,
    // re-entered as another. A statement that binds the collection via rule
    // structure, as `(G => (S in C)) => (zz in C)` does, writes into the
    // collection's data term (instantiate_fact), while the rule's text
    // remains unchanged as originally written. Only a rule authored within
    // the template scope can still write a member to another rule's
    // collection -- a Janet program that holds the node and writes `X in`
    // into it while constructing a rule -- and then the other rule
    // subsequently prints that member too: `(a p b) => (c in @{d c X})`.
    //
    // Within the rule's text refers to a `=>` fact among the nodes this
    // rendering descended from, or a parent that is a pattern itself: a
    // condition or consequence rendered independently. The parent is queried
    // SYNTACTICALLY -- based on its subject, predicate, and objects -- rather
    // than through its closure, which reaches variables via any collection
    // named by a rule. A consequence lacking any variable in any slot, such
    // as `(P q R) => (a in @{R})`, thus fails to constitute a pattern when
    // rendered in isolation: it prints like data, as `a in @{a}`, which
    // mirrors the output of the fact derived by its firings. Within the rule,
    // it is `a in @{a R}`.
    //
    // Only evaluated for a collection possessing a variable
    // member.
    const auto parent_is_pattern = [&]
    {
        if (parent == 0) return false;
        const network::FactStructure pfs = network::get_preferred_structure(z, parent, 1);

        const auto unbound_var = [&](const network::Node nd)
        { return nd != 0 && network::Zelph::is_var(resolve_var(nd)); };

        if (unbound_var(pfs.subject) || unbound_var(pfs.predicate)) return true;
        return std::any_of(pfs.objects.begin(), pfs.objects.end(), unbound_var);
    };

    const auto in_rule_text = [&]
    {
        for (const network::Node h : *history)
            if (network::Zelph::is_hash(h) && z->predicate_of(h) == z->core.Causes) return true;
        return parent_is_pattern();
    };

    // Only asked of a node that contains members: each unnamed node that
    // has been rendered, each fact included, arrives at this point.
    const bool is_conjunction = (!elements.empty() || !variable_elements.empty())
                             && z->check_fact(resolved, z->core.IsA, {z->core.Conjunction}).is_known();

    // A rule's conjunction set constitutes rule structure, and within a
    // pattern it prints its variable members alone. A rule over rules that
    // binds the set and asserts membership within it, such as
    // `(G => H) => ((X s y) => (X in G))`, names the data term of the set,
    // and a firing also writes into that term (via instantiate_fact,
    // container_plan), so the set keeps the conditions with which it was
    // written. Only a rule authored within the template scope can still write
    // a variable member into another rule's set -- a Janet program that holds
    // the set's node and writes `X in` it while constructing a rule -- and
    // that rule subsequently prints the set as `(X in {X})`, where its ground
    // members would put the other rule's conditions into its text. As the
    // subject of a rule, the set is not an object of any pattern, and prints
    // the conditions. A conjunction set that is a set constant -- what the
    // explicit form `(*{(a p b) (c q d)} ~ conjunction)` builds from ground
    // conditions, and what an older engine wrote as the conditions of a rule
    // it constructed -- prints its ground members everywhere, just as every
    // set constant does (below).
    //
    // Every other collection is data, regardless of position, including a
    // rule's text, and prints its ground members, displaying variables
    // only if it contains none -- except for a collection a firing built,
    // which prints its data (see below). A data collection D that a rule
    // reads and writes, as in `((X p Y), (X in D)) => (Y in D)`, holds the
    // rule's X and Y beside its data, and these are no members of the
    // data: printing them made the rule name a collection of two
    // variables, `@{Y X}`, where it names D. A SET CONSTANT is read
    // identically: its identity equals its members, and `(X in {a b})` is
    // the exact way a rule quantifies over them -- the X that the
    // condition adds to the set is the artefact there, not the a and b.
    //
    // A collection a firing built (a value recipe) constitutes data in any
    // context, and prints all contents it holds: a variable member is one
    // that the instantiation left unbound -- `b hates @{k Z}` -- and
    // omitting it printed a collection that holds less than it does. The
    // sole exception occurs when a variable is written into the collection
    // by a rule's statement, as a rule that binds the collection does via
    // `(X p Y) => (X in C)` or, nested within its consequence, through
    // `(X p Y) => ((X in C) noted yes)`: in such cases, that X is the other
    // rule's, not a member of this data, and is excluded even when the
    // collection contains no other elements. An empty term prints as `@{}`
    // (as shown below), in that rule as well: printed as `@{X}`, the rule
    // would name a collection holding its X. Asked solely for a variable
    // member of such a collection.
    if (network::Zelph::is_value_recipe(resolved))
    {
        if (!variable_elements.empty())
        {
            for (const network::Node rel : z->get_right(resolved))
            {
                if (z->predicate_of(rel) != z->core.PartOf) continue;
                network::adjacency_set objs;
                network::Node          s = z->parse_fact(rel, objs, 0);
                if (s != 0) s = resolve_var(s);
                if (s != 0 && objs.count(resolved) > 0 && variable_elements.count(s) != 0 && !network::is_rule_statement(z, rel))
                    elements.insert(s);
            }
        }
    }
    else if (!variable_elements.empty())
    {
        if (elements.empty())
            elements = variable_elements;
        else if (is_conjunction)
        {
            if (parent_is_pattern() && !z->is_set_constant(resolved)) elements = variable_elements;
        }
        else if (z->is_rule_template(resolved) && in_rule_text())
            elements.insert(variable_elements.begin(), variable_elements.end());
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

        // A rule's condition set is printed in the SURFACE SYNTAX the parser
        // accepts -- "(A, B) => C" -- whenever it is rendered as that rule's
        // subject. The brace form is the topology, and re-entering it built
        // something else: "{A B}" reads as a set literal, a literal carrying
        // variables builds a COLLECTION, and a collection is not tagged
        // `~ conjunction`, so the rule came back inert. That is the round
        // trip failing for the commonest rule shape there is.
        //
        // Only as a rule's subject: elsewhere the node is a container being
        // dumped (`.node`) or the premise set of a justification, and a comma
        // list would claim a statement that is not being made.
        //
        // "Inside the rule" is what `rendered_as_top` decides, and the parent
        // alone cannot: a justification is printed by handing the renderer the
        // condition set with the rule that fired as context, which looks from
        // here exactly like the set being reached from inside that rule. The
        // two evaluation strategies then disagreed on one line -- the same
        // deduction printed "<= {(a p b) (b p c)}" when the classic pass found
        // it and "<= ((a p b), (b p c))" when a seeded iteration did, because
        // only the seeded path keeps the rule node as the parent.
        const bool as_rule_conditions = is_conjunction && !rendered_as_top && parent != 0
                                     && z->parse_relation(parent) == z->core.Causes;

        // A COLLECTION prints with its own marker, because that is what it
        // is: a container with an identity, which `{...}` re-entered would
        // not rebuild. A rule's conjunction set keeps the bare brace -- it
        // is rule structure rather than a value, and the rule renders around
        // it.
        const bool bare_brace = z->is_set_constant(resolved) || is_conjunction;

        result     = as_rule_conditions ? "(" : (bare_brace ? "{" : "@{");
        bool first = true;

        // A rendering that already carries a scheme's own delimiters is
        // self-delimiting; wrapping it again would produce "($( ... ))".
        SchemeContext elem_ctx;
        elem_ctx.tables = ctx->tables;

        for (network::Node e : sorted_elements)
        {
            if (!first) result += as_rule_conditions ? ", " : " ";
            std::string elem_str;
            elem_ctx.self_delimited = false;
            node_to_string(z, elem_str, lang, e, max_objects, variables, resolved, child_history, &elem_ctx);

            // The comma already separates the conditions, so a member keeps
            // the bare S-P-O shape the surface syntax is written in. Inside
            // braces the space is the separator and a member carrying one
            // has to be bracketed.
            if (!as_rule_conditions
                && !elem_ctx.self_delimited
                && !elem_str.empty()
                && elem_str.find(' ') != std::string::npos
                && elem_str.front() != '('
                && elem_str.front() != '<'
                && elem_str.front() != '{'
                && !elem_str.starts_with("@{"))
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
        result += as_rule_conditions ? ")" : "}";

        return;
    }

    // A term into which nothing has yet been derived holds no member:
    // the data term of a rule's own collection that a statement binding
    // the collection names before that rule has fired. It is an empty
    // collection, not a node lacking a reading, which "??" would claim.
    if (network::Zelph::is_value_recipe(resolved) && z->exists(resolved))
    {
        result = "@{}";
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

    // A REFUTED fact is the other way round from a rule-pattern marking, and
    // the distinction is the one CLAUDE.md draws under "What the mathematics
    // audience will not forgive": that marking is a statement ABOUT a term and
    // belongs beside it, but a refutation is the claim the graph actually
    // holds -- the fact does not hold -- and `¬` is syntactically part of the
    // line that said so. Printing `a p b` for it broke the round trip in the
    // direction that matters most: what came back re-entered as the opposite
    // of what was typed.
    const bool refuted = z->is_refuted_fact(resolved);

    if (z->exists(resolved))
    {
        // Exactly the condition under which the structural path below can
        // produce a rendering (it falls back on parse_relation too).
        const bool has_own_structure = z->parse_relation(resolved) != 0;

        // The rule-pattern marking is a fact ABOUT the node, not a concept the
        // node is an instance of, so it must never be substituted FOR the
        // node: `(a p b) => (c q d)` printed as `(a p b) => ("rule pattern")`
        // once the consequence lost its own reading -- because the rule was
        // used as a predicate (two declared relation types among the node's
        // neighbours, so parse_relation gives up), or because a predicate
        // slice kept the marking while dropping the triple. Same ruling as
        // for the negation tag next to it, and the same reason: `.node` and
        // `.explain` report the marking BESIDE the term, never inside it.
        // The lookup is paid only for a node that carries the marking, and
        // is_rule_pattern is one atomic load in a graph that has none.
        const network::Node rule_pattern_concept = z->is_rule_pattern(resolved) ? z->rule_pattern_predicate(false) : 0;

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

                        if (rule_pattern_concept != 0 && concept_node == rule_pattern_concept)
                            continue; // metadata as well -- see above

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
    //
    // Whether the subject and object are identical is determined by
    // examining the nodes they DENOTE. A premise, a `!` line, or a query
    // answer is the pattern that was matched, rendered according to its
    // bindings, and `(T p (X + Y))` where T is bound to (a + b) is the
    // self-fact (a + b) p (a + b), even though the two pattern nodes differ.
    // Following the variable bindings (resolve_var) substitutes a side that
    // is a bare variable with its value, yet preserves a compound side like
    // (X + Y) as the pattern node it is. By itself, this recognized
    // `(T p U)` when both T and U are bound to (a + b), but did not
    // recognize `(T p (X + Y))` or `((X + Y) p T)`, so the same fact printed
    // in verbose form or as sugar depending on which rule had used it.
    // Without bindings -- a rule, a plain fact, .list-rules --
    // resolve_pattern_node returns the pattern itself, and the comparison
    // remains the one it has always been. A pattern whose instance is absent
    // (a negated condition that held) denotes nothing and keeps the verbose
    // form.
    const auto denotes_self_fact = [&]
    {
        if (subject == 0 || objects.size() != 1) return false;
        const network::Node s_eff = resolve_var(subject);
        const network::Node o_eff = resolve_var(*objects.begin());
        if (o_eff == s_eff) return true;
        return network::resolve_pattern_node(z, o_eff, variables) == network::resolve_pattern_node(z, s_eff, variables);
    };

    bool          self_fact_sugar = false;
    std::string   self_fact_pred;
    network::Node self_fact_rel = 0;
    if (denotes_self_fact())
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
            && s_str.front() != '{'
            && !s_str.starts_with("@{")) // the collection literal delimits itself too
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
            subject_name = s_str.empty() ? (is_condition ? "" : missing_part) : s_str;

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
            relation_name = r_str.empty() ? missing_part : r_str;

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
                && o_str.front() != '{'
                && !o_str.starts_with("@{")) // the collection literal delimits itself too
            {
                network::Node eff_obj  = resolve_var(object);
                std::string   raw_name = z->get_formatted_name(eff_obj, lang);
                // Compare formatted string with MARKED raw name
                if (o_str != mark_leaf(eff_obj, raw_name))
                {
                    o_str = "(" + o_str + ")";
                }
            }

            if (o_str.empty()) o_str = missing_part;

            if (!objects_name.empty()) objects_name += " ";
            objects_name += o_str;
        }
        if (objects_name.empty()) objects_name = missing_part;
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

    // The "¬" belongs to the RULE that wrote it, not to the node. The tag is
    // a fact ABOUT the pattern and a ground pattern is hash-consed, so a
    // pattern negated in ONE rule carries the tag everywhere -- and writing
    // it into the term rendered an ASSERTED fact as its own negation:
    // ".explain (a p b)" answered "¬(a p b) [axiom]" for a fact that holds,
    // which is neither true nor re-enterable (a top-level "¬" is not the
    // same statement, and what it would mean is undecided).
    //
    // So it is written where it is syntactically part of the statement: as a
    // rule's condition -- the parent is then the rule -- or as a member of a
    // rule's conjunction set. Everywhere else the tag is reported BESIDE the
    // term; see the property line of `.node` and the axiom label of
    // `.explain`.
    //
    // The rule serves as the parent to its conclusion as well, meaning that
    // merely being positioned beneath a rule is not enough: the node must
    // specifically be the rule's condition part. A rule that derives the
    // exact pattern one of its conditions negates, (:p A, ¬(:q A)) => (:q A),
    // printed its conclusion as (¬(:q A)), and the line returned an error --
    // "¬" means nothing when used as a conclusion.
    const auto is_condition_of_rule = [&]
    {
        if (z->parse_relation(parent) != z->core.Causes) return false;
        network::adjacency_set conclusions;
        return z->parse_fact(parent, conclusions) == resolved;
    };
    if ((refuted && parent == 0)
        || (is_negation && parent != 0
            && (is_condition_of_rule()
                || z->check_fact(parent, z->core.IsA, {z->core.Conjunction}).is_known())))
    {
        // A refutation is the claim this node stands for rather than a tag on
        // somebody else's pattern, so it needs no test for WHICH parent -- but
        // it needs the same test for whether there is one. `¬` is a statement
        // spelling and reads as one only where the node IS the statement: on
        // an answer line, in a `.explain` step, in the echo of the line that
        // wrote it. Inside a composed term it is a different operator -- the
        // negation-as-failure prefix, which a plain statement refuses -- so
        // the marking fact printed itself as "(¬(a p b)) ~ refuted", and that
        // line came back carrying a negation tag nobody had written. It prints
        // "(a p b) ~ refuted" now, where the predicate already says it.
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
