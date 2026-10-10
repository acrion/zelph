# 4 · Differentiation

*Prerequisites: [3 · Terms and Rewriting](tutorial-terms.md).*

Differentiation in zelph is not a function that walks a syntax tree. It is
a set of forward-chaining rules, and a derivative is an ordinary derived
fact — which means it persists, carries its justification, and can be fed
straight back in.

## The basics

```
zelph> .import math
zelph+> <x> ~ polyring
(:needsring <x>) ⇐ (<x> ~ polyring)
zelph+> c ~ symconst
zelph> ? $( x*x ) diffby x
Answer: ((x * x) diffby x) = (x + x)
zelph+> ? $( x + c ) diffby x
Answer: ((x + c) diffby x) = &1
zelph+> ? $( x^5 ) diffby x
Answer: ((x ^ &5) diffby x) = $( &5 * x ^ &4 )
zelph+> ? $( x^3 - 2*x + 7 ) diffby x
Answer: ($( x ^ &3 - &2 * x + &7 ) diffby x) = $( &3 * x ^ &2 - &2 )
zelph+> ? $( exp(x*x) ) diffby x
Answer: ($( exp(x * x) ) diffby x) = $( exp(x * x) * (x + x) )
zelph+> ? $( ln(x) ) diffby x
Answer: ($( ln(x) ) diffby x) = (&1 / x)
```

Sum rule, product rule, power rule, chain rule. Notice that `d(x+c)/dx`
answers `&1` and not `&1 + &0`: the raw derivative is deliberately *not*
the exposed result. The connect stage asserts an ordinary `:simplify`
request on it, [the simplifier](tutorial-terms.md) answers that, and the
simplified form is what appears under `=`. One cross-module cascade, no
post-processing pass.

## Function derivatives are facts

`exp` and `ln` are not privileged. There is one fact and two rules behind
them, and the mechanism is open:

```
zelph+> sinh hasderivative cosh
zelph> cosh hasderivative sinh
zelph> ? $( sinh(x) ) diffby x
Answer: ($( sinh(x) ) diffby x) = $( cosh(x) )
zelph+> ? $( cosh(x*x) ) diffby x
Answer: ($( cosh(x * x) ) diffby x) = $( sinh(x * x) * (x + x) )
```

The chain rule came along for free, because it is stated once, generically,
over whatever `hasderivative` facts exist:

```
((F of U) dstate X, F hasderivative G, (U wrt X) deriv P)
=> (((F of U) wrt X) deriv ((G of U) * P))
```

`ln` needs a rule of its own only because its derivative `1/u` is not of
the form `g(u)` for any named symbol `g`.

## Constancy, and an honest boundary

"A term not containing x is constant with respect to x" is the textbook rule. zelph avoids expressing it in this negative form, since a negation holds validity only when the corresponding positive condition is complete: a rule "not contains x, hence constant" would erroneously classify every shape that the containment check cannot traverse as constant, solely because nothing could ever have derived its containment. Constancy is defined exclusively at the **leaves**, each established by a positive rule:

```
(X dstate X, X ~ symvar) => ((X wrt X) deriv &1)
(T dstate X, X ~ symvar, T ~ symvar, T != X)   => ((T wrt X) deriv &0)
(T dstate X, X ~ symvar, T ~ symconst, T != X) => ((T wrt X) deriv &0)
((A cons R) dstate X, X ~ symvar, (A cons R) isnumeral true)
=> (((A cons R) wrt X) deriv &0)
```

A constant composite needs no rule of its own. `c * c` is differentiated like any other product; its raw derivative is assembled from the `&0` of its leaves, and the simplifier reduces it to `&0`:

```
zelph+> ? $( c*c ) diffby x
Answer: ((c * c) diffby x) = &0
```

A list is recognized as a numeral solely if the following condition holds: the list is not empty, terminates with nil, and every individual element qualifies as a digit based on the loaded arithmetic module. This verification occurs only when requested, specifically for lists undergoing differentiation.

The effect is an honest boundary:

```
zelph+> ? (x / c) diffby x
zelph+> ? (x foo c) diffby x
zelph+> ? <x c> diffby x
zelph+>
```

