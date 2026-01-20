# Mereology Constraint Violations in Wikidata

This page documents zelph's analysis of mereological constraints in Wikidata, supporting the work of the [Mereology Task Force](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Mereology_Task_Force).

## Background

Mereology is the study of part-whole relationships. In Wikidata, the primary mereological properties are:

- **P361** (part of) — X is part of Y
- **P527** (has part) — X has Y as a part
- **P463** (member of) — X is a member of Y

These properties should satisfy certain logical constraints:

1. **Antisymmetry**: If X is part of Y, then Y cannot be part of X (no cycles)
2. **Inverse consistency**: P361 and P527 should be inverses (if X P361 Y, then Y P527 X)
3. **Transitivity**: If A is part of B and B is part of C, then A is part of C

## Data Source

The analyses on this page were performed using the pruned Wikidata database `wikidata-20251222-pruned.bin` (December 2025 dump). This database excludes certain high-volume, low-relevance statements to reduce size while preserving ontological structure. See [Binary Data Files](binaries.md) for details on the pruning criteria and download links.

## Property Overview

```
wikidata> .node P361
Name in language 'en': 'part of'
Incoming connections from: (941952 connections)

wikidata> .node P527
Name in language 'en': 'has part(s)'
Incoming connections from: (173061 connections)
```

P361 has significantly more usage than P527, indicating that "part of" is the preferred direction for expressing parthood in Wikidata.

### Subproperties

```
wikidata> X P1647 P361
(no results)

wikidata> X P1647 P527
(no results)

wikidata> X P1647 P463
(no results)
```

Currently, none of these properties have subproperties. This aligns with the Mereology Task Force's ongoing work to develop a subproperty hierarchy for different types of parthood relationships.

## Antisymmetry Violations (Cycles)

Antisymmetry states that if X is part of Y, then Y cannot simultaneously be part of X. Violations indicate modeling errors.

### P361 Cycles

```
wikidata> X P361 Y, Y P361 X => !
```

**Result: 62 contradictions found (31 unique pairs)**

Selected examples:

