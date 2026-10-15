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
#include "script/syntax_errors.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"

#include <janet.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zelph
{
    // True if a PEG-AST node is the literal atom `text`. Used to recognise `=>`
    // in predicate position, i.e. that a statement defines a rule.
    static bool is_atom(Janet node, const char* text)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) != "atom") return false;

        if (janet_checktype(data[1], JANET_STRING))
            return std::string(reinterpret_cast<const char*>(janet_unwrap_string(data[1]))) == text;
        if (janet_checktype(data[1], JANET_BUFFER))
        {
            const JanetBuffer* b = janet_unwrap_buffer(data[1]);
            return std::string(reinterpret_cast<const char*>(b->data), b->count) == text;
        }
        return false;
    }

    // Whether a PEG-AST argument constitutes a single token identifying a
    // node -- an atom or a variable, whether focused or not -- instead of
    // a term that builds one. A Janet value counts as a term: the outcome
    // of an island is delivered as such, and whatever the island produced
    // is part of the term.
    static bool is_plain_token(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 2 || !janet_checktype(data[0], JANET_KEYWORD)) return false;
        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type == "focused") return is_plain_token(data[1]);
        return type == "atom" || type == "var";
    }

    // Returns true when a PEG-AST node refers to the node `text`, whether
    // it appears in bare form or enclosed in quotes: `"~"` builds the
    // node `~`. is_atom identifies an operator only as it is typed; this
    // function determines which node the atom represents.
    static bool is_name(Janet node, const char* text)
    {
        if (is_atom(node, text)) return true;

        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) != "atom") return false;

        std::string token;
        if (janet_checktype(data[1], JANET_STRING))
            token = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));
        else if (janet_checktype(data[1], JANET_BUFFER))
        {
            const JanetBuffer* b = janet_unwrap_buffer(data[1]);
            token.assign(reinterpret_cast<const char*>(b->data), b->count);
        }
        return token.size() >= 2 && token.front() == '"' && token.back() == '"'
            && string::unescape_atom(token.substr(1, token.size() - 2)) == text;
    }

    // True if a PEG-AST node is a `¬` pattern, through any number of plain
    // groupings -- "¬(F)" and "(¬(F))" are the same statement.
    //
    // The question has to be asked of the SYNTAX, not of the resulting node: the
    // negated condition is the fact `((F) ~ negation)`, a hash-consed node
    // just like any other fact, so the one node exists for every statement
    // that wrote it. Only the AST knows which statement wrote the "¬".
    static bool is_negation_ast(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type == "negation") return true;
        if (type == "nested" && len == 2) return is_negation_ast(data[1]);
        return false;
    }

    // Whether a rule anywhere within this argument holds `*(P) ~ negation`: a
    // fact `~ negation` whose subject carries the focus. The focus makes the
    // fact evaluate to its subject, rendering the condition the pattern P
    // itself -- positive in nature -- and the fact about it is never read,
    // since a negated condition IS the fact `((P) ~ negation)`
    // (Zelph::negation_operand). It reads like a negation yet is not one,
    // thus it is refused wherever it would land in a rule: within a
    // condition at any depth, within a consequence, within a rule a
    // statement mentions, or within a rule a rule generates. Asked of the
    // SYNTAX because the node it builds is an ordinary positive pattern.
    static bool focused_negation(Janet node, const bool in_rule)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        const bool        slot = type == "nested" || type == "condition";

        if (in_rule && slot && len == 4 && is_name(data[2], "~") && is_name(data[3], "negation"))
        {
            const Janet* subject;
            int32_t      subject_len;
            if (janet_indexed_view(data[1], &subject, &subject_len) && subject_len >= 1
                && janet_checktype(subject[0], JANET_KEYWORD)
                && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(subject[0]))) == "focused")
                return true;
        }

        const bool rule = in_rule || (slot && len > 3 && is_name(data[2], "=>"));
        for (int32_t i = 1; i < len; ++i)
            if (focused_negation(data[i], rule)) return true;
        return false;
    }

    // The same question for the neural condition. `≈` is written as a prefix on a
    // value, so the AST names it directly.
    static bool is_approx_ast(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type == "approx") return true;
        if (type == "nested" && len == 2) return is_approx_ast(data[1]);
        return false;
    }

    // The text of a token, which the AST stores as either a string
    // or a buffer.
    static std::string token_text(Janet token)
    {
        if (janet_checktype(token, JANET_STRING)) return reinterpret_cast<const char*>(janet_unwrap_string(token));
        if (janet_checktype(token, JANET_BUFFER))
        {
            const JanetBuffer* b = janet_unwrap_buffer(token);
            return {reinterpret_cast<const char*>(b->data), static_cast<std::size_t>(b->count)};
        }
        return {};
    }

    // Whether a PEG-AST node names a variable anywhere, inside the rules it
    // holds included.
    static bool mentions_variable(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type == "var") return true;
        if (type == "selffact" && len >= 2 && string::is_var(token_text(data[1]))) return true;
        for (int32_t i = 1; i < len; ++i)
            if (mentions_variable(data[i])) return true;
        return false;
    }

    // The parser's twin for asks_implications (janet_api_facts.cpp), which
    // reads the syntax: a `=>` that a statement holds asks which
    // implications exist when its condition is a bare variable, or when it
    // holds a variable and none of its consequences can be asserted, each
    // being a variable, a name different from `!`, a set, or a collection.
    static bool asks_implications_ast(const Janet* const data, const int32_t len)
    {
        const auto peel = [](Janet value)
        {
            const Janet* part;
            int32_t      part_len;
            while (janet_indexed_view(value, &part, &part_len) && part_len == 2 && janet_checktype(part[0], JANET_KEYWORD))
            {
                const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(part[0]));
                if (type != "focused" && type != "nested") break;
                value = part[1];
            }
            return value;
        };
        const auto type_of = [](const Janet value) -> std::string
        {
            const Janet* part;
            int32_t      part_len;
            if (!janet_indexed_view(value, &part, &part_len) || part_len < 1 || !janet_checktype(part[0], JANET_KEYWORD)) return {};
            return reinterpret_cast<const char*>(janet_unwrap_keyword(part[0]));
        };

        if (type_of(peel(data[1])) == "var") return true;
        for (int32_t i = 3; i < len; ++i)
        {
            const Janet       consequence = peel(data[i]);
            const std::string type        = type_of(consequence);
            if (type == "var" || type == "set" || type == "collection" || (type == "atom" && !is_atom(consequence, "!"))) continue;
            return false;
        }
        for (int32_t i = 1; i < len; ++i)
            if (i != 2 && mentions_variable(data[i])) return true;
        return false;
    }

    // Whether a statement ASKS: it names a variable outside every rule it
    // mentions, or it is a self-fact whose predicate is a variable (`:R
    // x`). Variables referenced within a mentioned rule belong to that
    // rule, hence "x documents ((X p Y) => (X q Y))" asks for nothing.
    // Determined by the syntax, before any construction: a statement that
    // asks and one that asserts read a condition operator in distinct ways,
    // and the operand is built by the first call that runs.
    //
    // A `=>` it holds that asks which implications exist makes the
    // statement ask too (asks_implications_ast): "x says (S => O)" was
    // answered as a query, yet its code corresponded to that of a
    // claim, and every asking left the fact and its variables behind.
    static bool names_query_variable(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type == "var") return true;
        if (type == "selffact" && len >= 2 && string::is_var(token_text(data[1]))) return true;

        const bool slot = type == "nested" || type == "condition";
        if (slot && len > 3 && is_atom(data[2], "=>")) return asks_implications_ast(data, len);

        for (int32_t i = 1; i < len; ++i)
            if (names_query_variable(data[i])) return true;
        return false;
    }

    // Whether a PEG-AST node writes a statement: a fact, a comma group, a
    // self-fact, or a condition operator applied to one, possibly via a
    // focus or a grouping. Only a statement may serve as the operand for a
    // condition operator (Zelph::negation_operand), thus `foo ~ negation`
    // and `X ~ negation` stay the ordinary facts they are.
    static bool is_statement_ast(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type == "nested") return len >= 4 || (len == 2 && is_statement_ast(data[1]));
        if (type == "focused") return len >= 2 && is_statement_ast(data[1]);
        return type == "conjunction" || type == "selffact" || type == "negation" || type == "approx";
    }

    // The value represented by a group with a single value, or by a focus on
    // an entire value, regardless of how many such elements are present:
    // `((a p b))` and `*(*(a p b))` both reduce to `(a p b)`
    // (transform_arg). `focus` is set when a focus was included among them.
    static Janet unwrapped(Janet value, bool& focus)
    {
        const Janet* data;
        int32_t      len;
        while (janet_indexed_view(value, &data, &len) && len == 2 && janet_checktype(data[0], JANET_KEYWORD))
        {
            const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
            if (type == "focused")
                focus = true;
            else if (type != "nested")
                break;
            value = data[1];
        }
        return value;
    }

    // Whether a claimed statement builds a part before the fact it states:
    // an argument that is not an atom or a variable -- specifically, a
    // nested statement, a set, a collection, a list, a number -- or a comma
    // list, whose members are asserted one after the other. The fact()
    // function might still refuse the statement after that part has been
    // written: `x says ((a p b) r (a p b) (c d e))` kept `a p b` and
    // `c d e`, which answered queries as data afterwards.
    static bool builds_parts(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) == "conjunction") return true;

        for (int32_t i = 1; i < len; ++i)
        {
            const Janet* part;
            int32_t      part_len;
            if (!janet_indexed_view(data[i], &part, &part_len) || part_len < 1 || !janet_checktype(part[0], JANET_KEYWORD)) continue;
            const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(part[0]));
            if (type != "atom" && type != "var") return true;
        }
        return false;
    }

    // The fact F associated with a line `(F) ~ refuted` or `*(F) ~ refuted`,
    // based on its subject: a statement enclosed in parentheses, with no
    // focus among its own parts -- `(a *p b) ~ refuted` marks `p`. `data`
    // and `len` receive F's AST node.
    //
    // A group around F or around its focus is read through, and likewise
    // a focus encompassing an entire value: "((a p b)) ~ refuted",
    // "(*(a p b)) ~ refuted", and "*(*(a p b)) ~ refuted" were constructed
    // via zelph/fact and refuted a fact the graph claimed.
    static bool refutation_operand(Janet subject, const Janet*& data, int32_t& len, bool& focus)
    {
        focus = false;
        if (!janet_indexed_view(unwrapped(subject, focus), &data, &len) || len <= 3 || !janet_checktype(data[0], JANET_KEYWORD)
            || std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) != "nested")
            return false;
        for (int32_t i = 1; i < len; ++i)
        {
            const Janet* part;
            int32_t      part_len;
            if (janet_indexed_view(data[i], &part, &part_len) && part_len >= 1 && janet_checktype(part[0], JANET_KEYWORD)
                && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(part[0]))) == "focused")
                return false;
        }
        return true;
    }

    // The fact F associated with a line `(S P *(F)) ~ refuted`: the subject
    // is a statement whose focus marks one of its parts, the statement F,
    // which the subject evaluates to; the line refutes F and writes `S P F`
    // beside it, mirroring how the focus operates in any context. The
    // focused part can hand on F via its own focus, as in
    // `(x says *(y says *(a p b))) ~ refuted`. `levels` receives the parts
    // of each statement en route to F, starting with the outermost, `whole`
    // indicates whether a focus marks the whole subject, and `data` and
    // `len` hold the AST node corresponding to F.
    static bool focused_refutation_operand(Janet subject, std::vector<std::vector<Janet>>& levels, bool& whole, const Janet*& data, int32_t& len)
    {
        whole = false;
        for (Janet statement = unwrapped(subject, whole);;)
        {
            const Janet* outer;
            int32_t      outer_len;
            if (!janet_indexed_view(statement, &outer, &outer_len) || outer_len <= 3 || !janet_checktype(outer[0], JANET_KEYWORD)
                || std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(outer[0]))) != "nested")
                return false;

            int focused = -1;
            for (int32_t i = 1; i < outer_len; ++i)
            {
                const Janet* part;
                int32_t      part_len;
                if (janet_indexed_view(outer[i], &part, &part_len) && part_len == 2 && janet_checktype(part[0], JANET_KEYWORD)
                    && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(part[0]))) == "focused")
                {
                    if (focused != -1) return false;
                    focused = i;
                }
            }
            if (focused == -1) return false;

            levels.emplace_back(outer + 1, outer + outer_len);
            bool focus = false;
            if (refutation_operand(outer[focused], data, len, focus)) return true;
            statement = unwrapped(outer[focused], focus);
        }
    }

    // The code of a condition operator, constructed such that its
    // operand constitutes a pattern rather than a claim
    // (janet_cfun_zelph_operand).
    static std::string operand_code(const std::string& code)
    {
        return "(zelph/operand (fn [] " + code + "))";
    }

    // The elements within `*{...} ~ conjunction`, the comma list expressed
    // in full: the focus returns the set, and this set constitutes the
    // condition list, meaning its members are conditions rather than the
    // data contained within a set literal.
    static bool explicit_conjunction(Janet node, const Janet*& members, int32_t& count)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len != 4) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type != "nested" && type != "condition" && type != "root") return false;
        if (!is_name(data[2], "~") || !is_name(data[3], "conjunction")) return false;

        const Janet* focus;
        int32_t      focus_len;
        if (!janet_indexed_view(data[1], &focus, &focus_len) || focus_len != 2) return false;
        if (!janet_checktype(focus[0], JANET_KEYWORD)) return false;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(focus[0]))) != "focused") return false;

        const Janet* set;
        int32_t      set_len;
        if (!janet_indexed_view(focus[1], &set, &set_len) || set_len < 1) return false;
        if (!janet_checktype(set[0], JANET_KEYWORD)) return false;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(set[0]))) != "set") return false;

        members = set + 1;
        count   = set_len - 1;
        return true;
    }

    // Whether a PEG-AST node qualifies as a rule statement whose focus marks
    // its condition list, `(*(A, B) => C)`, or as a rule containing such a
    // statement within its consequences, at any nesting level. When a focus or
    // group contains only one value, it is traversed, both around the
    // statement and situated between the focus and the comma list:
    // `((*(A, B) => C))` and `*(*(*(A, B)) => C)` represent identical
    // statements. The expression evaluates to the comma list, which thus
    // serves as the consequence of the rule that holds it (see
    // refuse_rule_slots).
    static bool focuses_condition_list(Janet node)
    {
        // The value lying under foci and groups of a single value, as
        // zelph-peel interprets it within the grammar.
        const auto peel = [](Janet value)
        {
            const Janet* part;
            int32_t      part_len;
            while (janet_indexed_view(value, &part, &part_len) && part_len == 2 && janet_checktype(part[0], JANET_KEYWORD))
            {
                const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(part[0]));
                if (type != "focused" && type != "nested") break;
                value = part[1];
            }
            return value;
        };

        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(peel(node), &data, &len) || len < 4 || !janet_checktype(data[0], JANET_KEYWORD)) return false;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) != "nested" || !is_atom(data[2], "=>")) return false;

        const Janet* part;
        int32_t      part_len;
        if (janet_indexed_view(data[1], &part, &part_len) && part_len == 2 && janet_checktype(part[0], JANET_KEYWORD)
            && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(part[0]))) == "focused")
        {
            const Janet* inner;
            int32_t      inner_len;
            if (janet_indexed_view(peel(part[1]), &inner, &inner_len) && inner_len >= 1 && janet_checktype(inner[0], JANET_KEYWORD)
                && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(inner[0]))) == "conjunction")
                return true;
        }

        // At a lower level, the rule written by the rule that a generator
        // writes: `(A) => ((B) => (*(C, D) => (E)))` hands the comma list to
        // the rule positioned in the centre.
        for (int32_t i = 3; i < len; ++i)
            if (focuses_condition_list(data[i])) return true;
        return false;
    }

    // The rules that a value mentions, each one positioned where it
    // appears: what resides within a rule is its own (a rule generator's
    // inner rule among them).
    static void mentioned_rules(Janet node, std::vector<Janet>& rules)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if ((type == "nested" || type == "condition") && len > 3 && is_atom(data[2], "=>"))
        {
            rules.push_back(node);
            return;
        }

        for (int32_t i = 1; i < len; ++i)
            mentioned_rules(data[i], rules);
    }

    // Whether a value consists of a focus, via groups containing a single
    // value: `*(a p b)` and `((*(a p b)))`. A focus there marks the whole
    // value, not any individual part, thus it hands on nothing that the
    // value would not be anyway.
    static bool focuses_whole_value(Janet value)
    {
        for (;;)
        {
            const Janet* part;
            int32_t      part_len;
            if (!janet_indexed_view(value, &part, &part_len) || part_len != 2 || !janet_checktype(part[0], JANET_KEYWORD)) return false;
            const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(part[0]));
            if (type == "focused") return true;
            if (type != "nested") return false;
            value = part[1];
        }
    }

    // A rule that another statement holds is mentioned whole
    // (zelph/rule-text), and a focus on one of its slots passes that part to
    // the surrounding statement: nothing names the rule at that point, hence
    // it was in force from a line that reads as a mention, and outside the
    // duplicate check -- "y says ((X p Y) => *(X q Y))" typed twice left two
    // distinct rules. The focus on the arrow names the rule, and the grammar
    // has by now interpreted it as the arrow (zelph-arrow).
    //
    // The refutation of a rule holds the rule as well (`¬`), and `refuted`
    // words the refusal for that line.
    static void refuse_focus_in_held_rule(const std::vector<Janet>& args, const bool refuted = false)
    {
        for (const Janet& arg : args)
        {
            const Janet* data;
            int32_t      len;
            if (janet_indexed_view(arg, &data, &len) && len >= 2 && janet_checktype(data[0], JANET_KEYWORD)
                && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) == "focused")
            {
                if (refuted)
                    throw std::runtime_error(
                        "\"¬\" refutes a rule as a whole, and a focus on one of its parts would hand that part to the "
                        "line, which claimed it while it refuted the rule. Refute the rule without the focus.");
                throw std::runtime_error(
                    "A rule inside another statement is mentioned, not asserted, and a focus on one of its parts "
                    "would hand that part to the statement and leave the rule on its own, where nothing mentions "
                    "it: in force, from a line that only talks about it. Mention the rule whole -- without the "
                    "focus, or with the focus on its arrow, \"(A *=> B)\", which names the rule -- or type it on "
                    "a line of its own.");
            }
        }

        // A focus on one condition of the comma list was dropped without
        // comment: "x says ((*(X R Y), Y R Z) => (X R Z))" mentioned the rule
        // as though the focus had never been present. A focus positioned
        // deeper within a condition writes a fact beside it and remains
        // permitted.
        const Janet* list;
        int32_t      list_len;
        if (args.empty() || !janet_indexed_view(args[0], &list, &list_len) || list_len < 2 || !janet_checktype(list[0], JANET_KEYWORD)
            || std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(list[0]))) != "conjunction")
            return;
        for (int32_t i = 1; i < list_len; ++i)
        {
            const Janet* member;
            int32_t      member_len;
            if (!janet_indexed_view(list[i], &member, &member_len) || member_len != 2) continue;
            if (focuses_whole_value(member[1]))
                throw std::runtime_error(
                    "Condition " + std::to_string(i) + " of the comma list carries a focus, which would make the list evaluate "
                                                       "to that condition, and in a rule that another statement holds it changes nothing: the rule is "
                                                       "mentioned whole. Write the condition without the focus.");
        }
    }

    std::string ScriptEngine::parse_zelph_to_janet(const std::string& input, const bool pattern) const
    {
        // The islands are expanded first, so what they construct exists
        // before the parser's decision about the line; this construction is
        // retained solely for a line that the parser accepts
        // (Impl::end_island_recording).
        //
        // A handler that parses a statement of its own does so inside the
        // recording associated with the line its island belongs to, and
        // leaves the decision to the parse that opened it.
        const bool  outer = !_pImpl->_island_recording;
        std::string code;
        try
        {
            code = transform_statement(_pImpl->expand_inline_keywords(input), pattern);
        }
        catch (...)
        {
            if (outer) _pImpl->end_island_recording(false);
            throw;
        }
        if (outer) _pImpl->end_island_recording(!code.empty());
        return code;
    }

    std::string ScriptEngine::transform_statement(const std::string& expanded, const bool pattern) const
    {
        JanetSymbol      match_sym = janet_csymbol("zelph-safe-parse");
        Janet            match_fun_out;
        JanetBindingType bt = janet_resolve(_pImpl->_janet_env, match_sym, &match_fun_out);

        if (bt != JANET_BINDING_DEF) return "";

        JanetFunction* match_fun = janet_unwrap_function(match_fun_out);
        Janet          args[2]   = {_pImpl->_zelph_peg, janet_cstringv(expanded.c_str())};
        Janet          result;

        if (Impl::pcall_rooted(match_fun, 2, args, &result) != JANET_SIGNAL_OK)
        {
            return "";
        }
        if (janet_checktype(result, JANET_NIL))
        {
            return "";
        }

        JanetArray* tree = janet_unwrap_array(result);
        if (tree->count < 1) return "";

        const Janet* root_data;
        int32_t      root_len;
        if (!janet_indexed_view(tree->data[0], &root_data, &root_len)) return "";

        std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(root_data[0]));

        using NestedOp = Impl::NestedOp;

        // A statement ASKS if it names a variable located outside the rules
        // it mentions (names_query_variable), or if an inline-keyword island
        // has resolved one on its behalf -- the variables originating from
        // the island belong to the statement itself (process_janet). A query
        // constitutes a condition list evaluated once, hence a condition
        // operator within it carries the same significance as in a rule's
        // condition list, and what the query constructs underneath one is a
        // pattern (zelph/query-text). A statement that asserts does not
        // possess a condition slot.
        const bool island_asks = _pImpl->_scoped_vars_preloaded && _pImpl->has_scoped_variables();

        // A command's pattern names a node and makes no assertion, hence a
        // ground path within it gets no guard, just as in a query
        // (_query_code): `.node` prints a path condition in the form "a p⁺ b"
        // and must locate it using that exact spelling.
        const Impl::BlockScope pattern_code(_pImpl->_query_code, _pImpl->_query_code || pattern);

        // The operators in a statement that ASSERTS. The sugar builds its
        // operand using zelph/fact and tags the outcome, meaning a statement
        // that denies a fact used to end up asserting it, and "x q (≈net(a p
        // b))" returned `a p b` in response to "S p O". Refused before any
        // construction.
        const auto refuse_in_assertion = [](const NestedOp op)
        {
            switch (op)
            {
            case NestedOp::Negation:
                throw std::runtime_error(
                    "\"¬\" is a condition operator and has no meaning in a statement that asserts: it succeeds "
                    "when a pattern is ABSENT, which only a condition can ask -- in a rule, or in a query (a line "
                    "with a variable). On its own line \"¬(a p b)\" says that the fact does not hold.");
            case NestedOp::Approx:
                throw std::runtime_error(
                    "\"≈\" is a condition operator and has no meaning in a statement that asserts: it asks what a "
                    "network believes, which a condition can read -- in a rule, or in a query (a line with a "
                    "variable) -- and a statement cannot claim.");
            case NestedOp::Path:
                throw std::runtime_error(
                    "\"⁺\" and \"∗\" are condition operators and have no meaning inside a statement that asserts: "
                    "reachability is what the engine WALKS. A condition reads it -- in a rule, or in a query (a "
                    "line with a variable), where \"S p⁺ b\" asks for it.");
            case NestedOp::None:
                break;
            }
        };

        // The identical trio expressed as the facts they build -- the
        // identical nodes (Zelph::negation_operand), hence the identical
        // refusal. Typed as a statement, "(a p b) ~ negation" asserted `a p
        // b` together with the condition that says it is absent.
        const auto refuse_written_out = [](const NestedOp op)
        {
            switch (op)
            {
            case NestedOp::Negation:
                throw std::runtime_error(
                    "\"((P) ~ negation)\" is the condition \"¬(P)\" written out, and a condition has no meaning in "
                    "a statement that asserts: it succeeds when P is ABSENT, which only a condition can ask -- in a "
                    "rule, or in a query (a line with a variable). On its own line \"¬(a p b)\" says that the fact "
                    "does not hold.");
            case NestedOp::Approx:
                throw std::runtime_error(
                    "\"((P) nn net)\" is the condition \"≈net(P)\" written out, and a condition has no meaning in a "
                    "statement that asserts: it asks what a network believes, which a condition can read -- in a "
                    "rule, or in a query (a line with a variable) -- and a statement cannot claim.");
            case NestedOp::Path:
                throw std::runtime_error(
                    "\"((S p O) closure one-or-more)\" is the condition \"S p⁺ O\" written out (\"zero-or-more\" "
                    "is \"S p∗ O\"), and a condition has no meaning in a statement that asserts: reachability is "
                    "what the engine WALKS. A condition reads it -- in a rule, or in a query (a line with a "
                    "variable).");
            case NestedOp::None:
                break;
            }
        };

        const auto refuse_misplaced = [](const NestedOp op, const bool in_query = false)
        {
            switch (op)
            {
            case NestedOp::Negation:
                throw std::runtime_error(
                    std::string("\"¬\" applies to a whole condition, not to something inside one: it "
                                "negates the condition it stands in front of, and nothing below a "
                                "condition reads it. Write it in front of the condition -- ")
                    + (in_query ? "\"X q b, ¬(X p b)\"." : "\"(A q B, ¬(A p B)) => ...\"."));
            case NestedOp::Approx:
                throw std::runtime_error(
                    std::string("\"≈\" applies to a whole condition, not to something inside one: it "
                                "asks what a network believes about the condition, and nothing reads "
                                "it below that. Write it as the condition -- ")
                    + (in_query ? "\"X rel S, ≈net(S p O)\"." : "\"(X rel S, ≈net(S p O)) => ...\"."));
            case NestedOp::Path:
                throw std::runtime_error(
                    "\"⁺\" and \"∗\" mark the predicate of a CONDITION, not of a fact "
                    "inside one: the closure is walked for the condition itself, and a "
                    "marker below it tags a fact nothing ever walks. Write the path as "
                    "its own condition.");
            case NestedOp::None:
                break;
            }
        };

        // An operator within a rule that a rule holds as an argument --
        // "(Z says ((X p Y, ¬(X q Y)) => (X r Y))) => (Z ok k)". A statement
        // or a query can mention that rule with its operators; a rule
        // matches it (in a condition) or writes it (in a consequence) as a
        // pattern, and the sugar is not read there. Typically the operator
        // stands at the top of a condition of the rule that is held, where
        // it belongs, thus "applies to a whole condition" named a mistake
        // the line did not make. The written-out spelling is accepted
        // there: it is the node the sugar builds, and the one a mention
        // prints.
        const auto refuse_in_held_rule = [](const NestedOp op, const bool in_condition)
        {
            if (op == NestedOp::None) return;

            const std::string role = in_condition ? "matches" : "writes";
            std::string       ops;
            std::string       mention;
            std::string       written;
            switch (op)
            {
            case NestedOp::Negation:
                ops     = "\"¬\" inside it is";
                mention = "\"x about ((X p Y, ¬(X q Y)) => (X r Y))\"";
                written = "write the condition as the fact \"¬\" builds, \"((P) ~ negation)\" for \"¬(P)\"";
                break;
            case NestedOp::Approx:
                ops     = "\"≈\" inside it is";
                mention = "\"x about ((X p Y, ≈net(X q Y)) => (X r Y))\"";
                written = "write the condition as the fact \"≈\" builds, \"((P) nn net)\" for \"≈net(P)\"";
                break;
            case NestedOp::Path:
                ops     = "\"⁺\" and \"∗\" inside it are";
                mention = "\"x about ((X p⁺ Y) => (X r Y))\"";
                written = "write the condition as the fact \"⁺\" or \"∗\" builds, \"((S p O) closure one-or-more)\" for "
                          "\"S p⁺ O\" and \"((S p O) closure zero-or-more)\" for \"S p∗ O\"";
                break;
            case NestedOp::None:
                break;
            }
            throw std::runtime_error("A rule that a rule holds as an argument is a pattern the outer rule " + role + ", and " + ops
                                     + " not supported there. A plain statement or a query may mention such a rule -- "
                                     + mention + ". Inside a rule, " + written + ", which is the spelling such a mention prints.");
        };

        // The slots of a rule, interpreted identically regardless of where
        // the rule appears: typed on a line of its own, or mentioned by
        // another statement (refuse_in_mentions).
        //
        // `¬` is a CONDITION operator. As a consequence it used to be
        // ignored outright: "(A p B) => ¬(A q B)" derived (x q y) -- the
        // exact opposite of what the rule says, without a word. What a
        // derived negation should mean is a separate question (zelph can
        // hold a fact as known-wrong, via the probability argument of
        // fact()); until it is answered, saying so beats guessing.
        const auto refuse_rule_slots = [&](const std::vector<Janet>& fact_args)
        {
            for (std::size_t i = 2; i < fact_args.size(); ++i)
            {
                if (is_negation_ast(fact_args[i]))
                    throw std::runtime_error(
                        "\"¬\" is a condition operator and has no meaning as a consequence: "
                        "a rule derives what holds, not what does not. To say that the two "
                        "may not hold together, write a contradiction rule -- "
                        "\"(A p B, A q B) => !\".");

                // The other two condition operators reached the consequence
                // slot unchecked and each failed its own way: a path
                // consequence was listed by .list-rules and derived nothing
                // at all, and both would have written their tag fact -- and
                // with it the one-step fact underneath it -- into the graph
                // as a claim nobody made.
                if (Impl::is_path_ast(fact_args[i]))
                    throw std::runtime_error(
                        "\"⁺\" and \"∗\" are condition operators and have no meaning as a "
                        "consequence: reachability is what the engine WALKS, not what a rule "
                        "asserts. Derive the one-step fact instead, and let the path condition "
                        "read it.");

                if (is_approx_ast(fact_args[i]))
                    throw std::runtime_error(
                        "\"≈\" is a condition operator and has no meaning as a consequence: "
                        "a rule cannot assert what a network believes. Consult the net in a "
                        "condition and derive an ordinary fact from it.");

                // A rule situated within a consequence that focuses its own
                // conditions hands them to the outer rule:
                // `(R is transitive) => (*(X R Y, Y R Z) => (X R Z))` writes
                // the comma list as the consequence. Zelph::fact refused it,
                // and its message advised against a comma list typed as the
                // consequence, a mistake the line had not made.
                if (focuses_condition_list(fact_args[i]))
                    throw std::runtime_error(
                        "The consequence is a rule with a focus on its condition list, and a focus makes a "
                        "statement evaluate to the part it marks: this rule's consequence would be that "
                        "condition list, a conjunction, which a rule cannot have as its consequence. A rule "
                        "that derives a rule writes the inner rule without that focus: "
                        "\"(A) => ((B, C) => (D))\".");

                // Expressed as the fact it builds, each of the three
                // refers to the identical node, and when derived it
                // asserted what that node concerns: "(A p B) => ((A q
                // B) ~ negation)" derived `a q b`, the precise fact it
                // says is absent, and one argument lower "(A p B) =>
                // (x says ((A q B) ~ negation))" performed the same
                // action.
                switch (_pImpl->written_out_condition_op(fact_args[i]))
                {
                case NestedOp::Negation:
                    throw std::runtime_error(
                        "\"((P) ~ negation)\" is the condition \"¬(P)\" written out, and has no meaning in a "
                        "consequence either: a rule derives what holds, not what does not, and deriving "
                        "this fact would claim P. To say that the two may not hold together, write a "
                        "contradiction rule -- \"(A p B, A q B) => !\".");
                case NestedOp::Path:
                    throw std::runtime_error(
                        "\"((S p O) closure one-or-more)\" is the condition \"S p⁺ O\" written out "
                        "(\"zero-or-more\" is \"S p∗ O\"), and has no meaning in a consequence either: "
                        "reachability is what the engine WALKS, and deriving this fact would claim the one "
                        "step S p O. Derive the one-step fact instead, and let the path condition read it.");
                case NestedOp::Approx:
                    throw std::runtime_error(
                        "\"((P) nn net)\" is the condition \"≈net(P)\" written out, and has no meaning in a "
                        "consequence either: a rule cannot assert what a network believes, and deriving "
                        "this fact would claim P. Consult the net in a condition and derive an ordinary "
                        "fact from it.");
                case NestedOp::None:
                    break;
                }
            }

            // The three above ask about the top of a slot, which is where
            // an operator belongs and therefore the only place anyone
            // thought to look. One argument further in it was neither read
            // nor reported: the rule was built without it and printed back
            // without it. Both slots are walked now -- a condition keeps
            // its own operator layers, everything under them and the whole
            // of a consequence keeps none. A nested "=>" opens its slots
            // again, so a rule generator's inner rule is untouched.
            for (std::size_t i = 0; i < fact_args.size(); ++i)
            {
                if (i == 1) continue; // the arrow

                // A slot of a rule is a statement, and the AST says which
                // slot this is -- so a fragment too short to be one is
                // named by its role rather than by its position in a
                // generated call. Only a `(...)` group is checked here: a
                // comma list carries its own conditions and names them
                // one by one, and a bare atom in a slot is a different
                // mistake with a message of its own.
                if (const int vals = script::nested_value_count(fact_args[i]); vals >= 0 && vals < 3)
                {
                    const Janet* slot_data;
                    int32_t      slot_len;
                    if (janet_indexed_view(fact_args[i], &slot_data, &slot_len) && slot_len >= 2)
                    {
                        std::vector<Janet> slot_args;
                        for (int32_t j = 1; j < slot_len; ++j)
                            slot_args.push_back(slot_data[j]);
                        // One value in parentheses is plain grouping --
                        // "(:isprime N)" is a whole statement -- so only
                        // two of them are short.
                        if (slot_args.size() == 2)
                            script::refuse_short_statement(i == 0 ? "the condition" : "the consequence", slot_args);
                    }
                }

                // An operator placed incorrectly within the rule's own slots
                // is reported accordingly; one that stands solely inside a
                // rule the slot holds as an argument has a reason of its own.
                refuse_misplaced(Impl::misplaced_condition_op(fact_args[i], i == 0, false, true));
                refuse_in_held_rule(Impl::misplaced_condition_op(fact_args[i], i == 0), i == 0);
            }
        };

        // A rule that a line mentions -- "x about ((X p Y, ¬(X q Y)) => (X r
        // Y))" -- features the same slots as any other rule: an operator
        // must reside at the top of one of its conditions and is rejected
        // elsewhere, just as in a typed rule. Mentioning the rule does not
        // put it in force; instead, it is recorded as data, with its ground
        // parts serving as patterns (zelph/rule-text). Previously, it was
        // outright rejected, even though the inner rule within a rule
        // generator accepted the operator, and a set holding the rule
        // accepted it anywhere, including within its consequence.
        const auto refuse_in_mentions = [&](const Janet node)
        {
            std::vector<Janet> rules;
            mentioned_rules(node, rules);
            for (const Janet r : rules)
            {
                const Janet* data;
                int32_t      len;
                janet_indexed_view(r, &data, &len);
                refuse_rule_slots(std::vector<Janet>(data + 1, data + len));
            }
        };

        // A condition set expressed using curly brackets constitutes a SET,
        // and a SET is considered data: "{(X go Y) (¬(X p b))} => (X ok Y)"
        // built a rule upon a collection that holds the negation fact,
        // printed it devoid of the "¬" symbol, and listed no rule. Refused
        // at any location it stands -- see set_literal_op.
        if (const NestedOp op = _pImpl->set_literal_op(tree->data[0]); op != NestedOp::None)
        {
            const std::string name = op == NestedOp::Negation ? "A negated condition (\"¬\")"
                                   : op == NestedOp::Approx   ? "A neural condition (\"≈\")"
                                                              : "A path condition (\"⁺\", \"∗\")";
            throw std::runtime_error(
                name + " stands inside a set or a collection literal, and a set is DATA: it holds its members, "
                       "and nothing reads a condition among them. Write conditions as a comma list -- "
                       "\"(X go Y, ¬(X p b)) => ...\" in a rule, \"X go Y, ¬(X p b)\" as a query -- or as "
                       "\"*{...} ~ conjunction\", the comma list written out.");
        }

        const auto refuse_focused_negation = []
        {
            throw std::runtime_error(
                "In a condition -- in a rule, or in a query -- \"*(P) ~ negation\" is not a negation: the focus "
                "makes it the pattern P itself, so the condition would read P positively. Write a negated "
                "condition as \"¬(P)\" or \"((P) ~ negation)\".");
        };

        // `*(P) ~ refuted`, refused in identical positions to those of the
        // negation's focus spelling, naming the spelling that conveys the
        // intended meaning.
        const auto refuse_focused_refutation = [](const bool in_rule)
        {
            throw std::runtime_error(
                std::string(in_rule ? "In a rule, \"*(P) ~ refuted\" is not a refutation: the focus makes it the pattern P "
                                      "itself, so the rule would read or derive P positively. "
                                    : "In a condition -- in a rule, or in a query -- \"*(P) ~ refuted\" is not a refutation: "
                                      "the focus makes it the pattern P itself, so the condition would read P positively. ")
                + "Write the marking in parentheses, \"((P) ~ refuted)\": as a condition it asks that P is refuted, "
                  "as a consequence it derives the refutation.");
        };

        // The code behind a query. Each query runs inside zelph/query-text,
        // which records what the statement generates: anything situated
        // beneath an operator is marked as a pattern before evaluation, and
        // anything containing the query's variables is removed after the
        // query is answered (Zelph::drop_query_text). A command's pattern
        // marks nothing: the command reverses all that its pattern built.
        const auto query_code = [pattern](const std::string& code)
        {
            return !pattern ? "(zelph/query-text (fn [] " + code + "))" : code;
        };

        // The code of a claimed statement that carries a refutation in
        // addition to what it writes runs within zelph/claim-text, so that a
        // statement that the refusal of the refutation stops keeps nothing it
        // built. This also applies to one that takes a node by reference,
        // where the claim at the line's end may be refused, and to one that
        // builds a part before its own fact (builds_parts).
        const auto claim_code = [this](const std::string& code, const bool claims, const Janet ast)
        {
            const bool scratch = claims && (_pImpl->writes_refuted_marking(ast) || Impl::takes_reference(ast) || builds_parts(ast));
            return scratch ? "(zelph/claim-text (fn [] " + code + "))" : code;
        };

        if (type == "root")
        {
            // Check how many items we have
            // root_data[0] is :root tag
            // root_data[1..n] are the values
            int val_count = root_len - 1;

            if (val_count == 0) return "";

            if (val_count == 1)
            {
                // A bare parenthesized fact like (A rel B) at the top level is a syntax error:
                // nested facts are only valid as arguments inside a larger statement, not standalone.
                const Janet* val_data;
                int32_t      val_len;
                if (janet_indexed_view(root_data[1], &val_data, &val_len) && val_len > 0
                    && janet_checktype(val_data[0], JANET_KEYWORD))
                {
                    std::string val_type = reinterpret_cast<const char*>(janet_unwrap_keyword(val_data[0]));
                    if (val_type == "nested")
                        return ""; // Syntax error: (fact) at top level is not a valid statement

                    const bool asks = names_query_variable(root_data[1]) || island_asks;

                    // A statement of fewer than three values is incomplete, and an
                    // empty result is how this function says so -- the REPL then
                    // buffers the line and waits for the rest of it. That reading
                    // is right for "a p" and wrong for "≈net(a p b)", which is not
                    // the beginning of anything: `≈` asks what a network believes,
                    // which a condition may read and a line cannot assert. The
                    // symptom was silence, and the next line being swallowed with
                    // it. Its two siblings are refused a few lines below and in
                    // zelph/path-guard; this is the same refusal. With a variable,
                    // "≈net(X p b)" constitutes a query and inquires of the net.
                    //
                    // A command's pattern asserts nothing either: it names the
                    // node the condition is, which `.node` prints as
                    // "≈net(a p b)" and must be located using that exact
                    // spelling.
                    if (val_type == "approx" && !asks && !pattern)
                        throw std::runtime_error(
                            "\"≈\" is a condition operator and has no meaning in a statement that asserts: it asks "
                            "what a network believes, which a condition can read -- in a rule, or in a query (a "
                            "line with a variable) -- and a statement cannot claim.");

                    // `¬(F)` on its own line is the one condition operator that
                    // has a reading outside a condition, and it is not the one it
                    // used to get. The sugar builds its operand with zelph/fact
                    // and tags the result, so the line ASSERTED F and then marked
                    // the very node it had just claimed as negated -- the graph
                    // said the fact holds and that it does not. It now means what
                    // it says: F is entered as known-wrong, through the
                    // probability argument Zelph::fact has always had. When a
                    // variable is involved, "¬(X p b)" does not refute anything;
                    // instead, it functions as a query, asking whether no fact
                    // matches.
                    if (val_type == "negation" && val_len >= 2 && !asks)
                    {
                        // The reading below is for a FACT, and the other two
                        // condition operators are not one. They are refused when
                        // they stand alone, and a `¬` in front used to get past
                        // every one of those refusals, because a negated line is
                        // routed through zelph/refute and the path sugar is keyed
                        // on zelph/fact. "¬(a p⁺ b)" therefore built a fact whose
                        // predicate is a node NAMED "p⁺" -- an invented predicate,
                        // marked refuted, from a line about reachability -- while
                        // "a p⁺ b" was refused as it should be. `≈` failed the
                        // other way: it reached the message below and was told it
                        // needs a fact, which it had.
                        if (Impl::is_path_ast(val_data[1]))
                            throw std::runtime_error(
                                "\"⁺\" and \"∗\" are condition operators, and a \"¬\" in front does not "
                                "change that: reachability is what the engine WALKS, so there is no "
                                "claim of it to deny. Write the path as a condition -- in a rule, or in a "
                                "query (a line with a variable) -- under \"¬\" if what you want is the "
                                "absence of a path.");

                        if (is_approx_ast(val_data[1]))
                            throw std::runtime_error(
                                "\"≈\" is a condition operator, and a \"¬\" in front does not change "
                                "that: it asks what a network believes, which a condition can read and a "
                                "statement can neither claim nor deny. Write it as a condition -- in a "
                                "rule, or in a query (a line with a variable) -- under \"¬\" for the case "
                                "the net does not confirm.");

                        const Janet* inner_data;
                        int32_t      inner_len;
                        if (janet_indexed_view(val_data[1], &inner_data, &inner_len)
                            && inner_len > 3
                            && janet_checktype(inner_data[0], JANET_KEYWORD)
                            && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(inner_data[0]))) == "nested")
                        {
                            // What is refuted is a FACT, and within it, a
                            // condition holds no significance any more than in
                            // any statement that asserts. The operand of the
                            // refutation was never walked, thus "¬(x says (¬(a
                            // p b)))" refuted `x says ...` and asserted `a p
                            // b`.
                            if (!pattern)
                            {
                                refuse_written_out(_pImpl->explicit_condition_op(inner_data + 1, inner_len - 1));
                                for (int32_t i = 1; i < inner_len; ++i)
                                {
                                    refuse_in_mentions(inner_data[i]);
                                    refuse_in_assertion(Impl::nested_condition_op(inner_data[i], false));
                                    refuse_written_out(_pImpl->written_out_condition_op(inner_data[i]));
                                }
                            }

                            std::vector<Janet> inner_args;
                            for (int32_t i = 1; i < inner_len; ++i)
                                inner_args.push_back(inner_data[i]);

                            // A refutation holds the rule it refutes, and a
                            // focus on one of the rule's parts caused the line
                            // to evaluate to that part and claim it:
                            // `¬((a p b) => *(c q d))` refuted the rule and
                            // asserted `c q d`.
                            if (inner_args.size() >= 3 && is_atom(inner_args[1], "=>")) refuse_focus_in_held_rule(inner_args, true);

                            const Impl::BlockScope claim(_pImpl->_claim_code, !pattern);
                            return _pImpl->build_smart_call("zelph/refute", inner_args);
                        }

                        throw std::runtime_error(
                            "\"¬\" on its own line says that a FACT does not hold, so it needs one -- "
                            "write \"¬(a p b)\". Inside a condition it is the negation-as-failure "
                            "operator and applies to a pattern.");
                    }

                    if (asks)
                    {
                        // A negation needs a condition to operate upon, and
                        // "¬(X)" possesses none: it built nothing, printed
                        // nothing, and answered nothing. Two values
                        // constitute a statement that is short, and
                        // build_smart_call names what is missing.
                        if (val_type == "negation" && val_len >= 2 && !is_statement_ast(val_data[1])
                            && script::nested_value_count(val_data[1]) != 2)
                            throw std::runtime_error(
                                "\"¬\" in a query negates a condition, so it needs one -- a pattern such as "
                                "\"¬(X p b)\".");

                        refuse_in_mentions(root_data[1]);
                        if (focused_negation(root_data[1], true)) refuse_focused_negation();
                        if (_pImpl->focused_refutation(root_data[1], true)) refuse_focused_refutation(false);
                        refuse_misplaced(Impl::misplaced_condition_op(root_data[1], true, true), true);

                        const Impl::BlockScope query(_pImpl->_query_code, true);
                        return query_code(_pImpl->transform_arg(root_data[1]));
                    }

                    // A rule that this value holds -- "{((X go Y, ¬(X p b))
                    // => (X ok Y))}" -- states precisely what it states:
                    // the locations where its operators belong are its
                    // condition slots.
                    //
                    // A path marker positioned on a self-fact's own
                    // predicate, ":p⁺ a", constitutes the line's intrinsic
                    // path, which zelph/path-guard governs after the ends
                    // have been resolved, just as in "a p⁺ b".
                    if (!pattern)
                    {
                        const bool own_path = val_type == "selffact" && Impl::is_path_ast(root_data[1]);
                        refuse_in_mentions(root_data[1]);
                        refuse_in_assertion(Impl::nested_condition_op(own_path ? val_data[2] : root_data[1], false));
                        refuse_written_out(_pImpl->written_out_condition_op(root_data[1]));
                    }
                }
                const bool             claims = !pattern && !island_asks && !names_query_variable(root_data[1]);
                const Impl::BlockScope claim(_pImpl->_claim_code, claims);
                return claim_code(_pImpl->transform_arg(root_data[1]), claims, root_data[1]);
            }
            else
            {
                // Fact (S P O...)
                std::vector<Janet> fact_args;
                for (int i = 1; i < root_len; ++i)
                {
                    fact_args.push_back(root_data[i]);
                }

                const bool rule = is_atom(fact_args[1], "=>");
                bool       asks = false;
                if (!rule)
                {
                    asks = island_asks;
                    for (const Janet& arg : fact_args)
                        asks = asks || names_query_variable(arg);
                }

                for (const Janet& arg : fact_args)
                {
                    if (focused_negation(arg, is_name(fact_args[1], "=>")))
                        throw std::runtime_error(
                            "In a rule, \"*(P) ~ negation\" is not a negation: the focus makes it the pattern "
                            "P itself, so the rule would read P positively. Write a negated condition as "
                            "\"¬(P)\" or \"((P) ~ negation)\".");
                    if (asks && focused_negation(arg, true)) refuse_focused_negation();
                    if (_pImpl->focused_refutation(arg, is_name(fact_args[1], "=>"))) refuse_focused_refutation(true);
                    if (asks && _pImpl->focused_refutation(arg, true)) refuse_focused_refutation(false);
                }

                // The whole line constitutes a marking: `(F) ~
                // refuted` or `*(F) ~ refuted`, as stated by
                // fact_args[0] (refutation_operand).
                const Janet* refuted_data  = nullptr;
                int32_t      refuted_len   = 0;
                bool         refuted_focus = false;
                const bool   marking_line  = !rule && fact_args.size() == 3 && is_name(fact_args[1], "~")
                                          && _pImpl->denotes_zelph_name(fact_args[2], "refuted")
                                          && refutation_operand(fact_args[0], refuted_data, refuted_len, refuted_focus);

                // The query condition, when read through the focus, is P
                // itself, interpreted in a positive manner.
                if (asks && marking_line && refuted_focus) refuse_focused_refutation(false);

                // The fact a negation builds, expressed as the statement itself
                // -- "(X p b) ~ negation" -- constitutes the negated condition,
                // which in a query corresponds to what it is within a rule
                // (Zelph::negation_operand).
                const bool negation_layer = fact_args.size() == 3 && is_name(fact_args[1], "~") && is_name(fact_args[2], "negation");

                if (rule)
                {
                    refuse_rule_slots(fact_args);
                }
                else if (asks)
                {
                    // A query consists of a condition list that is evaluated
                    // just once, with its single condition being the statement
                    // itself: an operator can reside at its top -- the
                    // negation it is written out as, a path marker on its
                    // predicate -- and never within it, exactly as in a rule's
                    // condition. A rule it mentions possesses the slots of any
                    // rule (refuse_in_mentions). The comma list expressed in
                    // full, "*{...} ~ conjunction", functions as a condition
                    // list, and each of its members qualifies as a condition.
                    for (const Janet& arg : fact_args)
                        refuse_in_mentions(arg);

                    const Janet* members;
                    int32_t      count;
                    if (explicit_conjunction(tree->data[0], members, count))
                    {
                        for (int32_t i = 0; i < count; ++i)
                            refuse_misplaced(Impl::misplaced_condition_op(members[i], true, true), true);
                    }
                    else if (negation_layer && is_statement_ast(fact_args[0]))
                    {
                        const Janet* subject;
                        int32_t      subject_len;
                        if (janet_indexed_view(fact_args[0], &subject, &subject_len) && subject_len >= 1
                            && janet_checktype(subject[0], JANET_KEYWORD)
                            && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(subject[0]))) == "focused")
                            refuse_focused_negation();
                        refuse_misplaced(Impl::misplaced_condition_op(fact_args[0], true, true), true);
                    }
                    else
                    {
                        for (const Janet& arg : fact_args)
                            refuse_misplaced(Impl::nested_condition_op(arg, false), true);
                    }
                }
                else if (!pattern)
                {
                    // Outside a rule and outside a query, there is no condition
                    // slot, so `¬` has no reading below the top level either --
                    // and it was not reported, it was DROPPED. The sugar builds
                    // its operand with `zelph/fact` and tags the result, so a
                    // statement that denies a fact came away claiming it.
                    //
                    // Same shape as the consequence slot, and as `¬(F)` on its own
                    // line before df49661 gave that one a reading. The residue is
                    // the same too: a negation of a node no rule negates.
                    //
                    // The whole statement is asked, predicate included: the guard
                    // must sit ahead of `build_smart_call`, because by the time
                    // `zelph/negate` receives its argument the fact underneath it
                    // has been asserted. Its two siblings are asked here for the
                    // same reason and by the same walk -- see nested_condition_op
                    // for what each of them did instead of being reported. A rule
                    // the statement mentions possesses the slots characteristic
                    // of any rule (refuse_in_mentions).
                    for (const Janet& arg : fact_args)
                    {
                        refuse_in_mentions(arg);
                        refuse_in_assertion(Impl::nested_condition_op(arg, false));
                    }

                    // Expressed in full, the three instances refer to
                    // identical nodes, positioned at the top of the
                    // statement or located anywhere within it -- yet not
                    // within a rule it mentions, which prints a negated
                    // condition in that specific form and must be read back
                    // as identical to itself.
                    refuse_written_out(_pImpl->explicit_condition_op(fact_args.data(), static_cast<int32_t>(fact_args.size())));
                    for (const Janet& arg : fact_args)
                        refuse_written_out(_pImpl->written_out_condition_op(arg));
                }

                // A path marker standing ALONE is not refused here: whether its
                // ends are variables is decided when they are resolved, not by the
                // syntax, and "S P279⁺ Q3" is a legitimate question. The walk above
                // does not reach that shape -- its positions are atoms, not nested
                // arguments. See janet_cfun_zelph_path_guard for the ground case.

                // A line standing alone with a ground marking serves as the
                // refutation of F, expressed exactly as the fact the refutation
                // writes, and carries the same meaning as "¬(F)": refused for
                // an F the graph claims, the probability half written for one
                // it does not, and the identical statement repeated alters
                // nothing -- thus, the printed marking re-enters. Built via
                // zelph/fact, the marking first asserted F as a nested
                // statement, then took it out of every query: following
                // "a p b", the line "*(a p b) ~ refuted" quietly un-claimed it,
                // and after "¬(a p b)", the line was refused as known to be
                // wrong.
                if (marking_line && !asks && !pattern)
                {
                    const Impl::BlockScope claim(_pImpl->_claim_code, true);

                    std::vector<Janet> refuted_args(refuted_data + 1, refuted_data + refuted_len);
                    const std::string  refute = _pImpl->build_smart_call("zelph/refute", refuted_args);
                    if (refuted_focus) return refute;
                    return "(zelph/fact " + refute + " " + _pImpl->transform_arg(fact_args[1]) + " " + _pImpl->transform_arg(fact_args[2]) + ")";
                }

                // A focus on a part of the rule itself --
                // `*{(X p Y)} => (X q Y)` -- makes the statement evaluate to
                // that part. The thunk returns the `=>` fact beside it,
                // enabling dedup-rule to perform its checks and mark the rule,
                // then return the same part of the rule that it keeps.
                const bool focused_rule = rule && std::any_of(fact_args.begin(), fact_args.end(), [](const Janet& arg)
                                                              {
                                                                  const Janet* data;
                                                                  int32_t      len;
                                                                  return janet_indexed_view(arg, &data, &len) && len >= 1 && janet_checktype(data[0], JANET_KEYWORD)
                                                                      && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) == "focused"; });

                const Impl::BlockScope query(_pImpl->_query_code, asks || pattern);
                const Impl::BlockScope claim(_pImpl->_claim_code, !rule && !asks && !pattern);
                std::string            call = _pImpl->build_smart_call("zelph/fact", fact_args, "", focused_rule);

                const NestedOp written_out = _pImpl->explicit_condition_op(fact_args.data(), static_cast<int32_t>(fact_args.size()));
                if (written_out != NestedOp::None) call = operand_code(call);

                // A rule statement is wrapped so that the WHOLE construction --
                // condition patterns, conjunction set, => fact -- happens where
                // it can be undone if the rule turns out to be one the graph
                // already has. See janet_cfun_zelph_dedup_rule. Recognising the
                // statement here, at the one place that knows it is a top-level
                // `S => O`, keeps the check off every other input line.
                if (rule) return "(zelph/dedup-rule (fn [] " + call + (focused_rule ? ") :focus)" : "))");

                if (asks) return query_code(call);

                return claim_code(call, !rule && !pattern, tree->data[0]);
            }
        }
        else if (type == "conjunction")
        {
            // A comma-separated list at the top level. When a variable is
            // present, it is the query that the rule-condition syntax
            // suggests; in its absence, each condition is asserted, thus
            // no condition may serve as an operator.
            const Janet list = tree->data[0];
            const bool  asks = island_asks || names_query_variable(list);

            if (!pattern || asks) refuse_in_mentions(list);

            if (asks)
            {
                if (focused_negation(list, true)) refuse_focused_negation();
                if (_pImpl->focused_refutation(list, true)) refuse_focused_refutation(false);
                refuse_misplaced(Impl::misplaced_condition_op(list, true, true), true);
            }
            else if (!pattern)
            {
                refuse_in_assertion(Impl::nested_condition_op(list, false));
                refuse_written_out(_pImpl->written_out_condition_op(list));

                // A member's own predicate is not considered an argument,
                // hence the traversal described above fails to detect a
                // marker at that location, and zelph/path-guard rejected
                // the input "c q d, a p⁺ b" solely after `c q d` had been
                // asserted.
                if (Impl::writes_path_marker(list)) refuse_in_assertion(NestedOp::Path);
            }

            const Impl::BlockScope query(_pImpl->_query_code, asks || pattern);
            const Impl::BlockScope claim(_pImpl->_claim_code, !asks && !pattern);
            const std::string      code = _pImpl->transform_arg(list);
            return asks ? query_code(code) : claim_code(code, !pattern, list);
        }

        return "";
    }

    // This is asked before any part of the statement executes, so the
    // inline-keyword islands are not expanded: a text that the grammar
    // parses solely post-expansion does not register an operator in this
    // context.
    bool ScriptEngine::carries_condition_operator(const std::string& input) const
    {
        using NestedOp = Impl::NestedOp;

        Janet match_fun_out;
        if (janet_resolve(_pImpl->_janet_env, janet_csymbol("zelph-safe-parse"), &match_fun_out) != JANET_BINDING_DEF) return false;

        Janet args[2] = {_pImpl->_zelph_peg, janet_cstringv(input.c_str())};
        Janet result;
        if (Impl::pcall_rooted(janet_unwrap_function(match_fun_out), 2, args, &result) != JANET_SIGNAL_OK) return false;
        if (!janet_checktype(result, JANET_ARRAY)) return false;

        const JanetArray* tree = janet_unwrap_array(result);
        if (tree->count < 1) return false;

        const Janet* root_data;
        int32_t      root_len;
        if (!janet_indexed_view(tree->data[0], &root_data, &root_len) || root_len < 1) return false;

        if (root_len >= 4 && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(root_data[0]))) == "root"
            && is_atom(root_data[2], "=>"))
            return false;

        if (Impl::nested_condition_op(tree->data[0], false) != NestedOp::None) return true;
        if (_pImpl->written_out_condition_op(tree->data[0]) != NestedOp::None) return true;
        if (Impl::writes_path_marker(tree->data[0])) return true;

        // The statement's inherent positions: the statement as a whole
        // written out, or a path marker situated on its predicate.
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(root_data[0]))) != "root" || root_len < 4) return false;
        if (_pImpl->explicit_condition_op(root_data + 1, root_len - 1) != NestedOp::None) return true;

        std::string token;
        std::string base;
        std::string mode;
        return Impl::atom_text(root_data[2], token) && Impl::split_path_marker(token, base, mode);
    }

    // Whether, in the grammar's reading, a statement of these values is
    // complete: when it consists of three or more values, or when a single
    // value stands independently -- such as a set, a collection, a list, a
    // focus, or a value positioned after a prefix (`¬`, `≈net`, `:p`). A
    // name remains pending until the rest of its statement is provided, and
    // a group -- "(a p b)", or a comma-separated sequence "(a p b, c q d)"
    // -- functions as a term that also awaits completion, except in cases
    // where a statement may be a group: either as a condition within a comma
    // list, or following the "?" prefix.
    static bool reads_whole(Janet node, const bool group)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
        if (len - 1 >= 3) return true;
        if (len - 1 != 1) return false;

        const Janet* value;
        int32_t      value_len;
        if (!janet_indexed_view(data[1], &value, &value_len) || value_len < 1 || !janet_checktype(value[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(value[0]));
        if (type == "nested" || type == "conjunction") return group;
        return type != "atom" && type != "var" && type != "unquote" && type != "number";
    }

    // Asked of the text as the grammar reads it, before the islands are
    // expanded -- the expansion invokes their handlers, which build nodes,
    // and the question reduces to determining if a line has concluded. A
    // text that the grammar fails to read remains incomplete, as the
    // scanner took it.
    bool ScriptEngine::Impl::reads_complete(const std::string& code) const
    {
        // The prefix of a result query, identified as Interactive::process
        // identifies it.
        std::string       text    = code;
        bool              request = false;
        const std::size_t q       = code.find_first_not_of(" \t\r\n");
        if (q != std::string::npos && code[q] == '?'
            && (q + 1 == code.size() || code[q + 1] == ' ' || code[q + 1] == '\t' || code[q + 1] == '\n'
                || code[q + 1] == '(' || code[q + 1] == '$'))
        {
            request = true;
            text    = code.substr(q + 1);
        }

        Janet match_fun_out;
        if (janet_resolve(_janet_env, janet_csymbol("zelph-safe-parse"), &match_fun_out) != JANET_BINDING_DEF) return false;

        Janet args[2] = {_zelph_peg, janet_cstringv(text.c_str())};
        Janet result;
        if (pcall_rooted(janet_unwrap_function(match_fun_out), 2, args, &result) != JANET_SIGNAL_OK) return false;
        if (!janet_checktype(result, JANET_ARRAY)) return false;

        const JanetArray* tree = janet_unwrap_array(result);
        if (tree->count < 1) return false;

        const Janet* root;
        int32_t      root_len;
        if (!janet_indexed_view(tree->data[0], &root, &root_len) || root_len < 2 || !janet_checktype(root[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(root[0]));
        if (type == "root") return reads_whole(tree->data[0], request);
        if (type != "conjunction") return false;

        for (int32_t i = 1; i < root_len; ++i)
            if (!reads_whole(root[i], true)) return false;
        return true;
    }

    // Helper to generate Janet code for a function call with potential focused arguments.
    // func_name: "zelph/fact" or "zelph/set"
    // args: Array of Janet tuples (the AST nodes)
    // A transitive path condition is written by SUFFIXING the predicate:
    // (X P⁺ Y) is one or more P steps, (X P∗ Y) zero or more. The marker is
    // recognised in PREDICATE POSITION ONLY, and only when something is left
    // of the token once it is removed -- which is what keeps it from being a
    // reserved character. A name may still contain ⁺ or ∗ anywhere (`Na⁺` is
    // a Wikidata label), and a predicate that IS the marker stays itself.
    //
    // ASCII `+` deliberately has no such reading: `z+` and `d+` are the
    // addition predicates of the integer and decimal arithmetic modules, and
    // `*` is the focus operator. ⁺ (U+207A) and ∗ (U+2217) are used nowhere
    // in the stdlib, are not in :reserved, and are what a mathematical reader
    // expects for R⁺ and R∗.
    bool ScriptEngine::Impl::split_path_marker(const std::string& token, std::string& base, std::string& mode)
    {
        static const std::string one_plus  = "\xE2\x81\xBA"; // U+207A SUPERSCRIPT PLUS SIGN
        static const std::string zero_plus = "\xE2\x88\x97"; // U+2217 ASTERISK OPERATOR

        for (const auto& [marker, name] : {std::pair{one_plus, "one-or-more"}, std::pair{zero_plus, "zero-or-more"}})
        {
            if (token.size() > marker.size() && token.compare(token.size() - marker.size(), marker.size(), marker) == 0)
            {
                base = token.substr(0, token.size() - marker.size());
                mode = name;
                return true;
            }
        }
        return false;
    }

    // The predicate of a statement, if it is a plain atom: [:atom "text"].
    bool ScriptEngine::Impl::atom_text(Janet arg, std::string& text)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(arg, &data, &len) || len < 2) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) != "atom") return false;
        if (!janet_checktype(data[1], JANET_STRING)) return false;
        text = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));
        return !(text.size() >= 2 && text.front() == '"'); // a quoted atom is a literal name, never an operator
    }

    // Does this argument stand for a path condition? The marker has no AST
    // node of its own -- it is a suffix on the PREDICATE token -- so the
    // statement has to be decomposed far enough to look at it. Asked of the
    // SYNTAX for the same reason is_negation_ast is: the tag fact the sugar
    // builds is hash-consed and says nothing about which line wrote it.
    bool ScriptEngine::Impl::is_path_ast(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        std::string       token;
        std::string       base;
        std::string       mode;

        // The self-path "(x p⁺ x)" corresponds to the selffact
        // `[:selffact "p⁺" value]` (refer to the selffact transform),
        // with its predicate token expressed as a plain string.
        if (type == "selffact") return len >= 3 && split_path_marker(token_text(data[1]), base, mode);

        if (type != "nested") return false;
        if (len == 2) return is_path_ast(data[1]);

        // [:nested subject predicate object ...] -- the same positions
        // build_smart_call reads.
        return atom_text(data[2], token) && split_path_marker(token, base, mode);
    }

    // Whether a statement or a condition anywhere within this value
    // carries a path marker on its predicate, outside the rules it
    // mentions. A comma list's conditions are not enclosed in `(...)`
    // groups, thus is_path_ast, which inquires about a group, fails to
    // recognize "X go Y, a P279⁺ d".
    bool ScriptEngine::Impl::writes_path_marker(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if ((type == "nested" || type == "condition") && len >= 4)
        {
            if (is_atom(data[2], "=>")) return false;

            std::string token;
            std::string base;
            std::string mode;
            if (atom_text(data[2], token) && split_path_marker(token, base, mode)) return true;
        }
        if (type == "selffact" && is_path_ast(node)) return true;

        for (int32_t i = 1; i < len; ++i)
            if (writes_path_marker(data[i])) return true;
        return false;
    }

    // Which condition operator, if any, stands INSIDE this argument? A plain
    // statement has no condition slot, so none of the three has a reading
    // below its top level -- and none of them was REPORTED there, each was
    // quietly built. `¬` was dropped and its operand asserted;
    // "x q (≈net(a p b))" then answered `a p b` to "S p O", which is the one
    // thing the refusal of a bare `≈` says a statement cannot do; and a path
    // marker hung a closure tag off a fact where nothing ever walks one.
    //
    // Asked of the SYNTAX, and the walk descends through CHILDREN rather than
    // through a list of the value forms that may carry one: a form added to
    // the grammar later must not reopen the hole by not being listed. The
    // statement's own top-level positions are atoms, so a path QUESTION --
    // "S P279⁺ b", which is legitimate and answers one -- is not reached.
    //
    // `into_rules` false leaves a rule within the argument untouched: its
    // slots are the places where its own operators belong.
    ScriptEngine::Impl::NestedOp ScriptEngine::Impl::nested_condition_op(Janet node, const bool into_rules)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return NestedOp::None;

        if (janet_checktype(data[0], JANET_KEYWORD))
        {
            const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
            if (type == "negation") return NestedOp::Negation;
            if (type == "approx") return NestedOp::Approx;
            if (!into_rules && (type == "nested" || type == "condition") && len > 3 && is_atom(data[2], "=>")) return NestedOp::None;
        }

        if (is_path_ast(node)) return NestedOp::Path;

        for (int32_t i = 1; i < len; ++i)
            if (const NestedOp inner = nested_condition_op(data[i], into_rules); inner != NestedOp::None) return inner;

        return NestedOp::None;
    }

    // The same question for a RULE, where the answer is not "nowhere" but "only
    // at the top of a condition". A rule has the slots these operators exist
    // for, and they were checked at the top of one and nowhere else -- so a
    // marker one argument down was built and never read: "(x q (¬(a p b))) =>
    // (c r d)" printed back as "(x q (a p b)) => (c r d)", the operator gone
    // from a rule that now means something else, and "(x q (a P279⁺ d))" tagged
    // a fact no closure is ever walked for. The consequence slot had the checks
    // but only the shallow ones, so the same thing one argument in derived the
    // pattern the operator denied.
    //
    // `in_condition` says which slot this node stands in, because that is the
    // whole of what decides. A condition may BE an operator, so the operator
    // layers are peeled here and the fact under them is what gets walked; an
    // argument and a consequence may not, so anything found there is reported.
    // A nested "=>" is a rule again -- that is a rule GENERATOR, whose inner
    // rule has its own slots and its own right to a `¬`. A query activates
    // `skip_rules`, because it reads a rule it mentions for that rule's own
    // slots (refuse_in_mentions), regardless of where the rule is positioned.
    // `skip_mentions` bypasses only those rules positioned as an argument and
    // still opens the inner rule of a generator, enabling a typed rule to
    // distinguish an operator embedded within a rule it holds from one
    // incorrectly placed within its own slots (refuse_in_held_rule).
    ScriptEngine::Impl::NestedOp ScriptEngine::Impl::misplaced_condition_op(Janet node, const bool in_condition, const bool skip_rules, const bool skip_mentions)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return NestedOp::None;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return NestedOp::None;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        const bool        slot = (type == "nested" || type == "condition");

        // A rule, no matter where it stands: its own condition and
        // consequence slots open again.
        if (slot && len > 3 && is_atom(data[2], "=>"))
        {
            if (skip_rules) return NestedOp::None;
            if (const NestedOp op = misplaced_condition_op(data[1], true, false, skip_mentions); op != NestedOp::None) return op;
            for (int32_t i = 3; i < len; ++i)
                if (const NestedOp op = misplaced_condition_op(data[i], false, false, skip_mentions); op != NestedOp::None) return op;
            return NestedOp::None;
        }

        const bool into_rules = !skip_rules && !skip_mentions;

        if (!in_condition) return nested_condition_op(node, into_rules);

        if (type == "conjunction")
        {
            for (int32_t i = 1; i < len; ++i)
                if (const NestedOp op = misplaced_condition_op(data[i], true, skip_rules, skip_mentions); op != NestedOp::None) return op;
            return NestedOp::None;
        }

        // The layers a condition is permitted to wear, peeled one at a time
        // so that the legitimate stack -- "¬≈net(...)", "¬(C P⁺ T)" --
        // survives.
        if (type == "negation" && len >= 2) return misplaced_condition_op(data[1], true, skip_rules, skip_mentions);
        if (type == "approx" && len >= 3) return misplaced_condition_op(data[2], true, skip_rules, skip_mentions);
        if (slot && len == 2) return misplaced_condition_op(data[1], true, skip_rules, skip_mentions); // plain grouping

        // `((P) ~ negation)` represents the logical negation `¬(P)`
        // expressed explicitly (Zelph::negation_operand), meaning it is
        // the same layer: its subject is the condition. When walked as an
        // ordinary fact, its subject counted as an argument, and
        // `((C P⁺ T) ~ negation)` was refused because of a marker that the
        // negation reads.
        if (slot && len == 4 && is_name(data[2], "~") && is_name(data[3], "negation"))
            return misplaced_condition_op(data[1], true, skip_rules, skip_mentions);

        // The comma list expressed in full, "*{...} ~ conjunction",
        // functions as a condition list, as the comma list itself does.
        const Janet* members;
        int32_t      count;
        if (explicit_conjunction(node, members, count))
        {
            for (int32_t i = 0; i < count; ++i)
                if (const NestedOp op = misplaced_condition_op(members[i], true, skip_rules, skip_mentions); op != NestedOp::None) return op;
            return NestedOp::None;
        }

        // A condition written as a self-fact is a statement in which the
        // operand corresponds to its argument: "(:p ¬(X q b)) => ..." built
        // a rule where the condition holds the negation fact as a term, and
        // displayed it back devoid of the "¬". The marker of ":p⁺ X" stands
        // on the condition's inherent predicate, at the location where a
        // path belongs.
        if (type == "selffact") return len >= 3 ? nested_condition_op(data[2], into_rules) : NestedOp::None;

        // The condition itself, finally. Its own predicate may carry a path
        // marker; its arguments may carry nothing.
        if (slot)
        {
            for (int32_t i = 1; i < len; ++i)
                if (const NestedOp op = nested_condition_op(data[i], into_rules); op != NestedOp::None) return op;
            return NestedOp::None;
        }

        // A condition expressed as some alternative value is also a
        // statement, and every component within it serves as an argument:
        // the members of a set.
        return nested_condition_op(node, into_rules);
    }

    // Which condition operator is located within a set or a collection
    // literal. A set holds nodes and nothing reads it as conditions, so an
    // operator among its members is a term that nobody evaluates -- and the
    // operator does not even appear when printed, since only a rule's
    // condition slot writes `¬`. A rule embedded within the literal opens
    // its condition slots once more ("{((X go Y, ¬(X p b)) => (X ok Y))}"
    // constitutes a set with one rule), and a set inside one of those is
    // asked again in turn. The operators written out are identical nodes,
    // and the comma list written out, "*{...} ~ conjunction", is not a data
    // set but rather the condition list itself.
    ScriptEngine::Impl::NestedOp ScriptEngine::Impl::set_literal_op(Janet node, bool in_set) const
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return NestedOp::None;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return NestedOp::None;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if ((type == "nested" || type == "condition") && len > 3 && is_atom(data[2], "=>")) in_set = false;

        const Janet* members;
        int32_t      count;
        if (explicit_conjunction(node, members, count))
        {
            for (int32_t i = 0; i < count; ++i)
                if (const NestedOp op = set_literal_op(members[i], false); op != NestedOp::None) return op;
            return NestedOp::None;
        }

        if (type == "set" || type == "collection") in_set = true;

        if (in_set)
        {
            if (type == "negation") return NestedOp::Negation;
            if (type == "approx") return NestedOp::Approx;
            if (is_path_ast(node)) return NestedOp::Path;
            if (const NestedOp op = explicit_condition_op(node); op != NestedOp::None) return op;
        }

        for (int32_t i = 1; i < len; ++i)
            if (const NestedOp op = set_literal_op(data[i], in_set); op != NestedOp::None) return op;
        return NestedOp::None;
    }

    // Whether a token denotes the node that `name` identifies within the
    // zelph language: the operator node itself, not a name that appears
    // similar. Within ".lang en", a token labelled "closure" represents a
    // node in that language, and a fact concerning it constitutes data; the
    // core spellings ("~", "negation") are resolved in every language
    // unless the language gives that name to a node of its own. A name
    // lacking a node currently signifies the operator exclusively when
    // instantiation of it creates the operator, in the zelph language.
    bool ScriptEngine::Impl::denotes_zelph_name(Janet token, const char* name) const
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(token, &data, &len) || len < 2) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        // A focus alters the value to which the statement evaluates,
        // without affecting the node that the token names: "(a q b) ~
        // *negation" still constructs the negation regardless, and
        // asserted `a q b` alongside it.
        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type == "focused") return denotes_zelph_name(data[1], name);
        if (type != "atom") return false;

        std::string text = token_text(data[1]);
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = string::unescape_atom(text.substr(1, text.size() - 2));

        const std::string&  lang  = _n->lang();
        const network::Node found = _n->get_node(text, lang);
        if (found != 0) return found == _n->get_node(name, "zelph");
        return lang == "zelph" && text == name;
    }

    // Whether a predicate token refers to a predicate declared as opaque,
    // read by lookup within the session's language: compiling a statement
    // creates no node, and a predicate lacking any existing node is not
    // considered declared. A variable does not constitute an atom, hence a
    // statement employing a variable as a predicate remains unwrapped.
    bool ScriptEngine::Impl::denotes_opaque_predicate(Janet token) const
    {
        if (!_n->has_opaque_predicates()) return false;

        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(token, &data, &len) || len < 2 || !janet_checktype(data[0], JANET_KEYWORD)) return false;
        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type == "focused") return denotes_opaque_predicate(data[1]);
        if (type != "atom") return false;

        std::string text = token_text(data[1]);
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = string::unescape_atom(text.substr(1, text.size() - 2));
        return _n->is_opaque_predicate(_n->get_node(text, _n->lang()));
    }

    // The counterpart of focused_negation for a refutation: `*(P) ~ refuted`
    // within a rule evaluates to the pattern P directly, meaning a
    // consequence derived P and a condition read P, both positively, while
    // the marking was written on the side as data -- a ground one at once
    // refuted what the rule then derived. Asked at the same positions, by
    // the node that the name denotes (denotes_zelph_name): `refuted` is a
    // named node in language `zelph`.
    bool ScriptEngine::Impl::focused_refutation(Janet node, const bool in_rule) const
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        const bool        slot = type == "nested" || type == "condition";

        if (in_rule && slot && len == 4 && is_name(data[2], "~") && denotes_zelph_name(data[3], "refuted"))
        {
            const Janet* subject;
            int32_t      subject_len;
            if (janet_indexed_view(data[1], &subject, &subject_len) && subject_len >= 1
                && janet_checktype(subject[0], JANET_KEYWORD)
                && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(subject[0]))) == "focused")
                return true;
        }

        const bool rule = in_rule || (slot && len > 3 && is_name(data[2], "=>"));
        for (int32_t i = 1; i < len; ++i)
            if (focused_refutation(data[i], rule)) return true;
        return false;
    }

    // Whether a statement writes a refuted marking `S ~ refuted` at any
    // location: a statement that is claimed and writes one runs within
    // zelph/claim-text (claim_code), so that a refusal leaves nothing it
    // built.
    bool ScriptEngine::Impl::writes_refuted_marking(Janet node) const
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if ((type == "nested" || type == "condition" || type == "root") && len == 4 && is_name(data[2], "~") && denotes_zelph_name(data[3], "refuted"))
            return true;

        for (int32_t i = 1; i < len; ++i)
            if (writes_refuted_marking(data[i])) return true;
        return false;
    }

    // Whether a statement takes a node by reference at any location: a Janet
    // variable `,name`, which is also what an inline-keyword island
    // transforms into. The node may represent a rule's pattern whose claim
    // is refused, and the claim reaches it solely at the end of the line.
    bool ScriptEngine::Impl::takes_reference(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) == "unquote") return true;

        for (int32_t i = 1; i < len; ++i)
            if (takes_reference(data[i])) return true;
        return false;
    }

    // Which condition operator corresponds to a statement expressed as S P
    // O: the fact that the sugar builds -- `((P) ~ negation)` for `¬(P)`,
    // `((S p O) closure one-or-more)` for `S p⁺ O`, `((P) nn net)` for
    // `≈net(P)`. The nodes are identical, thus the evaluation interprets
    // them identically. Identified via the node each token denotes
    // (denotes_zelph_name), and only when the subject is a statement, which
    // is the one operand an operator takes.
    ScriptEngine::Impl::NestedOp ScriptEngine::Impl::explicit_condition_op(const Janet* parts, const int32_t count) const
    {
        if (count != 3 || !is_statement_ast(parts[0])) return NestedOp::None;
        if (denotes_zelph_name(parts[1], "~") && denotes_zelph_name(parts[2], "negation")) return NestedOp::Negation;
        if (denotes_zelph_name(parts[1], "closure")) return NestedOp::Path;
        if (denotes_zelph_name(parts[1], "nn")) return NestedOp::Approx;
        return NestedOp::None;
    }

    ScriptEngine::Impl::NestedOp ScriptEngine::Impl::explicit_condition_op(Janet node) const
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 2) return NestedOp::None;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return NestedOp::None;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type == "nested" && len == 2) return explicit_condition_op(data[1]); // plain grouping
        if (type == "nested" || type == "condition") return explicit_condition_op(data + 1, len - 1);
        return NestedOp::None;
    }

    // Which condition operator a value writes out anywhere within it,
    // outside the rules it mentions: a mentioned rule prints its negated
    // conditions in exactly this form, and reads back as itself.
    ScriptEngine::Impl::NestedOp ScriptEngine::Impl::written_out_condition_op(Janet node) const
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return NestedOp::None;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return NestedOp::None;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if ((type == "nested" || type == "condition") && len > 3 && is_atom(data[2], "=>")) return NestedOp::None;

        if (const NestedOp op = explicit_condition_op(node); op != NestedOp::None) return op;

        for (int32_t i = 1; i < len; ++i)
            if (const NestedOp op = written_out_condition_op(data[i]); op != NestedOp::None) return op;
        return NestedOp::None;
    }

    // `role` names the place the statement stands in, taken from the AST node
    // the caller is transforming -- "condition 2 of the comma list" and the
    // like. Empty where the surrounding form adds nothing to the message.
    // Using `with_fact`, a focused call yields the fact and the focused part
    // as a tuple: the shape zelph/dedup-rule takes with :focus. A non-empty
    // `focused_code` represents the code of the focused part, built by the
    // caller and accepted exactly as provided.
    std::string ScriptEngine::Impl::build_smart_call(const std::string& func_name, const std::vector<Janet>& args, const std::string& role, const bool with_fact, const std::string& focused_code) const
    {
        if (args.empty()) return "nil";

        // A statement comprises three parts, and when fewer than three are
        // present, it possesses a reading solely at the TOP level, where "a p"
        // awaits the rest of the line -- this is what makes a statement
        // spannable. Here the fragment is either parenthesised or
        // comma-separated, thus it is closed and simply short, and the error
        // that was issued was Janet's "arity mismatch, expected at least 3,
        // got 2": a calling convention nobody wrote, for the commonest typo
        // there is. Five surface forms reach this point with it -- a condition
        // of a comma list, a consequence, a term in subject or in object
        // position, and a command's pattern argument.
        //
        // Refused HERE rather than in zelph/fact, because a call evaluates its
        // arguments first: a check inside the function fires once the subject
        // and the predicate have been created, and the message would arrive
        // after the graph had already grown.
        if ((func_name == "zelph/fact" || func_name == "zelph/refute") && args.size() < 3)
            script::refuse_short_statement(role, args);

        // A marking that a claimed statement writes is itself claimed, and a
        // claimed marking serves as a refutation: it is constructed just like
        // the line "(F) ~ refuted" is, via zelph/refute, which refuses it for
        // an F the graph claims and accepts it again for one already
        // refuted. Built through zelph/fact, the operand F was initially
        // asserted, and the marking subsequently removed it from every query:
        // after "a p b", both "x says ((a p b) ~ refuted)" and "c q d, (a p
        // b) ~ refuted" un-claimed `a p b` without comment, and after "¬(a p
        // b)" both were refused as known to be wrong.
        if ((func_name == "zelph/fact" || func_name == "zelph/refute") && _claim_code && args.size() == 3 && is_name(args[1], "~")
            && denotes_zelph_name(args[2], "refuted"))
        {
            const Janet* refuted_data = nullptr;
            int32_t      refuted_len  = 0;
            bool         focus        = false;
            if (refutation_operand(args[0], refuted_data, refuted_len, focus))
            {
                // Zelph::fact rejects the refutation of a marking, yet a call
                // evaluates its arguments first, with the operand F being built
                // and asserted by that time. Refused in this context, a typed
                // line writes nothing.
                if (func_name == "zelph/refute")
                    throw std::runtime_error("\"(F) ~ refuted\" is a refutation, and a refutation cannot itself be refuted: it is withdrawn by "
                                             "removing its marking (.remove or .prune-facts)");

                const std::string refute = build_smart_call("zelph/refute", std::vector<Janet>(refuted_data + 1, refuted_data + refuted_len));
                if (focus) return refute;
                return "(zelph/fact " + refute + " " + transform_arg(args[1]) + " " + transform_arg(args[2]) + ")";
            }

            // A focus on a part of the subject transfers that part to the
            // marking, which refutes it, causing the code to construct the
            // part using zelph/refute. Built with zelph/fact, the marking
            // bypassed the refusal. Following "a p b", the line
            // "(x says *(a p b)) ~ refuted" refuted the claimed `a p b`. On a
            // graph excluding `a p b`, the line refuted `a p b` and claimed
            // `x says (a p b)`, which holds it. Both two-line spellings of
            // this are refused.
            std::vector<std::vector<Janet>> levels;
            bool                            whole = false;
            if (focused_refutation_operand(args[0], levels, whole, refuted_data, refuted_len))
            {
                if (func_name == "zelph/refute")
                    throw std::runtime_error("\"(F) ~ refuted\" is a refutation, and a refutation cannot itself be refuted: it is withdrawn by "
                                             "removing its marking (.remove or .prune-facts)");

                // Each statement on the way to F is constructed around the code
                // of the part it hands on, starting with the innermost.
                std::string subject = build_smart_call("zelph/refute", std::vector<Janet>(refuted_data + 1, refuted_data + refuted_len));
                for (auto level = levels.rbegin(); level != levels.rend(); ++level)
                    subject = build_smart_call("zelph/fact", *level, "", false, subject);
                if (whole) return subject;
                return "(zelph/fact " + subject + " " + transform_arg(args[1]) + " " + transform_arg(args[2]) + ")";
            }
        }

        // Path sugar, before anything else looks at the arguments: the
        // statement becomes the plain one-step fact, tagged.
        if (func_name == "zelph/fact" && args.size() >= 3)
        {
            std::string token;
            std::string base;
            std::string mode;
            if (atom_text(args[1], token) && split_path_marker(token, base, mode))
            {
                // Exactly one object is the only shape a path condition has --
                // evaluate_closure refuses the rest and says so. That is also
                // the shape whose two ends can be guarded, so the guard is
                // emitted for it and the others fall through to the refusal
                // the evaluator gives them.
                if (args.size() == 3)
                {
                    const std::string from = transform_arg(args[0]);
                    const std::string to   = transform_arg(args[2]);

                    // In a query, a ground path functions as a reachability
                    // test, just as it does in a rule, and there is no need
                    // for a guard.
                    if (_query_code)
                        return operand_code("(zelph/path (zelph/fact " + from + " \"" + string::escape_atom(base) + "\" " + to + ") \"" + mode + "\")");

                    // Bound once each: a variable token resolves to a FRESH
                    // variable node per evaluation, so naming the ends twice
                    // would give the guard and the pattern different variables.
                    const std::string head = "(let [$pfrom " + from + " $pto " + to + "]";
                    const std::string body = " (do (zelph/path-guard $pfrom $pto) (zelph/path (zelph/fact $pfrom \"";
                    return operand_code(head + body + string::escape_atom(base) + "\" $pto) \"" + mode + "\")))");
                }

                std::string call = "(zelph/fact " + transform_arg(args[0]) + " \"" + string::escape_atom(base) + "\"";
                for (size_t i = 2; i < args.size(); ++i)
                    call += " " + transform_arg(args[i]);
                call += ")";
                return operand_code("(zelph/path " + call + " \"" + mode + "\")");
            }
        }

        // Every argument of an opaque predicate constitutes a mention scope
        // (as detailed below), and any refutation marking placed within it
        // also counts as a mention: since it does not refute anything, it is
        // not built as a claim.
        //
        // A refutation within a claimed statement claims the marking rather
        // than the fact it refutes (Zelph::refute), and the content that the
        // fact holds is mentioned in the same way: `¬(x says (a p b))` used to
        // write a new `a p b` as data, while a marking inside the fact fails
        // to refute anything.
        const bool       opaque     = (func_name == "zelph/fact" || func_name == "zelph/refute") && args.size() >= 3 && denotes_opaque_predicate(args[1]);
        const bool       refutation = func_name == "zelph/refute" && _claim_code;
        const BlockScope opaque_arguments(_claim_code, _claim_code && !opaque && !refutation);

        int                      focused_index = -1;
        std::vector<std::string> arg_codes;
        arg_codes.reserve(args.size());

        for (size_t i = 0; i < args.size(); ++i)
        {
            const Janet* data;
            int32_t      len;
            if (!janet_indexed_view(args[i], &data, &len)) return "nil";

            std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));

            if (type == "focused")
            {
                if (focused_index != -1)
                {
                    // Error: Multiple foci (handled by returning nil or could throw)
                    // "Only one element... may have a star"
                    return "(error \"Zelph: Multiple focus markers (*) in one statement\")";
                }
                focused_index = (int)i;
                // Recursively transform the actual value inside the focus tag
                // [:focused val] -> data[1] is val
                arg_codes.push_back(focused_code.empty() ? transform_arg(data[1]) : focused_code);
            }
            else
            {
                arg_codes.push_back(transform_arg(args[i]));
            }
        }

        // Each argument of an opaque predicate constitutes a mention scope,
        // as a rule nested in a statement is rule text (zelph/rule-text):
        // what the argument creates is written down without being claimed.
        if (opaque || refutation)
        {
            for (size_t i = 0; i < args.size(); ++i)
                if ((i != 1 || refutation) && !is_plain_token(args[i])) arg_codes[i] = "(zelph/mention (fn [] " + arg_codes[i] + "))";
        }

        if (focused_index == -1)
        {
            // Simple case: No focus, just call the function
            std::string call = "(" + func_name;
            for (const auto& code : arg_codes)
                call += " " + code;
            call += ")";
            return call;
        }
        else
        {
            // Focused case: Use `let` to evaluate args, create side-effect, return focused arg.
            // (let [$0 arg0 $1 arg1 ... _ (func $0 $1 ...)] $focused_index)
            std::string let_block = "(let [";
            for (size_t i = 0; i < arg_codes.size(); ++i)
            {
                let_block += "$" + std::to_string(i) + " " + arg_codes[i] + " ";
            }
            // The fact that a focus writes beside the part it hands on passes
            // through zelph/aside, which records it for the statement around
            // it: a line claims it as it claims what it writes, and a mention
            // marks it with what the mention wrote. A rule's own focus hands
            // on a part of the rule itself, whose `=>` fact is no such fact.
            const bool aside = func_name == "zelph/fact" && !with_fact;
            let_block += (aside ? "_ (zelph/aside (" : "_ (") + func_name;
            for (size_t i = 0; i < arg_codes.size(); ++i)
            {
                let_block += " $" + std::to_string(i);
            }
            const std::string focused = "$" + std::to_string(focused_index);
            let_block += (aside ? "))] " : ")] ") + (with_fact ? "[_ " + focused + "]" : focused) + ")";
            return let_block;
        }
    }

    // Convert PEG-AST tuple to Janet Source Code String
    std::string ScriptEngine::Impl::transform_arg(Janet arg_tuple) const
    {
        if (!janet_checktype(arg_tuple, JANET_TUPLE) && !janet_checktype(arg_tuple, JANET_ARRAY)) return "nil";

        const Janet* data;
        int32_t      len;
        janet_indexed_view(arg_tuple, &data, &len);

        if (len == 1 && janet_checktype(data[0], JANET_KEYWORD))
        {
            // An EMPTY container still denotes something: both `<>` and `{}`
            // are nil -- the empty cons list is the terminator every list
            // ends at, and Zelph::set() has always answered nil for the
            // empty set. Falling through to "nil" (Janet's, not the node)
            // made the whole statement evaluate to nothing, silently.
            const std::string tag = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
            if (tag == "set") return "(zelph/set)";
            if (tag == "collection") return "(zelph/collection)";
            if (tag == "list-nodes") return "(zelph/list)";
        }

        if (len < 2) return "nil"; // Minimum [:type value...]

        std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));

        if (type == "focused")
        {
            // If transform_arg is called directly on a focused node (e.g. root level single item),
            // just return the inner transformation. The focus has no effect if there's no surrounding operation.
            return transform_arg(data[1]);
        }
        else if (type == "nested")
        {
            // [:nested val1 val2 ...]
            // A single value in parentheses is plain grouping, not a fact.
            // Required for self-fact sugar in nested positions, e.g. a rule
            // consequence (:isprime N) or a subject ((:simplify T) = S).
            if (len == 2) return transform_arg(data[1]);

            std::vector<Janet> args;
            for (int32_t i = 1; i < len; ++i)
                args.push_back(data[i]);

            // A rule written inside another statement still constitutes rule
            // text: its ground parts are patterns instead of claims, and the
            // collections it writes belong exclusively to it
            // (zelph/rule-text).
            if (args.size() >= 3 && is_atom(args[1], "=>"))
            {
                refuse_focus_in_held_rule(args);
                const BlockScope mention(_claim_code, false);
                return "(zelph/rule-text (fn [] " + build_smart_call("zelph/fact", args) + "))";
            }

            // A condition operator expressed in full is the operator,
            // with its operand being a pattern as the sugar's is.
            if (explicit_condition_op(args.data(), static_cast<int32_t>(args.size())) != NestedOp::None)
                return operand_code(build_smart_call("zelph/fact", args));

            return build_smart_call("zelph/fact", args);
        }
        else if (type == "set")
        {
            // [:set val1 val2 ...]
            std::vector<Janet> args;
            for (int32_t i = 1; i < len; ++i)
                args.push_back(data[i]);
            return build_smart_call("zelph/set", args);
        }
        else if (type == "collection")
        {
            // [:collection val1 val2 ...]
            std::vector<Janet> args;
            for (int32_t i = 1; i < len; ++i)
                args.push_back(data[i]);
            return build_smart_call("zelph/collection", args);
        }
        else if (type == "conjunction")
        {
            // [:conjunction [:condition v1 v2 ...] [:condition v3 v4 ...] ...]
            // Build a set of condition facts, mark as conjunction, return the set node.
            // This is the desugared form of: (*{cond1 cond2 ...} ~ conjunction)
            std::vector<std::string> cond_codes;
            for (int32_t i = 1; i < len; ++i)
            {
                const Janet* cond_data;
                int32_t      cond_len;
                if (!janet_indexed_view(data[i], &cond_data, &cond_len) || cond_len < 2) continue;

                int cond_val_count = cond_len - 1; // excluding :condition tag
                if (cond_val_count == 1)
                {
                    // A comma list evaluates to the list, thus a focus on any
                    // of its conditions as a whole changed nothing and was
                    // dropped without comment: "*(a p b), c q d" asserted both,
                    // and "(*(X R Y), Y R Z) => (X R Z)" built the plain rule.
                    // A rule that another statement holds expresses this in
                    // its own words (refuse_focus_in_held_rule).
                    if (focuses_whole_value(cond_data[1]))
                        throw std::runtime_error(
                            "Condition " + std::to_string(i) + " of the comma list carries a focus, which would make the list evaluate "
                                                               "to that condition, and a comma list evaluates to the list itself: the focus changes "
                                                               "nothing. Write the condition without the focus.");

                    // Single value (e.g. a nested conjunction or negated pattern)
                    cond_codes.push_back(transform_arg(cond_data[1]));
                }
                else
                {
                    // Multiple values: treat as fact (S P O...), supports * focus
                    std::vector<Janet> args;
                    for (int32_t j = 1; j < cond_len; ++j)
                        args.push_back(cond_data[j]);
                    // The AST says which condition this is, so the message can
                    // say it too -- the same courtesy zelph/conjunction pays
                    // for a member that is not a statement.
                    const std::string role = "condition " + std::to_string(i) + " of the comma list";

                    // A rule among the members is a rule that another
                    // statement holds, as is the case within parentheses (the
                    // nested case above). Written with zelph/fact, its parts
                    // were claims: "x says ((m n o) => (c q d), j r s)"
                    // answered `c q d` and `m n o`, which the set spelling of
                    // the same line left as patterns.
                    if (args.size() >= 3 && is_atom(args[1], "=>"))
                    {
                        refuse_focus_in_held_rule(args);
                        const BlockScope mention(_claim_code, false);
                        cond_codes.push_back("(zelph/rule-text (fn [] " + build_smart_call("zelph/fact", args, role) + "))");
                        continue;
                    }

                    std::string code = build_smart_call("zelph/fact", args, role);
                    if (explicit_condition_op(args.data(), static_cast<int32_t>(args.size())) != NestedOp::None) code = operand_code(code);
                    cond_codes.push_back(code);
                }
            }

            if (cond_codes.empty()) return "nil";
            if (cond_codes.size() == 1) return cond_codes[0]; // Safety: shouldn't happen with PEG

            // (zelph/conjunction code0 code1 ...) -- the members are evaluated
            // left to right, as they were when this built the collection and
            // its tag inline. What the call adds is the refusal of a member
            // that is not a fact pattern.
            std::string call = "(zelph/conjunction";
            for (const auto& code : cond_codes)
                call += " " + code;
            call += ")";
            return call;
        }
        else if (type == "negation")
        {
            // [:negation inner]
            // Desugars ¬X to (zelph/negate X), which returns the negated
            // condition, the fact ((X) ~ negation).
            //
            // "¬¬(F)" says the plain condition F, and interpreting it in
            // this manner requires a second negation stratum, which is the
            // parked feature "¬(A, B)" is refused for; until then, the
            // operator says so instead of constructing a condition that the
            // evaluation cannot read.
            if (is_negation_ast(data[1]))
            {
                throw std::runtime_error(
                    "\"¬\" does not nest: \"¬¬(F)\" means \"F\", and reading it that way "
                    "needs a second negation level. Write the plain pattern for the "
                    "positive condition.");
            }

            // A focus on the whole operand had no effect and was dropped
            // without comment: "(¬*(X R Y), Y R Z) => (X R Z)" built the rule
            // using "¬(X R Y)".
            if (focuses_whole_value(data[1]))
                throw std::runtime_error(
                    "A focus directly inside \"¬\" changes nothing: \"¬\" negates the whole pattern that follows it, and the "
                    "condition is that negation, never the pattern alone. Write \"¬(P)\" without the focus.");

            // The operand is constructed in a context where it functions as a
            // pattern rather than a claim (zelph/operand): a fact refuted on
            // its own line is negated within a condition exactly as it
            // stands, whereas previously building it again resulted in the
            // failure "this fact is known to be wrong".
            return operand_code("(zelph/negate " + transform_arg(data[1]) + ")");
        }
        else if (type == "approx")
        {
            // [:approx net-name inner] -- desugars ≈net(pattern) to
            // (zelph/approx pattern "net"): tags the pattern with the fact
            // (pattern nn net) and returns the pattern node, so it can serve
            // as a rule condition like any other pattern.
            if (len < 3) return "nil";
            std::string net;
            if (janet_checktype(data[1], JANET_STRING))
                net = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));
            return operand_code("(zelph/approx " + transform_arg(data[2])
                                + " \"" + string::replace_all_copy(net, "\"", "\\\"") + "\")");
        }
        else if (type == "selffact")
        {
            // [:selffact pred-token value]
            // Desugars ":pred X" to the self-fact (X pred X). The operand is
            // evaluated exactly once and used as both subject and object, so
            // side effects (focus, fact creation) happen once and both sides
            // are guaranteed to be the same node. A variable token as
            // predicate (e.g. :R) keeps variable semantics, matching
            // ordinary fact parsing.
            if (len < 3) return "nil";

            std::string pred;
            if (janet_checktype(data[1], JANET_STRING))
                pred = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));
            else if (janet_checktype(data[1], JANET_BUFFER))
            {
                JanetBuffer* b = janet_unwrap_buffer(data[1]);
                pred           = std::string(reinterpret_cast<const char*>(b->data), b->count);
            }
            if (pred.empty()) return "nil";

            // The marker is a suffix on the PREDICATE token, and this is a
            // predicate position, so ":P⁺ X" is the self-path "(X P⁺ X)" --
            // the reachability of a node from itself, which is a cycle test.
            // Splitting it off here is what keeps the two spellings one thing:
            // the sugar took its token literally, so ":P279⁺ x" built a
            // predicate NAMED "P279⁺" and derived nothing, while "x P279⁺ x"
            // was read as a path.
            //
            // Emitted exactly as build_smart_call emits it, guard included and
            // for the same reason -- the guard has to run before the one-step
            // fact under the marker is built. What differs is that the operand
            // is bound ONCE and stands at both ends, which is the whole of
            // what the self-fact sugar promises; it also makes the guard's
            // question the right one, since a concrete operand is a ground
            // path and a variable is the question the engine answers.
            std::string base;
            std::string mode;
            if (split_path_marker(pred, base, mode))
                return operand_code("(let [$sf " + transform_arg(data[2]) + "] "
                                    + (_query_code ? "" : "(do (zelph/path-guard $sf $sf) ")
                                    + "(zelph/path (zelph/fact $sf \"" + string::escape_atom(base) + "\" $sf) \"" + mode + "\")"
                                    + (_query_code ? ")" : "))"));

            const std::string pred_code = string::is_var(pred)
                                            ? "'" + pred
                                            : "\"" + string::replace_all_copy(pred, "\"", "\\\"") + "\"";

            return "(let [$sf " + transform_arg(data[2]) + "] (zelph/fact $sf " + pred_code + " $sf))";
        }
        else if (type == "list-nodes")
        {
            // [:list-nodes val1 val2 ...] — node list < A B C >
            std::vector<Janet> args;
            for (int32_t i = 1; i < len; ++i)
                args.push_back(data[i]);
            return build_smart_call("zelph/list", args);
        }

        // Handle leaf nodes (atom, var, list-compact, unquote)
        // These expect data[1] to be the content string/buffer
        std::string val_str;
        if (janet_checktype(data[1], JANET_STRING))
        {
            val_str = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));
        }
        else if (janet_checktype(data[1], JANET_BUFFER))
        {
            val_str = reinterpret_cast<const char*>(janet_unwrap_buffer(data[1]));
        }

        if (type == "unquote")
        {
            // Janet variable reference: emit the variable name directly.
            // At runtime, resolve_janet_arg handles both string values
            // (resolved as node names) and zelph/node abstract values.
            return val_str;
        }
        else if (type == "var")
        {
            // Variables are symbols in Janet (e.g. X, _V).
            // IMPORTANT: We must quote them (e.g. 'A), otherwise Janet tries
            // to evaluate 'A' as a bound variable and fails if it's not defined.
            return "'" + val_str;
        }
        else if (type == "atom")
        {
            // Atoms are strings in Janet -- but the capture is zelph TEXT,
            // and handing it to Janet unchanged put zelph names through
            // Janet's escape set: a node written `a\b` came out named
            // `a<backspace>`. Decode zelph's own two escapes here, then
            // re-encode for Janet, which spells those same two the same way.
            std::string name = val_str;
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = string::unescape_atom(name.substr(1, name.size() - 2));

            return "\"" + string::escape_atom(name) + "\"";
        }
        else if (type == "list-compact")
        {
            // Convert <123> content to (zelph/list-chars "123").
            // janet_cfun_zelph_list_chars reverses the characters internally
            // so the LSB (rightmost char) becomes the outermost cons cell.
            // The content is not quoted in zelph, so it is a name already.
            std::string content = "\"" + string::escape_atom(val_str) + "\"";
            return "(zelph/list-chars " + content + ")";
        }
        else if (type == "number")
        {
            // &-literal: delegate the representation to the (redefinable)
            // Janet function zelph/number. Validation happens there too.
            std::string content = "\"" + string::replace_all_copy(val_str, "\"", "\\\"") + "\"";
            return "(zelph/number " + content + ")";
        }

        return "nil";
    }
}