Silence, not zero. The standard library lacks a quotient rule, thus `d(x/c)/dx` yields no result; a term built from a predicate zelph knows nothing about has none either; and `<x c>`, a list comprising two symbols, fails to be a numeric value. All three are honest. No rule applies to them, hence nothing can claim they are constant.

Adding the quotient rule involves a brief task aligned with the [operator extension protocol](tutorial-terms.md#extending-the-simplifier-instead): two decompose rules that pass the request on to the operands, and one assemble rule. No declaration of constancy is required – a constant numerator undergoes structural differentiation, just as `c * c` above.

## Higher and mixed derivatives

Since a derivative is an ordinary fact exposed under `=`, iterating is just
chaining. `diff.zph` provides that as a list walk:

```
zelph+> <x y> ~ polyring
(:needsring <x y>) ⇐ (<x y> ~ polyring)
(x pouter y) ⇐ (:needsring <x y>)
zelph+> ? $( x^5 ) diffalong <x x>
Answer: ((x ^ &5) diffalong <x x>) = $( &20 * x ^ &3 )
zelph+> ? $( x^3*y + x*y^2 ) diffalong <x y>
Answer: ($( x ^ &3 * y + x * y ^ &2 ) diffalong <x y>) = $( &3 * x ^ &2 + &2 * y )
```

The whole implementation is four rules:

```
(T diffalong nil) => ((T diffalong nil) = T)
(T diffalong (V cons R)) => (T diffby V)
(T diffalong (V cons R), (T diffby V) = D) => (D diffalong R)
(T diffalong (V cons R), (T diffby V) = D, (D diffalong R) = E)
=> ((T diffalong (V cons R)) = E)
```

A list, and not extra objects on the fact, for a reason worth knowing: a
zelph fact carries a **set** of objects, so `(T d x y)` and `(T d y x)`
would be the *same node*. Order has to live in the data.

## Clairaut's theorem, observed

Which makes the symmetry of mixed partials checkable rather than assumed:

```
zelph+> ? $( x^3*y + x*y^2 ) diffalong <x y>
Answer: … = $( &3 * x ^ &2 + &2 * y )
zelph> ? $( x^3*y + x*y^2 ) diffalong <y x>
Answer: … = $( &3 * x ^ &2 + &2 * y )
```

Those are two *different* request nodes — `<x y>` and `<y x>` are distinct
cons lists — reaching one and the same answer node. You can confirm the
node identity rather than trusting the printed text:

```
zelph+> %(let [t (zelph/fact (zelph/fact (zelph/fact "x" "^" (zelph/number "3")) "*" "y") "+" (zelph/fact "x" "*" (zelph/fact "y" "^" (zelph/number "2")))) a (zelph/fact t "diffalong" (zelph/list "x" "y")) b (zelph/fact t "diffalong" (zelph/list "y" "x"))] (string "SCHWARZ-distinct-requests-" (not (= a b)) "-same-answer-" (= (get (get (zelph/query (zelph/fact a "=" '_D)) 0) '_D) (get (get (zelph/query (zelph/fact b "=" '_D)) 0) '_D))))
"SCHWARZ-distinct-requests-true-same-answer-true"
```

Here the two orders meet at a single node since their simplified forms happen to align. This alignment need not happen in general, as the simplifier normalizes neither commutativity nor associativity: although the two orders yield the same value, they may produce distinct arrangements of identical terms, and deciding that such arrangements are equivalent falls within the responsibility of the polynomial layer:

```
zelph> ? $( (x*y)*(x+y) ) diffalong <x y>
Answer: ($( x * y * (x + y) ) diffalong <x y>) = $( x + y + y + x )
zelph+> ? $( (x*y)*(x+y) ) diffalong <y x>
Answer: ($( x * y * (x + y) ) diffalong <y x>) = $( x + y + x + y )
zelph+> ? $( x + y + y + x ) ≡ $( x + y + x + y )
Answer: ($( x + y + y + x ) ≡ $( x + y + x + y )) = proven
```

## The one associativity there is

Iterated differentiation assembles its constants in layers: the third
derivative of x⁵ arrives as 5·(4·(3·x²)), every factor correct and the
whole thing unreadable. So the simplifier carries exactly one associativity
rule — the case where a numeral meets a numeral one level down:

```
zelph+> ? $( x^5 ) diffalong <x x x>
Answer: ((x ^ &5) diffalong <x x x>) = $( &60 * x ^ &2 )
zelph+> ? $( x^6 ) diffalong <x x x x>
Answer: ((x ^ &6) diffalong <x x x x>) = $( &360 * x ^ &2 )
```

It is admitted as an exception, not as a relaxation of the discipline from
[tutorial 3](tutorial-terms.md#why-there-is-no-commutativity-rule): it is
measure-reducing (two products become one), its result is already a normal
form, and it does not fire in general — only when both factors are
numerals. A term like `$( y * (4*x) )` is left exactly as written.

The equality question still belongs to the polynomial layer:

```
zelph+> ? :topoly $( 60*x^2 )
Answer: (:topoly $( &60 * x ^ &2 )) = (x poly <(pos zint &0) (pos zint &0) (pos zint &60)>)
```

Coefficients ⟨0, 0, 60⟩. Two layers, two jobs: local rewriting for terms,
normal forms for equality.

## A capstone: Euler's homogeneous function theorem

If f is homogeneous of degree n, then x·∂f/∂x + y·∂f/∂y = n·f. Take
f = x³ + x²y + y³, homogeneous of degree 3. Differentiate:

```
zelph+> <x y> ~ polyring
zelph> ? $( x^3 + x^2*y + y^3 ) diffby x
Answer: … = $( &3 * x ^ &2 + &2 * x * y )
zelph+> ? $( x^3 + x^2*y + y^3 ) diffby y
Answer: … = $( x ^ &2 + &3 * y ^ &2 )
```

and assemble the claim:

```
zelph+> ? $( x*(3*x^2 + 2*x*y) + y*(x^2 + 3*y^2) ) ≡ $( 3*(x^3 + x^2*y + y^3) )
Answer: … = proven
```

Two layers cooperating: `diff` produced the partials as facts, `topoly`
proved the identity between them. Neither knows about the other — they
share a vocabulary of ordinary `+`, `*` and `^` facts, and that is the
entire interface.

You can also let a *rule* do the assembling, so the theorem is checked for
any f you declare:

```
zelph+> (F eulertest (X then Y)) => (F diffby X)
zelph> (F eulertest (X then Y)) => (F diffby Y)
zelph> (F eulertest (X then Y), (F diffby X) = P, (F diffby Y) = Q) => (((X * P) + (Y * Q)) ≡ (&3 * F))
```

Now `$( x^3 + x^2*y + y^3 ) eulertest (x then y)` states the identity to be
proven, and `topoly` proves it. Statements about statements, all the way
up.

## Exercises

1.  `sin hasderivative cos` is easy; the other half, however, is not, since d(cos u)/du = −sin(u) does not conform to the form g(u) for any named symbol `g`. Give `cos` a dedicated assemble rule, structured similarly to how `ln` is handled:

    ```
    ((cos of U) dstate X, (U wrt X) deriv P)
    => (((cos of U) wrt X) deriv ((neg of (sin of U)) * P))
    ```

    Next verify that `$( sin(x) ) diffalong <x x x x>` closes the cycle back to `sin(x)`, and that `$( cos(c) ) diffby x` answers `&0`. Why is it unnecessary for `cos` to have a rule that passes the request to `u`, or one that makes `cos(c)` constant?

2.  Verify Euler's theorem for a homogeneous polynomial of degree 4 in three
    variables.

3.  `? $( x^2 ) diffalong <x x x>` — predict the answer, then check it.

4.  Write the quotient rule in the form of an operator extension and confirm that `((ln of x) diffalong <x x>)` subsequently yields the correct result. Next, attempt `(c / x) diffby x`: the constant numerator requires no more than the three rules you wrote.

## Next

[5 · Inside the Normal Form](tutorial-polynomials.md) opens up the
representation that has been doing the proving all along.