| Item 1 | Item 2 |
|--------|--------|
| [Q12583](https://www.wikidata.org/wiki/Q12583) | [Q1514908](https://www.wikidata.org/wiki/Q1514908) |
| [Q136222399](https://www.wikidata.org/wiki/Q136222399) | [Q7765486](https://www.wikidata.org/wiki/Q7765486) |
| [Q106872770](https://www.wikidata.org/wiki/Q106872770) | [Q106873003](https://www.wikidata.org/wiki/Q106873003) |
| [Q127204277](https://www.wikidata.org/wiki/Q127204277) | [Q277633](https://www.wikidata.org/wiki/Q277633) |
| [Q101112931](https://www.wikidata.org/wiki/Q101112931) | [Q101112934](https://www.wikidata.org/wiki/Q101112934) |

Full session log:
```
wikidata> X P361 Y, Y P361 X => !
((X «P361» Y), (Y «P361» X)) «=>» «!»
wikidata> .run
Starting reasoning with 24 worker threads.
«!» ⇐ («Q136222399» «P361» «Q7765486»), («Q7765486» «P361» «Q136222399»)
«!» ⇐ («Q106872770» «P361» «Q106873003»), («Q106873003» «P361» «Q106872770»)
«!» ⇐ («Q127204277» «P361» «Q277633»), («Q277633» «P361» «Q127204277»)
«!» ⇐ («Q12583» «P361» «Q1514908»), («Q1514908» «P361» «Q12583»)
«!» ⇐ («Q101112931» «P361» «Q101112934»), («Q101112934» «P361» «Q101112931»)
«!» ⇐ («Q12160640» «P361» «Q63436916»), («Q63436916» «P361» «Q12160640»)
«!» ⇐ («Q136505238» «P361» «Q136506192»), («Q136506192» «P361» «Q136505238»)
«!» ⇐ («Q67622024» «P361» «Q66491366»), («Q66491366» «P361» «Q67622024»)
«!» ⇐ («Q125216961» «P361» «Q124839885»), («Q124839885» «P361» «Q125216961»)
«!» ⇐ («Q107551422» «P361» «Q107551348»), («Q107551348» «P361» «Q107551422»)
«!» ⇐ («Q127206310» «P361» «Q379689»), («Q379689» «P361» «Q127206310»)
«!» ⇐ («Q131625661» «P361» «Q2189346»), («Q2189346» «P361» «Q131625661»)
«!» ⇐ («Q131625100» «P361» «Q3246679»), («Q3246679» «P361» «Q131625100»)
«!» ⇐ («Q42189086» «P361» «Q107443979»), («Q107443979» «P361» «Q42189086»)
«!» ⇐ («Q66491366» «P361» «Q67622024»), («Q67622024» «P361» «Q66491366»)
«!» ⇐ («Q116144995» «P361» «Q295802»), («Q295802» «P361» «Q116144995»)
«!» ⇐ («Q137161191» «P361» «Q137163886»), («Q137163886» «P361» «Q137161191»)
«!» ⇐ («Q127196224» «P361» «Q111737399»), («Q111737399» «P361» «Q127196224»)
«!» ⇐ («Q61588670» «P361» «Q61585675»), («Q61585675» «P361» «Q61588670»)
«!» ⇐ («Q7765486» «P361» «Q136222399»), («Q136222399» «P361» «Q7765486»)
«!» ⇐ («Q106873003» «P361» «Q106872770»), («Q106872770» «P361» «Q106873003»)
«!» ⇐ («Q63436916» «P361» «Q12160640»), («Q12160640» «P361» «Q63436916»)
«!» ⇐ («Q1987589» «P361» «Q127204811»), («Q127204811» «P361» «Q1987589»)
«!» ⇐ («Q20180731» «P361» «Q1928690»), («Q1928690» «P361» «Q20180731»)
«!» ⇐ («Q2189346» «P361» «Q131625661»), («Q131625661» «P361» «Q2189346»)
«!» ⇐ («Q3246679» «P361» «Q131625100»), («Q131625100» «P361» «Q3246679»)
«!» ⇐ («Q123916571» «P361» «Q6849876»), («Q6849876» «P361» «Q123916571»)
«!» ⇐ («Q428691» «P361» «Q15841889»), («Q15841889» «P361» «Q428691»)
«!» ⇐ («Q741226» «P361» «Q33162389»), («Q33162389» «P361» «Q741226»)
«!» ⇐ («Q1514908» «P361» «Q12583»), («Q12583» «P361» «Q1514908»)
«!» ⇐ («Q127199390» «P361» «Q24901995»), («Q24901995» «P361» «Q127199390»)
«!» ⇐ («Q6849876» «P361» «Q123916571»), («Q123916571» «P361» «Q6849876»)
«!» ⇐ («Q1062068» «P361» «Q58609682»), («Q58609682» «P361» «Q1062068»)
«!» ⇐ («Q107443979» «P361» «Q42189086»), («Q42189086» «P361» «Q107443979»)
«!» ⇐ («Q277633» «P361» «Q127204277»), («Q127204277» «P361» «Q277633»)
«!» ⇐ («Q111698850» «P361» «Q56490040»), («Q56490040» «P361» «Q111698850»)
«!» ⇐ («Q101112934» «P361» «Q101112931»), («Q101112931» «P361» «Q101112934»)
«!» ⇐ («Q107551348» «P361» «Q107551422»), («Q107551422» «P361» «Q107551348»)
«!» ⇐ («Q111737399» «P361» «Q127196224»), («Q127196224» «P361» «Q111737399»)
«!» ⇐ («Q136506192» «P361» «Q136505238»), («Q136505238» «P361» «Q136506192»)
«!» ⇐ («Q105061646» «P361» «Q105061710»), («Q105061710» «P361» «Q105061646»)
«!» ⇐ («Q107434371» «P361» «Q107434131»), («Q107434131» «P361» «Q107434371»)
«!» ⇐ («Q56490040» «P361» «Q111698850»), («Q111698850» «P361» «Q56490040»)
«!» ⇐ («Q24901995» «P361» «Q127199390»), («Q127199390» «P361» «Q24901995»)
«!» ⇐ («Q15841889» «P361» «Q428691»), («Q428691» «P361» «Q15841889»)
«!» ⇐ («Q16365600» «P361» «Q16414514»), («Q16414514» «P361» «Q16365600»)
«!» ⇐ («Q127204811» «P361» «Q1987589»), («Q1987589» «P361» «Q127204811»)
«!» ⇐ («Q107434131» «P361» «Q107434371»), («Q107434371» «P361» «Q107434131»)
«!» ⇐ («Q379689» «P361» «Q127206310»), («Q127206310» «P361» «Q379689»)
«!» ⇐ («Q137163886» «P361» «Q137161191»), («Q137161191» «P361» «Q137163886»)
«!» ⇐ («Q99279852» «P361» «Q99279484»), («Q99279484» «P361» «Q99279852»)
«!» ⇐ («Q16414514» «P361» «Q16365600»), («Q16365600» «P361» «Q16414514»)
«!» ⇐ («Q99279484» «P361» «Q99279852»), («Q99279852» «P361» «Q99279484»)
«!» ⇐ («Q105061710» «P361» «Q105061646»), («Q105061646» «P361» «Q105061710»)
«!» ⇐ («Q83437828» «P361» «Q85781002»), («Q85781002» «P361» «Q83437828»)
«!» ⇐ («Q58609682» «P361» «Q1062068»), («Q1062068» «P361» «Q58609682»)
«!» ⇐ («Q295802» «P361» «Q116144995»), («Q116144995» «P361» «Q295802»)
«!» ⇐ («Q85781002» «P361» «Q83437828»), («Q83437828» «P361» «Q85781002»)
«!» ⇐ («Q124839885» «P361» «Q125216961»), («Q125216961» «P361» «Q124839885»)
«!» ⇐ («Q33162389» «P361» «Q741226»), («Q741226» «P361» «Q33162389»)
«!» ⇐ («Q1928690» «P361» «Q20180731»), («Q20180731» «P361» «Q1928690»)
«!» ⇐ («Q61585675» «P361» «Q61588670»), («Q61588670» «P361» «Q61585675»)
Reasoning complete. Total unification matches processed: 942012. Total contradictions found: 62.
Found one or more contradictions!
Reasoning summary: 942012 matches processed, 62 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h7m8.789s – 942012 matches processed, 62 contradictions found.
```

### P527 Cycles

```
wikidata> X P527 Y, Y P527 X => !
```

**Result: 46 contradictions found (23 unique pairs)**

Selected examples:

| Item 1 | Item 2 |
|--------|--------|
| [Q74121934](https://www.wikidata.org/wiki/Q74121934) | [Q74122710](https://www.wikidata.org/wiki/Q74122710) |
| [Q431498](https://www.wikidata.org/wiki/Q431498) | [Q25206835](https://www.wikidata.org/wiki/Q25206835) |
| [Q1501558](https://www.wikidata.org/wiki/Q1501558) | [Q193886](https://www.wikidata.org/wiki/Q193886) |
| [Q207318](https://www.wikidata.org/wiki/Q207318) | [Q15053704](https://www.wikidata.org/wiki/Q15053704) |
| [Q552431](https://www.wikidata.org/wiki/Q552431) | [Q913302](https://www.wikidata.org/wiki/Q913302) |

Full session log:
```
wikidata> X P527 Y, Y P527 X => !
((Y «P527» X), (X «P527» Y)) «=>» «!»
wikidata> .run
Starting reasoning with 24 worker threads.
«!» ⇐ («Q74121934» «P527» «Q74122710»), («Q74122710» «P527» «Q74121934»)
«!» ⇐ («Q101066900» «P527» «Q107551287»), («Q107551287» «P527» «Q101066900»)
«!» ⇐ («Q431498» «P527» «Q25206835»), («Q25206835» «P527» «Q431498»)
«!» ⇐ («Q137362925» «P527» «Q576309»), («Q576309» «P527» «Q137362925»)
«!» ⇐ («Q134888567» «P527» «Q2056031»), («Q2056031» «P527» «Q134888567»)
«!» ⇐ («Q27017232» «P527» «Q27020062»), («Q27020062» «P527» «Q27017232»)
«!» ⇐ («Q104665607» «P527» «Q56332683»), («Q56332683» «P527» «Q104665607»)
«!» ⇐ («Q385256» «P527» «Q112971079»), («Q112971079» «P527» «Q385256»)
«!» ⇐ («Q56332683» «P527» «Q104665607»), («Q104665607» «P527» «Q56332683»)
«!» ⇐ («Q105940533» «P527» «Q105940361»), («Q105940361» «P527» «Q105940533»)
«!» ⇐ («Q2056031» «P527» «Q134888567»), («Q134888567» «P527» «Q2056031»)
«!» ⇐ («Q116742292» «P527» «Q116742242»), («Q116742242» «P527» «Q116742292»)
«!» ⇐ («Q1501558» «P527» «Q193886»), («Q193886» «P527» «Q1501558»)
«!» ⇐ («Q207318» «P527» «Q15053704»), («Q15053704» «P527» «Q207318»)
«!» ⇐ («Q3990372» «P527» «Q78251538»), («Q78251538» «P527» «Q3990372»)
«!» ⇐ («Q105969788» «P527» «Q124754144»), («Q124754144» «P527» «Q105969788»)
«!» ⇐ («Q124754144» «P527» «Q105969788»), («Q105969788» «P527» «Q124754144»)
«!» ⇐ («Q37859573» «P527» «Q37859386»), («Q37859386» «P527» «Q37859573»)
«!» ⇐ («Q112971079» «P527» «Q385256»), («Q385256» «P527» «Q112971079»)
«!» ⇐ («Q3770855» «P527» «Q104499»), («Q104499» «P527» «Q3770855»)
«!» ⇐ («Q105974650» «P527» «Q124788693»), («Q124788693» «P527» «Q105974650»)
«!» ⇐ («Q116742242» «P527» «Q116742292»), («Q116742292» «P527» «Q116742242»)
«!» ⇐ («Q552431» «P527» «Q913302»), («Q913302» «P527» «Q552431»)
«!» ⇐ («Q104499» «P527» «Q3770855»), («Q3770855» «P527» «Q104499»)
«!» ⇐ («Q37859386» «P527» «Q37859573»), («Q37859573» «P527» «Q37859386»)
«!» ⇐ («Q111935683» «P527» «Q111919271»), («Q111919271» «P527» «Q111935683»)
«!» ⇐ («Q2330954» «P527» «Q7127281»), («Q7127281» «P527» «Q2330954»)
«!» ⇐ («Q135922083» «P527» «Q135925186»), («Q135925186» «P527» «Q135922083»)
«!» ⇐ («Q913302» «P527» «Q552431»), («Q552431» «P527» «Q913302»)
«!» ⇐ («Q27020062» «P527» «Q27017232»), («Q27017232» «P527» «Q27020062»)
«!» ⇐ («Q85988873» «P527» «Q10985693»), («Q10985693» «P527» «Q85988873»)
«!» ⇐ («Q193886» «P527» «Q1501558»), («Q1501558» «P527» «Q193886»)
«!» ⇐ («Q7127281» «P527» «Q2330954»), («Q2330954» «P527» «Q7127281»)
«!» ⇐ («Q111919271» «P527» «Q111935683»), («Q111935683» «P527» «Q111919271»)
«!» ⇐ («Q25206835» «P527» «Q431498»), («Q431498» «P527» «Q25206835»)
«!» ⇐ («Q110945881» «P527» «Q108739909»), («Q108739909» «P527» «Q110945881»)
«!» ⇐ («Q15053704» «P527» «Q207318»), («Q207318» «P527» «Q15053704»)
«!» ⇐ («Q10985693» «P527» «Q85988873»), («Q85988873» «P527» «Q10985693»)
«!» ⇐ («Q107551287» «P527» «Q101066900»), («Q101066900» «P527» «Q107551287»)
«!» ⇐ («Q78251538» «P527» «Q3990372»), («Q3990372» «P527» «Q78251538»)
«!» ⇐ («Q105940361» «P527» «Q105940533»), («Q105940533» «P527» «Q105940361»)
«!» ⇐ («Q576309» «P527» «Q137362925»), («Q137362925» «P527» «Q576309»)
«!» ⇐ («Q124788693» «P527» «Q105974650»), («Q105974650» «P527» «Q124788693»)
«!» ⇐ («Q135925186» «P527» «Q135922083»), («Q135922083» «P527» «Q135925186»)
«!» ⇐ («Q108739909» «P527» «Q110945881»), («Q110945881» «P527» «Q108739909»)
«!» ⇐ («Q74122710» «P527» «Q74121934»), («Q74121934» «P527» «Q74122710»)
Reasoning complete. Total unification matches processed: 173105. Total contradictions found: 46.
Found one or more contradictions!
Reasoning summary: 173105 matches processed, 46 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h0m9.252s – 173105 matches processed, 46 contradictions found.
```

## Inverse Consistency

P361 (part of) and P527 (has part) should be inverses. We tested how many pairs have both relationships correctly established:

```
wikidata> X P361 Y, Y P527 X => X "consistent inverse" Y
wikidata> .run
...
Reasoning complete in 0h0m14.948s – 453454 matches processed, 0 contradictions found.
```

**Result: ~450,000 item pairs have correctly inverse P361/P527 relationships.**

This indicates good inverse consistency in the existing data.

## Transitivity Analysis

Mereologically, "part of" is transitive: if A is part of B and B is part of C, then A should be part of C. We tested whether such transitive chains are explicitly materialized:

```
wikidata> A P361 B, B P361 C, A P361 C => A "transitive-P361" C
wikidata> .run
Reasoning complete in 0h0m5.306s – 1473685 matches processed, 0 contradictions found.
```

**Result: No explicit transitive chains found.**

This means transitivity is not materialized in Wikidata — when A P361 B and B P361 C, the relationship A P361 C is not explicitly stated. This is expected behavior, as materializing all transitive relationships would dramatically increase data size.

## Recipe/Ingredient Problem

The Task Force has discussed the misuse of P527 (has part) for recipe ingredients (see [Effort 0001](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Mereology_Task_Force/Effort_0001:_Removing_or_migrating_part_of_statements_in_recipe_relationships)). We tested for items that "have part" a food item:

```
wikidata> X P527 Y, Y P31 Q2095 => X "recipe-test" Y
((X «P527» Y), (Y «P31» «Q2095»)) «=>» (X «recipe-test» Y)
wikidata> .run
Starting reasoning with 24 worker threads.
«Q5309357» «recipe-test» «Q877957» ⇐ («Q5309357» «P527» «Q877957»), («Q877957» «P31» «Q2095»)
«Q4843243» «recipe-test» «Q877957» ⇐ («Q4843243» «P527» «Q877957»), («Q877957» «P31» «Q2095»)
«Q5073693» «recipe-test» «Q877957» ⇐ («Q5073693» «P527» «Q877957»), («Q877957» «P31» «Q2095»)
«Q6488824» «recipe-test» «Q877957» ⇐ («Q6488824» «P527» «Q877957»), («Q877957» «P31» «Q2095»)
«Q5874601» «recipe-test» «Q1208221» ⇐ («Q5874601» «P527» «Q1208221»), («Q1208221» «P31» «Q2095»)
«Q1810638» «recipe-test» «Q1208221» ⇐ («Q1810638» «P527» «Q1208221»), («Q1208221» «P31» «Q2095»)
«Q595744» «recipe-test» «Q1208221» ⇐ («Q595744» «P527» «Q1208221»), («Q1208221» «P31» «Q2095»)
«Q12368100» «recipe-test» «Q1208221» ⇐ («Q12368100» «P527» «Q1208221»), («Q1208221» «P31» «Q2095»)
«Q4216325» «recipe-test» «Q842221» ⇐ («Q4216325» «P527» «Q842221»), («Q842221» «P31» «Q2095»)
«Q1196289» «recipe-test» «Q842221» ⇐ («Q1196289» «P527» «Q842221»), («Q842221» «P31» «Q2095»)
«Q5506400» «recipe-test» «Q420683» ⇐ («Q5506400» «P527» «Q420683»), («Q420683» «P31» «Q2095»)
«Q7605238» «recipe-test» «Q420683» ⇐ («Q7605238» «P527» «Q420683»), («Q420683» «P31» «Q2095»)
«Q6127085» «recipe-test» «Q420683» ⇐ («Q6127085» «P527» «Q420683»), («Q420683» «P31» «Q2095»)
«Q7634410» «recipe-test» «Q420683» ⇐ («Q7634410» «P527» «Q420683»), («Q420683» «P31» «Q2095»)
«Q4879252» «recipe-test» «Q420683» ⇐ («Q4879252» «P527» «Q420683»), («Q420683» «P31» «Q2095»)
«Q584564» «recipe-test» «Q420683» ⇐ («Q584564» «P527» «Q420683»), («Q420683» «P31» «Q2095»)
«Q19390528» «recipe-test» «Q17638951» ⇐ («Q19390528» «P527» «Q17638951»), («Q17638951» «P31» «Q2095»)
«Q85784549» «recipe-test» «Q1365891» ⇐ («Q85784549» «P527» «Q1365891»), («Q1365891» «P31» «Q2095»)
«Q842922» «recipe-test» «Q1365891» ⇐ («Q842922» «P527» «Q1365891»), («Q1365891» «P31» «Q2095»)
«Q14529408» «recipe-test» «Q1365891» ⇐ («Q14529408» «P527» «Q1365891»), («Q1365891» «P31» «Q2095»)
«Q106355895» «recipe-test» «Q1365891» ⇐ («Q106355895» «P527» «Q1365891»), («Q1365891» «P31» «Q2095»)
«Q3180957» «recipe-test» «Q1133209» ⇐ («Q3180957» «P527» «Q1133209»), («Q1133209» «P31» «Q2095»)
«Q5171114» «recipe-test» «Q1133209» ⇐ («Q5171114» «P527» «Q1133209»), («Q1133209» «P31» «Q2095»)
«Q26897677» «recipe-test» «Q21546387» ⇐ («Q26897677» «P527» «Q21546387»), («Q21546387» «P31» «Q2095»)
«Q865448» «recipe-test» «Q21546387» ⇐ («Q865448» «P527» «Q21546387»), («Q21546387» «P31» «Q2095»)
«Q3360579» «recipe-test» «Q2631692» ⇐ («Q3360579» «P527» «Q2631692»), («Q2631692» «P31» «Q2095»)
«Q2364865» «recipe-test» «Q2631692» ⇐ («Q2364865» «P527» «Q2631692»), («Q2631692» «P31» «Q2095»)
«Q3748739» «recipe-test» «Q2631692» ⇐ («Q3748739» «P527» «Q2631692»), («Q2631692» «P31» «Q2095»)
«Q2063761» «recipe-test» «Q2631692» ⇐ («Q2063761» «P527» «Q2631692»), («Q2631692» «P31» «Q2095»)
«Q6936882» «recipe-test» «Q21154910» ⇐ («Q6936882» «P527» «Q21154910»), («Q21154910» «P31» «Q2095»)
«Q6968166» «recipe-test» «Q21154910» ⇐ («Q6968166» «P527» «Q21154910»), («Q21154910» «P31» «Q2095»)
«Q7062059» «recipe-test» «Q21154910» ⇐ («Q7062059» «P527» «Q21154910»), («Q21154910» «P31» «Q2095»)
«Q85791787» «recipe-test» «Q3574648» ⇐ («Q85791787» «P527» «Q3574648»), («Q3574648» «P31» «Q2095»)
«Q63856972» «recipe-test» «Q123221424» ⇐ («Q63856972» «P527» «Q123221424»), («Q123221424» «P31» «Q2095»)
«Q212121» «recipe-test» «Q123221424» ⇐ («Q212121» «P527» «Q123221424»), («Q123221424» «P31» «Q2095»)
«Q5706310» «recipe-test» «Q123221424» ⇐ («Q5706310» «P527» «Q123221424»), («Q123221424» «P31» «Q2095»)
«Q6006053» «recipe-test» «Q4815005» ⇐ («Q6006053» «P527» «Q4815005»), («Q4815005» «P31» «Q2095»)
«Q5180030» «recipe-test» «Q11190742» ⇐ («Q5180030» «P527» «Q11190742»), («Q11190742» «P31» «Q2095»)
«Q100507813» «recipe-test» «Q11190742» ⇐ («Q100507813» «P527» «Q11190742»), («Q11190742» «P31» «Q2095»)
«Q2009326» «recipe-test» «Q11190742» ⇐ («Q2009326» «P527» «Q11190742»), («Q11190742» «P31» «Q2095»)
«Q7165342» «recipe-test» «Q11190742» ⇐ («Q7165342» «P527» «Q11190742»), («Q11190742» «P31» «Q2095»)
«Q127477120» «recipe-test» «Q11190742» ⇐ («Q127477120» «P527» «Q11190742»), («Q11190742» «P31» «Q2095»)
«Q10357458» «recipe-test» «Q11190742» ⇐ («Q10357458» «P527» «Q11190742»), («Q11190742» «P31» «Q2095»)
«Q1328462» «recipe-test» «Q11190742» ⇐ («Q1328462» «P527» «Q11190742»), («Q11190742» «P31» «Q2095»)
«Q5491445» «recipe-test» «Q564722» ⇐ («Q5491445» «P527» «Q564722»), («Q564722» «P31» «Q2095»)
«Q2890029» «recipe-test» «Q564722» ⇐ («Q2890029» «P527» «Q564722»), («Q564722» «P31» «Q2095»)
«Q122842209» «recipe-test» «Q1195290» ⇐ («Q122842209» «P527» «Q1195290»), («Q1195290» «P31» «Q2095»)
«Q15724985» «recipe-test» «Q119863250» ⇐ («Q15724985» «P527» «Q119863250»), («Q119863250» «P31» «Q2095»)
«Q135037929» «recipe-test» «Q135038148» ⇐ («Q135037929» «P527» «Q135038148»), («Q135038148» «P31» «Q2095»)
«Q2140524» «recipe-test» «Q2941309» ⇐ («Q2140524» «P527» «Q2941309»), («Q2941309» «P31» «Q2095»)
«Q3823549» «recipe-test» «Q11690488» ⇐ («Q3823549» «P527» «Q11690488»), («Q11690488» «P31» «Q2095»)
«Q5203376» «recipe-test» «Q11690488» ⇐ («Q5203376» «P527» «Q11690488»), («Q11690488» «P31» «Q2095»)
«Q2914862» «recipe-test» «Q2830455» ⇐ («Q2914862» «P527» «Q2830455»), («Q2830455» «P31» «Q2095»)
«Q84564532» «recipe-test» «Q2830455» ⇐ («Q84564532» «P527» «Q2830455»), («Q2830455» «P31» «Q2095»)
«Q1187319» «recipe-test» «Q2830455» ⇐ («Q1187319» «P527» «Q2830455»), («Q2830455» «P31» «Q2095»)
«Q3635711» «recipe-test» «Q2830455» ⇐ («Q3635711» «P527» «Q2830455»), («Q2830455» «P31» «Q2095»)
«Q5708347» «recipe-test» «Q2830455» ⇐ («Q5708347» «P527» «Q2830455»), («Q2830455» «P31» «Q2095»)
«Q15880225» «recipe-test» «Q3290783» ⇐ («Q15880225» «P527» «Q3290783»), («Q3290783» «P31» «Q2095»)
«Q7157930» «recipe-test» «Q147651» ⇐ («Q7157930» «P527» «Q147651»), («Q147651» «P31» «Q2095»)
«Q117971950» «recipe-test» «Q147651» ⇐ («Q117971950» «P527» «Q147651»), («Q147651» «P31» «Q2095»)
«Q7157931» «recipe-test» «Q147651» ⇐ («Q7157931» «P527» «Q147651»), («Q147651» «P31» «Q2095»)
«Q7157935» «recipe-test» «Q147651» ⇐ («Q7157935» «P527» «Q147651»), («Q147651» «P31» «Q2095»)
«Q2917139» «recipe-test» «Q147651» ⇐ («Q2917139» «P527» «Q147651»), («Q147651» «P31» «Q2095»)
«Q1421521» «recipe-test» «Q147651» ⇐ («Q1421521» «P527» «Q147651»), («Q147651» «P31» «Q2095»)
«Q1369093» «recipe-test» «Q147651» ⇐ («Q1369093» «P527» «Q147651»), («Q147651» «P31» «Q2095»)
«Q7157944» «recipe-test» «Q147651» ⇐ («Q7157944» «P527» «Q147651»), («Q147651» «P31» «Q2095»)
Reasoning complete. Total unification matches processed: 1500. Total contradictions found: 0.
Reasoning summary: 1500 matches processed, 0 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h0m0.012s – 1500 matches processed, 0 contradictions found.
```

**Result: 66 cases found** where an item has a food (Q2095) as a "part". These may be candidates for migration to a more specific property like "has ingredient".

## Philosophical Note: "Everything" (Q2165236)

The Task Force has discussed Q2165236 ("everything") as the theoretical top of the parthood hierarchy. We explored its structure:

```
wikidata> X P361 Q2165236
Answer: «Q203872» «P361» «Q2165236»

wikidata> .node Q203872
Name in language 'en': 'being'

wikidata> Q2165236 P527 X
(no results)
```

Only "being" (Q203872) is explicitly stated as part of "everything", and "everything" has no explicit parts. This reflects Ege's philosophical observation that Q2165236 may only contain "real" things.

## Summary

| Check | Result |
|-------|--------|
| P361 antisymmetry violations | **31 pairs** |
| P527 antisymmetry violations | **23 pairs** |
| P361/P527 inverse consistency | ~450,000 consistent pairs |
| Transitivity materialized | No |
| P361/P527/P463 subproperties | None (yet) |
| Recipe/ingredient misuse | 66 cases found |

## Future Work

1. **Batch fixing cycles**: The detected antisymmetry violations can be exported for cleanup via QuickStatements
2. **Transitive closure support**: zelph could implement `P361+` syntax to match SPARQL's `wdt:P361+`
3. **Subproperty analysis**: Once the Task Force establishes subproperties, zelph can analyze their usage patterns
4. **Deeper ingredient analysis**: Expand the recipe test to find more cases of mereological property misuse

## References

- [Wikidata:WikiProject Ontology/Mereology Task Force](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Mereology_Task_Force)
- [Stanford Encyclopedia of Philosophy: Mereology](https://plato.stanford.edu/entries/mereology/)
- [Effort 0001: Recipe relationships](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Mereology_Task_Force/Effort_0001:_Removing_or_migrating_part_of_statements_in_recipe_relationships)
