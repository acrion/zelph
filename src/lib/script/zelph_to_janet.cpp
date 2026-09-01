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

    // True if a PEG-AST node is a `¬` pattern, through any number of plain
    // groupings -- "¬(F)" and "(¬(F))" are the same statement.
    //
    // The question has to be asked of the SYNTAX, not of the resulting node: the
    // negation tag is a fact about the pattern node, and a ground pattern is
    // hash-consed, so a node negated in one rule carries the tag everywhere. Only
    // the AST knows which statement wrote the "¬".
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

    std::string ScriptEngine::parse_zelph_to_janet(const std::string& input) const
    {
        const std::string expanded = _pImpl->expand_inline_keywords(input);

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

                    // A statement of fewer than three values is incomplete, and an
                    // empty result is how this function says so -- the REPL then
                    // buffers the line and waits for the rest of it. That reading
                    // is right for "a p" and wrong for "≈net(a p b)", which is not
                    // the beginning of anything: `≈` asks what a network believes,
                    // which a condition may read and a line cannot assert. The
                    // symptom was silence, and the next line being swallowed with
                    // it. Its two siblings are refused a few lines below and in
                    // zelph/path-guard; this is the same refusal.
                    if (val_type == "approx")
                        throw std::runtime_error(
                            "\"≈\" is a condition operator and has no meaning in a plain statement: "
                            "it asks what a network believes, which a rule condition can read and a "
                            "statement cannot claim. Use it in a rule condition.");

                    // `¬(F)` on its own line is the one condition operator that
                    // has a reading outside a condition, and it is not the one it
                    // used to get. The sugar builds its operand with zelph/fact
                    // and tags the result, so the line ASSERTED F and then marked
                    // the very node it had just claimed as negated -- the graph
                    // said the fact holds and that it does not. It now means what
                    // it says: F is entered as known-wrong, through the
                    // probability argument Zelph::fact has always had.
                    if (val_type == "negation" && val_len >= 2)
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
                                "claim of it to deny. Use the path condition in a rule, under \"¬\" if "
                                "what you want is the absence of a path.");

                        if (is_approx_ast(val_data[1]))
                            throw std::runtime_error(
                                "\"≈\" is a condition operator, and a \"¬\" in front does not change "
                                "that: it asks what a network believes, which a rule condition can read "
                                "and a statement can neither claim nor deny. Use it in a rule "
                                "condition, under \"¬\" for the case the net does not confirm.");

                        const Janet* inner_data;
                        int32_t      inner_len;
                        if (janet_indexed_view(val_data[1], &inner_data, &inner_len)
                            && inner_len > 3
                            && janet_checktype(inner_data[0], JANET_KEYWORD)
                            && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(inner_data[0]))) == "nested")
                        {
                            std::vector<Janet> inner_args;
                            for (int32_t i = 1; i < inner_len; ++i)
                                inner_args.push_back(inner_data[i]);
                            return _pImpl->build_smart_call("zelph/refute", inner_args);
                        }

                        throw std::runtime_error(
                            "\"¬\" on its own line says that a FACT does not hold, so it needs one -- "
                            "write \"¬(a p b)\". Inside a rule condition it is the "
                            "negation-as-failure operator and applies to a pattern.");
                    }
                }
                return _pImpl->transform_arg(root_data[1]);
            }
            else
            {
                // Fact (S P O...)
                std::vector<Janet> fact_args;
                for (int i = 1; i < root_len; ++i)
                {
                    fact_args.push_back(root_data[i]);
                }
                // `¬` is a CONDITION operator. As a consequence it used to be
                // ignored outright: "(A p B) => ¬(A q B)" derived (x q y) -- the
                // exact opposite of what the rule says, without a word. What a
                // derived negation should mean is a separate question (zelph can
                // hold a fact as known-wrong, via the probability argument of
                // fact()); until it is answered, saying so beats guessing.
                if (is_atom(fact_args[1], "=>"))
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

                        switch (Impl::misplaced_condition_op(fact_args[i], i == 0))
                        {
                        case Impl::NestedOp::Negation:
                            throw std::runtime_error(
                                "\"¬\" applies to a whole condition, not to something inside one: the "
                                "tag it writes is read where the condition is, and nowhere below it. "
                                "Write it in front of the condition -- \"(A q B, ¬(A p B)) => ...\".");
                        case Impl::NestedOp::Approx:
                            throw std::runtime_error(
                                "\"≈\" applies to a whole condition, not to something inside one: it "
                                "asks what a network believes about the condition, and nothing reads "
                                "it below that. Write it as the condition -- "
                                "\"(X rel S, ≈net(S p O)) => ...\".");
                        case Impl::NestedOp::Path:
                            throw std::runtime_error(
                                "\"⁺\" and \"∗\" mark the predicate of a CONDITION, not of a fact "
                                "inside one: the closure is walked for the condition itself, and a "
                                "marker below it tags a fact nothing ever walks. Write the path as "
                                "its own condition.");
                        case Impl::NestedOp::None:
                            break;
                        }
                    }
                }
                else
                {
                    // Outside a rule there is no condition slot, so `¬` has no
                    // reading below the top level either -- and it was not
                    // reported, it was DROPPED. The sugar builds its operand with
                    // `zelph/fact` and tags the result, so a statement that denies
                    // a fact came away claiming it.
                    //
                    // Same shape as the consequence slot, and as `¬(F)` on its own
                    // line before df49661 gave that one a reading. The residue is
                    // the same too: a negation tag on a node no rule negates,
                    // which `.node` then reports as "Negated by a rule: yes".
                    //
                    // The whole statement is asked, predicate included: the guard
                    // must sit ahead of `build_smart_call`, because by the time
                    // `zelph/negate` receives its argument the fact underneath it
                    // has been asserted. Its two siblings are asked here for the
                    // same reason and by the same walk -- see nested_condition_op
                    // for what each of them did instead of being reported.
                    for (const Janet& arg : fact_args)
                    {
                        switch (Impl::nested_condition_op(arg))
                        {
                        case Impl::NestedOp::Negation:
                            throw std::runtime_error(
                                "\"¬\" is a condition operator and has no meaning inside a plain "
                                "statement: it succeeds when a pattern is ABSENT, which only a rule "
                                "condition can ask. On its own line \"¬(a p b)\" says that the fact "
                                "does not hold.");
                        case Impl::NestedOp::Approx:
                            throw std::runtime_error(
                                "\"≈\" is a condition operator and has no meaning inside a plain "
                                "statement: it asks what a network believes, which a rule condition "
                                "can read and a statement cannot claim. Use it in a rule condition.");
                        case Impl::NestedOp::Path:
                            throw std::runtime_error(
                                "\"⁺\" and \"∗\" are condition operators and have no meaning inside a "
                                "plain statement: reachability is what the engine WALKS. On its own "
                                "line \"S p⁺ b\" is a question and answers one; inside a rule it is a "
                                "condition.");
                        case Impl::NestedOp::None:
                            break;
                        }
                    }
                }

                // A path marker standing ALONE is not refused here: whether its
                // ends are variables is decided when they are resolved, not by the
                // syntax, and "S P279⁺ Q3" is a legitimate question. The walk above
                // does not reach that shape -- its positions are atoms, not nested
                // arguments. See janet_cfun_zelph_path_guard for the ground case.

                const std::string call = _pImpl->build_smart_call("zelph/fact", fact_args);

                // A rule statement is wrapped so that the WHOLE construction --
                // condition patterns, conjunction set, => fact -- happens where
                // it can be undone if the rule turns out to be one the graph
                // already has. See janet_cfun_zelph_dedup_rule. Recognising the
                // statement here, at the one place that knows it is a top-level
                // `S => O`, keeps the check off every other input line.
                if (is_atom(fact_args[1], "=>")) return "(zelph/dedup-rule (fn [] " + call + "))";

                return call;
            }
        }
        else if (type == "conjunction")
        {
            // Direktes Komma-Conjunction am Top-Level
            return _pImpl->transform_arg(tree->data[0]);
        }

        return "";
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
        if (type != "nested") return false;
        if (len == 2) return is_path_ast(data[1]);

        // [:nested subject predicate object ...] -- the same positions
        // build_smart_call reads.
        std::string token;
        std::string base;
        std::string mode;
        return atom_text(data[2], token) && split_path_marker(token, base, mode);
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
    ScriptEngine::Impl::NestedOp ScriptEngine::Impl::nested_condition_op(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 1) return NestedOp::None;

        if (janet_checktype(data[0], JANET_KEYWORD))
        {
            const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
            if (type == "negation") return NestedOp::Negation;
            if (type == "approx") return NestedOp::Approx;
        }

        if (is_path_ast(node)) return NestedOp::Path;

        for (int32_t i = 1; i < len; ++i)
            if (const NestedOp inner = nested_condition_op(data[i]); inner != NestedOp::None) return inner;

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
    // rule has its own slots and its own right to a `¬`.
    ScriptEngine::Impl::NestedOp ScriptEngine::Impl::misplaced_condition_op(Janet node, const bool in_condition)
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
            if (const NestedOp op = misplaced_condition_op(data[1], true); op != NestedOp::None) return op;
            for (int32_t i = 3; i < len; ++i)
                if (const NestedOp op = misplaced_condition_op(data[i], false); op != NestedOp::None) return op;
            return NestedOp::None;
        }

        if (!in_condition) return nested_condition_op(node);

        if (type == "conjunction")
        {
            for (int32_t i = 1; i < len; ++i)
                if (const NestedOp op = misplaced_condition_op(data[i], true); op != NestedOp::None) return op;
            return NestedOp::None;
        }

        // The layers a condition is permitted to wear, peeled one at a time
        // so that the legitimate stack -- "¬≈net(...)", "¬(C P⁺ T)" --
        // survives.
        if (type == "negation" && len >= 2) return misplaced_condition_op(data[1], true);
        if (type == "approx" && len >= 3) return misplaced_condition_op(data[2], true);
        if (slot && len == 2) return misplaced_condition_op(data[1], true); // plain grouping

        // The condition itself, finally. Its own predicate may carry a path
        // marker; its arguments may carry nothing.
        if (slot)
            for (int32_t i = 1; i < len; ++i)
                if (const NestedOp op = nested_condition_op(data[i]); op != NestedOp::None) return op;

        return NestedOp::None;
    }

    // `role` names the place the statement stands in, taken from the AST node
    // the caller is transforming -- "condition 2 of the comma list" and the
    // like. Empty where the surrounding form adds nothing to the message.
    std::string ScriptEngine::Impl::build_smart_call(const std::string& func_name, const std::vector<Janet>& args, const std::string& role) const
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

                    // Bound once each: a variable token resolves to a FRESH
                    // variable node per evaluation, so naming the ends twice
                    // would give the guard and the pattern different variables.
                    const std::string head = "(let [$pfrom " + from + " $pto " + to + "]";
                    const std::string body = " (do (zelph/path-guard $pfrom $pto) (zelph/path (zelph/fact $pfrom \"";
                    return head + body + string::escape_atom(base) + "\" $pto) \"" + mode + "\")))";
                }

                std::string call = "(zelph/fact " + transform_arg(args[0]) + " \"" + string::escape_atom(base) + "\"";
                for (size_t i = 2; i < args.size(); ++i)
                    call += " " + transform_arg(args[i]);
                call += ")";
                return "(zelph/path " + call + " \"" + mode + "\")";
            }
        }

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
                arg_codes.push_back(transform_arg(data[1]));
            }
            else
            {
                arg_codes.push_back(transform_arg(args[i]));
            }
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
            let_block += "_ (" + func_name;
            for (size_t i = 0; i < arg_codes.size(); ++i)
            {
                let_block += " $" + std::to_string(i);
            }
            let_block += ")] $" + std::to_string(focused_index) + ")";
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
                    cond_codes.push_back(build_smart_call("zelph/fact", args, "condition " + std::to_string(i) + " of the comma list"));
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
            // Desugars ¬X to (zelph/negate X)
            // which tags the pattern node with core.Negation and returns it.
            //
            // "¬¬(F)" tags the SAME pattern node twice -- the tag is a fact
            // ABOUT the node -- so the second operator was a no-op and the
            // statement silently meant "¬(F)". That is the opposite of what
            // it says: a double negation is the plain condition, and the
            // rule fired exactly when it should not have. Reading it that
            // way needs a second negation stratum, which is the parked
            // feature "¬(A, B)" is refused for; until then the operator says
            // so rather than dropping one of itself.
            if (is_negation_ast(data[1]))
            {
                throw std::runtime_error(
                    "\"¬\" does not nest: \"¬¬(F)\" would tag the same pattern twice "
                    "and mean \"¬(F)\", not \"F\". Write the plain pattern for the "
                    "positive condition.");
            }

            return "(zelph/negate " + transform_arg(data[1]) + ")";
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
            return "(zelph/approx " + transform_arg(data[2])
                 + " \"" + string::replace_all_copy(net, "\"", "\\\"") + "\")";
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
                return "(let [$sf " + transform_arg(data[2])
                     + "] (do (zelph/path-guard $sf $sf) (zelph/path (zelph/fact $sf \""
                     + string::escape_atom(base) + "\" $sf) \"" + mode + "\")))";

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
