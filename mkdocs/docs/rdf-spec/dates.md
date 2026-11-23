# Dates and Intervals

A date seldom names an instant. "1990" at the precision of a year refers to some day within that year, and thus is neither before nor after 1 May 1990. This page specifies what a date or time value expands to: the interval of the days, or of the seconds, that it may represent, expressed using the numbers from the standard library, so that numerical comparisons are the comparisons of dates. Nothing on this page has been implemented yet; a sentence regarding the present engine says "today", and the transcripts outside a box are actual executions of the present engine on expansion statements written by hand.

## The Model

**No date type.** zelph gets no kind of node for dates and possesses no intrinsic comparison for them. A date value is a [value node](literals.md) just like every literal, and its [expansion](expansion.md) comprises numbers from the standard library. Any mechanism that compares two numbers also compares two dates: the rules of `cmp` with outcomes `<`, `>` and `==`, and the engine’s [numeric comparisons](../logic.md#numeric-comparisons) such as `zelph:lt`.

**Days are counted.** A day is named by its Julian Day Number: the count of days elapsed since 1 January 4713 BCE according to the Julian calendar, which is designated as day 0. The count remains independent of the calendar in which a date is expressed: 23 April 1616 in the Julian calendar and 3 May 1616 in the Gregorian calendar correspond to the same day, represented as 2311415. It is positive for every day following day 0; any prior day is a negative integer in `integer-arithmetic`, written `(neg zint N)`.

**A value is an interval.** The expansion of a date value gives its first and its last bound, expressed in a single term:

!!! example "Specified – not yet implemented"

    ```
    "1990"^^xsd:gYear              zelph:interval  (&2447893 interval &2448257)
    "1952-03-11"^^xsd:date         zelph:interval  (&2434083 interval &2434083)
    "2020-02-29T23:30:00+02:00"^^xsd:dateTime
                                   zelph:interval  ((&2458909 at &77400) interval (&2458909 at &77400))
    ```

The year 1990 runs from day 2447893, marking the first day of January, to day 2448257, corresponding to the 31st day of December. A date at day precision is an interval lasting one day.

**A bound is a day or an instant.** When a value conveys no information beneath the day level, both bounds are day numbers. When it carries a time of day, both bounds are instants: `(D at S)` is second `S` of day `D`, where `S` ranges from 0 to 86399.

- The time zone offset is applied: the instant is converted to UTC, and the day and second correspond to those of the UTC instant. `23:30:00+02:00` on 29 February 2020 is second 77400 of that day.
- A value lacking an offset is read as UTC.
- `24:00:00`, which XSD allows, is treated as second 0 of the next day.
- A fraction of a second is not kept: the bounds are aligned to whole seconds, so a value containing a fraction expands to the second that contains it.

Day numbers and instants are compared with each other in the straightforward manner: as a first bound a day `D` stands for `(D at &0)`, as a last bound for `(D at &86399)`.

The term constructors `interval` and `at` are names within the language `zelph`, beside `neg` and `zint`. They are not predicates of facts that carry meaning in isolation, and they are registered in the same manner as those, so that a one-day interval, with its two bounds being one node, does not print in the shortened form of a statement about itself.

## The XSD Types

The `calendar` module registers an [expander](expansion.md#expanders) for each of the following datatypes. Each initially verifies the lexical space of its type in full; an ill-typed literal, such as `"1990-02-30"^^xsd:date`, undergoes no expansion. XSD dates represent dates in the proleptic Gregorian calendar, and XSD 1.1, which RDF 1.1 employs, designates the year preceding year 1 as year 0: `0000` corresponds to 1 BCE and `-0001` corresponds to 2 BCE.

| Datatype | Without an offset | With an offset |
|---|---|---|
| `xsd:dateTime`, `xsd:dateTimeStamp` | the second, as an instant read as UTC | the second, as an instant |
| `xsd:date` | the day: two day numbers | the full 24 hours of that local day, as instants |
| `xsd:gYearMonth` | the first to the last day of the month | the identical span, represented as instants |
| `xsd:gYear` | the first to the last day of the year | the same span, expressed as instants |

A full day featuring a non-zero offset covers two UTC days: `"2020-01-01+05:00"^^xsd:date` runs from `(&2458849 at &68400)`, 19:00 UTC on the preceding day, to `(&2458850 at &68399)`. An offset of zero, denoted by `Z`, gives day numbers, just as no offset does.

### Types Without an Interval

`xsd:time`, `xsd:gMonth`, `xsd:gDay`, and `xsd:gMonthDay` name something that recurs daily, monthly or annually. They possess no day interval and get no expansion. The duration types are left to a later module.

## Day Numbers

A day number is computed from year, month, and day using a single formula per calendar. Divisions are rounded toward minus infinity, and the year count includes a year 0; with these conventions, the formulas apply to every year, regardless of how far in the past.

Using `a = (14 - month) / 12`, `y = year + 4800 - a` and `m = month + 12·a - 3`:

- Gregorian: `day + (153·m + 2) / 5 + 365·y + y/4 - y/100 + y/400 - 32045`
- Julian: `day + (153·m + 2) / 5 + 365·y + y/4 - 32083`

An expander performs calculations in the host language using integers that do not overflow and builds the result with `zelph/int`; a day preceding day 0 appears as `(neg zint N)`. The process constitutes input conversion, just as the reading of a numeral’s digits does; no rule is involved, and no fact other than the expansion statement is written.

## Wikibase’s Precisions {#wikibase-precisions}

Wikibase, the system underpinning Wikidata, stores a point in time as a timestamp featuring a **precision** ranging from 0 to 14 and a **calendar model**. The precision indicates the extent of the timestamp’s intended detail; Wikidata exclusively accepts precisions between 0 and 11. The table gives the interval corresponding to each precision level. The year `Y` denotes the year of the timestamp within the numbering scheme employed by Wikibase’s data model, which lacks a year 0: 1 BCE corresponds to year −1.

| Precision | Unit | Interval |
|---|---|---|
| 14 | second | the second, as an instant |
| 13 | minute | the instants from second 0 to second 59 within the minute |
| 12 | hour | the instants from second 0 to second 3599 within the hour |
| 11 | day | the day |
| 10 | month | the first day to the last day of the month |
| 9 | year | from 1 January to 31 December in year `Y` |
| 8 | decade | the years `d` through `d + 9`, where `d` is `Y` with its final digit replaced by 0; the first decade spans years 1 to 9 |
| 7 | century | the years from `100·c - 99` to `100·c`, where `c` is `Y / 100` rounded up |
| 6 | millennium | the years from `1000·m - 999` to `1000·m`, where `m` is `Y / 1000` rounded up |
| 5 to 0 | 10⁴ to 10⁹ years | using the unit `u` and `n` = `Y / u` rounded to the nearest integer, with halves rounded away from zero: the years from `n·u - u/2` to `n·u + u/2 - 1` |

For a year preceding the common era, the table is mirrored: it is applied to the year’s magnitude excluding its sign, and the two bounds are swapped. The first century BCE runs from 100 BCE to 1 BCE. For the precisions 5 to 0, `n` = 0 gives the years from `u/2 - 1` BCE to `u/2 - 1` CE.

The centuries and millennia are defined by Wikibase: they end with the year that is a multiple of the unit, meaning the twentieth century spans from 1901 to 2000. A year range becomes days via the value’s calendar model: its first day is 1 January of the first year and its last day 31 December of the last year, both expressed in the calendar the value names, proleptic Gregorian (`Q1985727`) or proleptic Julian (`Q1985786`). At day and month precision, the days correspond to those of that same calendar.

Three instances, each calculated using the formulas presented above:

| Value | Interval | Day numbers |
|---|---|---|
| 1900, precision 7, Gregorian | From 1 January 1801 to 31 December 1900 | 2378862 to 2415385 |
| 100 BCE, precision 7, Julian | From 1 January 100 BCE to 31 December 1 BCE | 1684899 to 1721423 |
| 13,800,000,000 BCE, precision 0, Gregorian | From 14,499,999,999 BCE to 13,500,000,000 BCE | −5296014528209 to −4930772028210 |

Some values lack **an interval**: a timestamp featuring year 0, which the count excluding year 0 does not include; a calendar model distinct from the two standard ones; a timestamp that fails to represent a valid date at the given precision. Wikibase’s fields `before` and `after` are not utilized, and its `timezone` field is not applied: Wikidata documents the times of its values as without meaning below the day level.

One caution belongs to the data rather than to the table. A century value whose timestamp ends in `00`, like 1900 at precision 7, means the nineteenth century according to Wikibase’s definition, whereas an editor who typed "1900s" might have meant the twentieth; roughly half of the century values in a sample of the dump of 9 March 2026 exhibited this form (estimated on 4 November 2026). An interval is exactly as good as the precision an editor chose.

How these values are received, as literals that Wikidata’s RDF export has already adjusted according to their precision, and how the expander for Wikibase’s full values reads them, is detailed in [Wikidata under the Mapping](wikidata-mapping.md). The table in question applies equally to both routes.

## The Three Relations

Two intervals stand in exactly one of three relations:

| Relation | Condition | For dates |
|---|---|---|
| certainly before | the last bound of A is less than the first bound of B | each day A may refer to comes prior to each day B may refer to |
| certainly after | the first bound of A is greater than the last bound of B | the reverse |
| overlapping | neither | they might refer to the same day |

The three mutually exclude one another, and one of them always remains valid. When the bounds are day numbers, "overlapping" means "possibly the same day"; when they are instants, "possibly the same second".

For bounds that are day numbers – covering every value in Wikidata – each relation consists of one comparison between numbers, or of two for "overlapping", and a rule writes them using the engine’s numeric comparisons. The session below is a genuine execution on today’s engine. The three expansion statements are manually written, as an expander will do; `y1990`, `may1990`, and `d1989-12-31` stand for value nodes.

```
zelph> .import decimal-arithmetic
zelph+> .import integer-arithmetic
zelph> %(zelph/no-selffact-sugar "interval")
zelph> y1990 zelph:interval (&2447893 interval &2448257)
zelph> may1990 zelph:interval (&2448013 interval &2448043)
zelph> d1989-12-31 zelph:interval (&2447892 interval &2447892)
zelph> (A zelph:interval (F interval L), B zelph:interval (G interval M), L zelph:lt G) => (A before B)
...
zelph> (A zelph:interval (F interval L), B zelph:interval (G interval M), F zelph:gt M) => (A after B)
...
zelph> (A zelph:interval (F interval L), B zelph:interval (G interval M), A != B, L zelph:ge G, F zelph:le M) => (A overlaps B)
...
zelph> A before B
Answer: d1989-12-31 before y1990
Answer: d1989-12-31 before may1990
zelph> A after B
Answer: may1990 after d1989-12-31
Answer: y1990 after d1989-12-31
zelph> A overlaps B
Answer: may1990 overlaps y1990
Answer: y1990 overlaps may1990
```

The year 1990 and May 1990 overlap: neither precedes the other, even though the truthy literals Wikidata writes for them, `1990-01-01` and `1990-05-01`, would establish a sequence.

A comparison condition derives nothing and leaves no trace of how the verdict was reached beyond `[compared]` in an explanation. When the derivation of each comparison is wanted in the graph, the same relations are written using the rules of the standard library: a rule requests the comparison by deriving the fact `L cmp G`, and another reads the result that `cmp` derives.

```
zelph> .import decimal-arithmetic
zelph+> .import integer-arithmetic
zelph> %(zelph/no-selffact-sugar "interval")
zelph> y1990 zelph:interval (&2447893 interval &2448257)
zelph> d1991-01-01 zelph:interval (&2448258 interval &2448258)
zelph> (A zelph:interval (F interval L), B zelph:interval (G interval M), A != B) => (L cmp G)
zelph+> (A zelph:interval (F interval L), B zelph:interval (G interval M), L < G) => (A before B)
(y1990 before d1991-01-01) ⇐ {(&2448257 < &2448258) (y1990 zelph:interval (&2447893 interval &2448257)) (d1991-01-01 zelph:interval (&2448258 interval &2448258))}
zelph> .explain y1990 before d1991-01-01
y1990 before d1991-01-01
   ├─ y1990 zelph:interval (&2447893 interval &2448257)  [axiom]
   ├─ d1991-01-01 zelph:interval (&2448258 interval &2448258)  [axiom]
   └─ &2448257 < &2448258
      ├─ (&2448257 lcmp &2448258) res lt
      │  ├─ (7 dcmp 8) res lt  [axiom]
...
```

The explanation continues down to the digit tables.

The two other relations read the same comparisons. "Certainly after" is the `<` fact arising from the request made for the two values in the reverse order, and "overlapping" holds when neither of the two results of `cmp` is `lt`:

```
zelph> .import decimal-arithmetic
zelph+> .import integer-arithmetic
zelph> %(zelph/no-selffact-sugar "interval")
zelph> y1990 zelph:interval (&2447893 interval &2448257)
zelph> may1990 zelph:interval (&2448013 interval &2448043)
zelph> d1989-12-31 zelph:interval (&2447892 interval &2447892)
zelph> (A zelph:interval (F interval L), B zelph:interval (G interval M), A != B) => (L cmp G)
zelph+> (A zelph:interval (F interval L), B zelph:interval (G interval M), L < G) => (A before B)
...
zelph> (A zelph:interval (F interval L), B zelph:interval (G interval M), M < F) => (A after B)
...
zelph> (A zelph:interval (F interval L), B zelph:interval (G interval M), A != B, (L cmp G) = R, R != lt, (M cmp F) = S, S != lt) => (A overlaps B)
...
zelph> A before B
Answer: d1989-12-31 before y1990
Answer: d1989-12-31 before may1990
zelph> A after B
Answer: y1990 after d1989-12-31
Answer: may1990 after d1989-12-31
zelph> A overlaps B
Answer: may1990 overlaps y1990
Answer: y1990 overlaps may1990
```

Each such comparison leaves facts in the graph, so a check across many pairs uses the condition and a proof that must show its arithmetic uses `cmp`; whenever both give a verdict, it is identical ([Numbers](numbers.md#comparing-numbers)).

**The module `intervals`** holds the three relations once, for each type of bound: day numbers, instants, and the numeric bounds of quantities ([Wikidata](wikidata-mapping.md)). It adheres to the pattern of `cmp`: the fact `I icmp J` over two interval terms asks, and the module derives one of `I ilt J`, `I igt J`, and `I iov J`, along with the matching `(I icmp J) = lt`, `gt` or `ov`. When comparing two instants, it compares the days first and examines the seconds only where the days are equal, and it reads a day number adjacent to an instant as previously specified. Its rules rely on the numeric comparisons, so asking incurs the cost of the request and its answer, with no additional cost per digit.

The comparison operators in SPARQL carry these interpretations when applied to date values: `<` means certainly before, `>` means certainly after, and `=` indicates overlapping ([Querying Terms](querying.md#dates-in-sparql)).

## What an Interval Does Not Say

- **A truthy literal of Wikidata is not its value.** Wikidata’s RDF writes a date as an `xsd:dateTime`, except for the few values that its cleaner gives up on and that arrive as plain strings, and the year 1990 arrives as `1990-01-01T00:00:00Z`. The expander for `xsd:dateTime` can only read the literal’s content: one second. The interval associated with a Wikidata date comes from its full value, which carries the precision ([Wikidata](wikidata-mapping.md)); a network constructed solely from truthy triples lacks intervals for Wikidata’s dates, and comparing such literals is wrong below day precision.
- **An interval is exact, and the data need not be.** "Certainly before" is certain with respect to the stored values. A date that an editor marked as approximate, or entered as 1 January due to lack of a more accurate one, is equally incorrect in its interval as in its text. The [checks](checks.md) skip what the data marks as doubtful.
- **Comparing costs.** The magnitude of a date’s expansion, and the cost of a comparison each way, is detailed in [Costs](costs.md). Writing each bound as two smaller numbers, enabling `cmp` to reuse more of its computations, remains an [open decision](open-decisions.md#day-numbers-in-two-parts).
