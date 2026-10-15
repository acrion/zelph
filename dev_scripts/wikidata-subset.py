#!/usr/bin/env python3
"""Cut a subset out of a Wikidata JSON dump, chosen by criteria that are data.

    wikidata-subset.py scan DUMP TABLE [--criteria FILE] [--quantity P ...] [--jobs N]
    wikidata-subset.py keep TABLE CRITERIA KEEP
    wikidata-subset.py extract DUMP KEEP OUT.json [--criteria FILE] [--language L ...]
    wikidata-subset.py accept CRITERIA KEEP NETWORK > SESSION.zph
    wikidata-subset.py verdict CRITERIA LOG NETWORK
    wikidata-subset.py criteria CRITERIA

scan reads the dump once and writes a table with one line per entity: its id,
its number of sitelinks, the largest amount of every quantity property the
criteria name (P1082 population, say), and each item-valued claim of its main
snaks, at every rank -- exactly the claims zelph's importer reads.

keep reads only the table, in four streaming passes, and decides with the
criteria file which entities the subset holds. Each kept entity is either a
full line (kind K: every item-valued claim whose object is in the subset) or a
class-only line (kind C: its subclass-of claims and nothing else). The keep
list names the reasons of every entity, and its header and the standard output
give a count per reason.

extract reads the dump a second time and writes the kept entities as a dump of
their own, which `.load OUT.json` imports into zelph: labels in the chosen
languages, no descriptions, aliases, sitelinks, qualifiers or references, and
only the claims a keep-list line allows.

accept writes a zelph session that loads the imported network and checks the
acceptance list of the criteria file; verdict reads that session's output,
prints the figures (nodes, RAM, file size, load time, names) with a warning
for each one above its target, and fails when a check failed or did not run.

criteria checks a criteria file without reading a dump or a table.

DUMP is a .json, a .json.bz2 (decompressed through a pipe by lbzip2, or by
bzip2 where lbzip2 is missing) or - for standard input. Every pass streams: no
input is read into memory whole. The outputs are written under a temporary
name and renamed once complete.

The criteria file (JSON):

    instance_of, subclass_of    the two class properties (default P31, P279)
    quantities                  the quantity properties scan records
    languages                   the label languages extract keeps (default en, mul)
    keep                        rules; an entity a rule matches is kept (K) with
                                the rule's "reason". A rule's conditions all hold:
        entity_type             "item" or "property"
        min_sitelinks           at least this many sitelinks
        min_quantity            {"P1082": 100000}: the largest amount at least that
        instance_of             a direct instance of one of these classes
        instance_of_subclass_of an instance of one of these classes or of a
                                subclass of one (through subclass_of, any depth)
        not_instance_of_subclass_of   the opposite of the previous
        has_value               {"P105": [...]}: a claim with one of these values
        keep_values_of          {"P36": "capital"}: also keep the claim's values,
                                with the given reason
    value_items                 {"reason", "min_referrers", "ignore_properties"}:
                                keep an item that at least min_referrers kept
                                entities point at through other properties
    ancestors                   {"reason"}: keep every subclass_of ancestor of a
                                kept entity
    class_graph                 {"reason", "exclude_instances_of"}: every subject
                                and object of a subclass_of claim becomes a
                                class-only line, unless it is a direct instance
                                of an excluded class
    absent_samples              [{"name", "instance_of"}]: keep reports the first
                                instance of each class that the subset leaves out
    acceptance                  what accept checks:
        labels                  {"en": [[word, id], ...]}: the word resolves to id
        carried_labels          {"en": [[word, id], ...]}: id carries the word as
                                its name, whichever entity the word resolves to
        facts                   [[subject, property, object], ...]
        paths                   [[subject, "P31/P279*", object], ...]; a step with
                                a trailing * follows the property any number of
                                times, zero included
        no_instances_of         classes of which no instance may be in the network
        present_reasons         every entity kept for one of these reasons must
                                be in the network with a label
        marker                  a name in language zelph that the import writes
        targets                 {"nodes", "ram_gib", "file_bytes", "load_seconds"}

Exit codes: 0 done, 1 a check failed, 2 the input or the criteria are unusable.
"""

import argparse
import collections
import json
import multiprocessing
import os
import re
import shutil
import subprocess
import sys

TABLE_FORMAT = b"# wikidata-subset table 1"
KEEP_FORMAT = "# wikidata-subset keep 1"
BATCH_BYTES = 8 << 20

ID_TAG = b'"id":"'
SITELINKS_TAG = b'"sitelinks":{'
SITE_TAG = b'"site":"'
SNAK = rb'"mainsnak":\{"snaktype":"value","property":"'
HASH = rb'(?:,"hash":"[0-9a-f]+")?'
CLAIM = re.compile(SNAK + rb'(P\d+)"' + HASH
                   + rb',"datavalue":\{"value":\{"entity-type":"item",(?:"numeric-id":\d+,)?"id":"(Q\d+)"')


class Unusable(Exception):
    """An input or a criteria file that cannot be used; the message says why."""


def fail(message):
    print(f"wikidata-subset: error: {message}", file=sys.stderr)
    sys.exit(2)


def entity_key(entity):
    """Sort order of ids: properties first, then by number (P31 < P279 < Q2 < Q10)."""
    return (entity[:1], int(entity[1:]) if entity[1:].isdigit() else 0, entity)


def write_atomically(path, mode="wb"):
    """An output file under a temporary name; rename_into_place gives it its name."""
    return open(path + ".partial", mode)


def rename_into_place(path):
    os.replace(path + ".partial", path)


# --------------------------------------------------------------------------
# reading a dump

def open_dump(path, threads=None):
    """The dump's lines as a binary stream, and the decompressor process or None."""
    if path == "-":
        return sys.stdin.buffer, None
    if not os.path.isfile(path):
        raise Unusable(f"no such dump: {path}")
    if path.endswith(".bz2"):
        tool = shutil.which("lbzip2")
        if tool:
            command = [tool, "-dc"] + (["-n", str(threads)] if threads else []) + [path]
        else:
            tool = shutil.which("bzip2")
            if not tool:
                raise Unusable(f"{path} needs lbzip2 or bzip2 on PATH")
            command = [tool, "-dc", path]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, bufsize=1 << 20)
        return process.stdout, process
    return open(path, "rb", buffering=1 << 20), None


def close_dump(stream, process, path):
    if process is None:
        if stream is not sys.stdin.buffer:
            stream.close()
        return
    stream.close()
    if process.wait() != 0:
        raise Unusable(f"decompressing {path} failed (exit {process.returncode})")


def dump_batches(stream, size=BATCH_BYTES):
    """Lists of complete lines of about size bytes each."""
    while True:
        lines = stream.readlines(size)
        if not lines:
            return
        yield lines


def entity_of(line):
    """The id of an entity line: the first "id" of the line, as the importer reads it."""
    if not line.startswith(b"{"):
        return None
    i = line.find(ID_TAG)
    if i < 0:
        return None
    return line[i + 6:line.find(b'"', i + 6)]


# --------------------------------------------------------------------------
# scan

_quantity_patterns = {}


def quantity_patterns(quantities):
    key = tuple(quantities)
    if key not in _quantity_patterns:
        _quantity_patterns[key] = [
            (p, b'"property":"' + p + b'"',
             re.compile(SNAK + p + rb'"' + HASH + rb',"datavalue":\{"value":\{"amount":"([+-]?[0-9.]+)"'))
            for p in quantities]
    return _quantity_patterns[key]


def scan_line(line, quantities):
    """One table row for a dump line, or None for a line that holds no entity."""
    entity = entity_of(line)
    if entity is None:
        return None
    s = line.rfind(SITELINKS_TAG)
    sitelinks = line.count(SITE_TAG, s) if s >= 0 else 0
    amounts = []
    for prop, token, pattern in quantity_patterns(quantities):
        if token in line:
            found = pattern.findall(line)
            if found:
                amounts.append(prop + b"=" + max(found, key=float).lstrip(b"+"))
    groups = {}
    for prop, obj in CLAIM.findall(line):
        groups.setdefault(prop, {})[obj] = None
    claims = b" ".join(p + b"=" + b",".join(o) for p, o in groups.items())
    return b"%s\t%d\t%s\t%s\n" % (entity, sitelinks, b" ".join(amounts), claims)


def scan_batch(lines, quantities):
    rows = [scan_line(line, quantities) for line in lines]
    return b"".join(r for r in rows if r is not None)


def scan(args):
    quantities = list(args.quantity or [])
    if args.criteria:
        quantities += [q for q in load_criteria(args.criteria).get("quantities", []) if q not in quantities]
    quantities = [q.encode() for q in quantities]
    stream, process = open_dump(args.dump, args.decompress_threads)
    rows = 0
    with write_atomically(args.table) as out:
        out.write(TABLE_FORMAT + b"\tquantities=" + b",".join(quantities) + b"\n")
        if args.jobs <= 1:
            for lines in dump_batches(stream, args.batch_bytes):
                chunk = scan_batch(lines, quantities)
                rows += chunk.count(b"\n")
                out.write(chunk)
        else:
            # A bounded number of batches in flight: Pool.imap would read
            # the entire dump ahead of the workers.
            with multiprocessing.Pool(args.jobs) as pool:
                pending = collections.deque()
                for lines in dump_batches(stream, args.batch_bytes):
                    pending.append(pool.apply_async(scan_batch, (lines, quantities)))
                    while len(pending) >= 2 * args.jobs:
                        chunk = pending.popleft().get()
                        rows += chunk.count(b"\n")
                        out.write(chunk)
                while pending:
                    chunk = pending.popleft().get()
                    rows += chunk.count(b"\n")
                    out.write(chunk)
    close_dump(stream, process, args.dump)
    rename_into_place(args.table)
    print(f"scanned {rows} entities into {args.table}")


# --------------------------------------------------------------------------
# the criteria

def load_criteria(path):
    try:
        with open(path, encoding="utf-8") as f:
            criteria = json.load(f)
    except (OSError, ValueError) as e:
        raise Unusable(f"cannot read the criteria file {path}: {e}")
    if not isinstance(criteria, dict):
        raise Unusable(f"{path} does not hold a JSON object")
    named = [r.get("reason") for r in criteria.get("keep", [])]
    named += [r for rule in criteria.get("keep", []) for r in rule.get("keep_values_of", {}).values()]
    named += [criteria[k].get("reason") for k in ("value_items", "ancestors", "class_graph") if k in criteria]
    for reason in named:
        if not isinstance(reason, str) or not reason or any(c in reason for c in ",\t\n"):
            raise Unusable(f"{path}: every reason is a non-empty name without a comma or a tab: {reason!r}")
    return criteria


RULE_KEYS = {"reason", "entity_type", "min_sitelinks", "min_quantity", "instance_of",
             "instance_of_subclass_of", "not_instance_of_subclass_of", "has_value", "keep_values_of"}


class Rule:
    """One rule of the criteria's "keep" list, with its class sets as bytes."""

    def __init__(self, spec, quantities):
        unknown = set(spec) - RULE_KEYS
        if unknown or "reason" not in spec:
            raise Unusable(f"a keep rule needs a reason and knows only {sorted(RULE_KEYS)}: {spec}")
        self.reason = spec["reason"]
        self.entity_type = spec.get("entity_type")
        if self.entity_type not in (None, "item", "property"):
            raise Unusable(f"{self.reason}: entity_type is item or property, not {self.entity_type}")
        self.min_sitelinks = spec.get("min_sitelinks")
        self.min_quantity = [(p.encode(), float(v)) for p, v in spec.get("min_quantity", {}).items()]
        for p, _ in self.min_quantity:
            if p not in quantities:
                raise Unusable(f"{self.reason}: the table records no {p.decode()} (scan with the criteria file)")
        self.instance_of = {c.encode() for c in spec.get("instance_of", [])}
        self.below = [c.encode() for c in spec.get("instance_of_subclass_of", [])]
        self.not_below = [c.encode() for c in spec.get("not_instance_of_subclass_of", [])]
        self.has_value = [(p.encode() + b"=", {v.encode() for v in vs}) for p, vs in spec.get("has_value", {}).items()]
        self.keep_values_of = [(p.encode() + b"=", r) for p, r in spec.get("keep_values_of", {}).items()]
        self.below_set = self.not_below_set = None

    def classes(self):
        return self.below + self.not_below

    def resolve(self, descendants):
        self.below_set = set().union(*(descendants[c] for c in self.below)) if self.below else None
        self.not_below_set = set().union(*(descendants[c] for c in self.not_below)) if self.not_below else None

    def matches(self, row):
        if self.entity_type and row.entity_type != self.entity_type:
            return False
        if self.min_sitelinks is not None and row.sitelinks < self.min_sitelinks:
            return False
        for p, least in self.min_quantity:
            amount = row.quantity(p)
            if amount is None or amount < least:
                return False
        if self.instance_of and not (self.instance_of & row.instances):
            return False
        if self.below_set is not None and not (self.below_set & row.instances):
            return False
        if self.not_below_set is not None and self.not_below_set & row.instances:
            return False
        for p, wanted in self.has_value:
            if not wanted.intersection(values(row.claims, p)):
                return False
        return True


def values(claims, prop_eq):
    """The objects of one property in a claims field; prop_eq is b"P31=" and the like."""
    if claims.startswith(prop_eq):
        start = len(prop_eq)
    else:
        k = claims.find(b" " + prop_eq)
        if k < 0:
            return ()
        start = k + 1 + len(prop_eq)
    end = claims.find(b" ", start)
    return claims[start:end if end >= 0 else len(claims)].split(b",")


def objects(claims, ignore):
    """Every object of a claims field, once, apart from the ignored properties."""
    found = set()
    for group in claims.split(b" "):
        prop, _, objs = group.partition(b"=")
        if objs and prop not in ignore:
            found.update(objs.split(b","))
    return found


class Row:
    __slots__ = ("entity", "entity_type", "sitelinks", "amounts", "claims", "instances")

    def __init__(self, line, instance_eq):
        fields = line.rstrip(b"\n").split(b"\t")
        if len(fields) != 4:
            raise Unusable(f"a table line without its four fields: {line[:80]!r}")
        self.entity, sitelinks, self.amounts, self.claims = fields
        self.entity_type = "property" if self.entity.startswith(b"P") else "item"
        self.sitelinks = int(sitelinks)
        self.instances = set(values(self.claims, instance_eq))

    def quantity(self, prop):
        for a in self.amounts.split(b" "):
            p, _, v = a.partition(b"=")
            if p == prop:
                return float(v)
        return None


def table_quantities(path):
    """The quantity properties a table records, after checking that it is one."""
    try:
        with open(path, "rb") as f:
            header = f.readline()
    except OSError as e:
        raise Unusable(f"cannot read the table {path}: {e}")
    if not header.startswith(TABLE_FORMAT):
        raise Unusable(f"{path} is not a table written by wikidata-subset.py scan")
    for field in header.rstrip(b"\n").split(b"\t")[1:]:
        if field.startswith(b"quantities="):
            return {q for q in field[len(b"quantities="):].split(b",") if q}
    return set()


def table_rows(path):
    """The lines of a table after its header, streamed."""
    with open(path, "rb", buffering=1 << 20) as f:
        f.readline()
        yield from f


def check_criteria(criteria):
    """Every rule and every acceptance entry in the shape the tool reads; raises Unusable."""
    quantities = {q.encode() for q in criteria.get("quantities", [])}
    rules = [Rule(spec, quantities) for spec in criteria.get("keep", [])]
    for key, needed in (("value_items", {"reason"}), ("ancestors", {"reason"}), ("class_graph", {"reason"})):
        if key in criteria and not needed <= set(criteria[key]):
            raise Unusable(f"{key} needs {sorted(needed)}")
    spec = criteria.get("acceptance", {})

    def shaped(name, entries, size):
        if not isinstance(entries, list) or not all(
                isinstance(e, list) and len(e) == size and all(isinstance(x, str) for x in e) for e in entries):
            raise Unusable(f"acceptance {name}: a list of {size} strings each")

    for key in ("labels", "carried_labels"):
        for lang, pairs in spec.get(key, {}).items():
            shaped(f"{key} {lang}", pairs, 2)
    shaped("facts", spec.get("facts", []), 3)
    shaped("paths", spec.get("paths", []), 3)
    for _, path, _ in spec.get("paths", []):
        if not all(re.fullmatch(r"P\d+\*?", step) for step in path.split("/")):
            raise Unusable(f"acceptance paths: {path} is not a list of P<n> or P<n>* steps")
    for key, value in spec.get("targets", {}).items():
        if key not in ("nodes", "ram_gib", "file_bytes", "load_seconds") or not isinstance(value, (int, float)):
            raise Unusable(f"acceptance targets: {key} is not one of nodes, ram_gib, file_bytes, load_seconds with a number")
    return rules


def criteria_command(args):
    criteria = load_criteria(args.criteria)
    rules = check_criteria(criteria)
    print(f"{args.criteria}: {len(rules)} keep rules, "
          + ", ".join(k for k in ("value_items", "ancestors", "class_graph", "acceptance") if k in criteria))


# --------------------------------------------------------------------------
# keep

def keep(args):
    """Decide the subset in four streaming passes over the table:

    1. the class graph: every subclass claim of every entity;
    2. the rules, each decided on its own row; which class graph nodes the
       dump holds, and which of them are instances of an excluded class;
    3. the values the rules ask to keep, and how many kept entities point at
       each object;
    4. which of the objects that enough kept entities point at the dump holds.

    Then the ancestors of everything kept, and the class-only lines: the class
    graph nodes that are neither kept nor excluded."""
    criteria = load_criteria(args.criteria)
    check_criteria(criteria)
    instance_eq = criteria.get("instance_of", "P31").encode() + b"="
    subclass_eq = criteria.get("subclass_of", "P279").encode() + b"="
    rules = [Rule(spec, table_quantities(args.table)) for spec in criteria.get("keep", [])]
    graph = criteria.get("class_graph")
    excluded_classes = {c.encode() for c in (graph or {}).get("exclude_instances_of", [])}
    value_spec = criteria.get("value_items")
    ancestor_spec = criteria.get("ancestors")
    samples = [(s["name"], s["instance_of"].encode()) for s in criteria.get("absent_samples", [])]

    # Pass 1
    parents = {}
    children = collections.defaultdict(list)
    for line in table_rows(args.table):
        if subclass_eq not in line:
            continue
        row = Row(line, instance_eq)
        ups = values(row.claims, subclass_eq)
        if ups:
            parents[row.entity] = ups
            for up in ups:
                children[up].append(row.entity)
    graph_nodes = set(parents).union(children)

    descendants = {}
    for rule in rules:
        for c in rule.classes():
            if c not in descendants:
                seen, todo = {c}, [c]
                while todo:
                    for child in children.get(todo.pop(), ()):
                        if child not in seen:
                            seen.add(child)
                            todo.append(child)
                descendants[c] = seen
        rule.resolve(descendants)
    del children

    # Pass 2
    reasons = collections.defaultdict(list)
    wanted_values = collections.defaultdict(set)
    holders = {}
    in_dump, excluded = set(), set()
    candidates = {name: [] for name, _ in samples}
    for line in table_rows(args.table):
        row = Row(line, instance_eq)
        matched = False
        for rule in rules:
            if rule.matches(row):
                matched = True
                reasons[row.entity].append(rule.reason)
                for prop_eq, reason in rule.keep_values_of:
                    found = values(row.claims, prop_eq)
                    if found:
                        holders.setdefault((rule.reason, prop_eq, reason), {})[row.entity] = found
                        for v in found:
                            wanted_values[v].add(reason)
        if row.entity in graph_nodes:
            in_dump.add(row.entity)
            if excluded_classes & row.instances:
                excluded.add(row.entity)
        elif not matched:
            for name, cls in samples:
                if cls in row.instances and len(candidates[name]) < 50:
                    candidates[name].append(row.entity)

    # Pass 3
    ignore = {p.encode() for p in (value_spec or {}).get("ignore_properties", [])}
    referrers = collections.Counter()
    for line in table_rows(args.table):
        entity = line[:line.find(b"\t")]
        wanted = wanted_values.get(entity)
        if wanted is None and entity not in reasons:
            continue
        if wanted:
            for reason in sorted(wanted):
                if reason not in reasons[entity]:
                    reasons[entity].append(reason)
        if value_spec:
            referrers.update(objects(Row(line, instance_eq).claims, ignore))

    # Pass 4
    if value_spec:
        if "reason" not in value_spec:
            raise Unusable("value_items needs a reason")
        least = value_spec.get("min_referrers", 1)
        proposed = {o for o, n in referrers.items() if n >= least and o not in reasons}
        del referrers
        for line in table_rows(args.table):
            entity = line[:line.find(b"\t")]
            if entity in proposed:
                reasons[entity].append(value_spec["reason"])

    if ancestor_spec:
        todo = list(reasons)
        while todo:
            for up in parents.get(todo.pop(), ()):
                if up not in reasons and up in in_dump:
                    reasons[up].append(ancestor_spec["reason"])
                    todo.append(up)

    class_only = (in_dump - excluded - set(reasons)) if graph else set()
    chosen = {}
    for name, cls in samples:
        chosen[name] = next((e for e in candidates[name] if e not in reasons and e not in class_only), None)

    counts = collections.Counter(r for rs in reasons.values() for r in rs)
    named = []
    for rule in rules:
        named += [rule.reason] + [r for _, r in rule.keep_values_of]
    named += [spec["reason"] for spec in (value_spec, ancestor_spec) if spec]
    report = [f"reason\t{r}\t{counts.get(r, 0)}" for r in dict.fromkeys(named)]
    if graph:
        report.append(f"reason\t{graph['reason']}\t{len(class_only)}")
    for rule in rules:
        for prop_eq, value_reason in rule.keep_values_of:
            found = holders.get((rule.reason, prop_eq, value_reason), {})
            complete = sum(1 for vs in found.values() if any(v in reasons for v in vs))
            report.append(f"values\t{rule.reason}\t{prop_eq[:-1].decode()}\t{len(found)}\t{complete}")
    report.append(f"kept\t{len(reasons)}")
    report.append(f"class-only\t{len(class_only)}")
    for name, _ in samples:
        report.append(f"sample\t{name}\t{chosen[name].decode() if chosen[name] else '-'}")

    with write_atomically(args.keep, "w") as out:
        out.write(f"{KEEP_FORMAT}\ttable={os.path.basename(args.table)}\n")
        for line in report:
            out.write(f"# {line}\n")
        entries = [(e.decode(), "K", ",".join(rs)) for e, rs in reasons.items()]
        entries += [(e.decode(), "C", graph["reason"]) for e in class_only]
        for entity, kind, why in sorted(entries, key=lambda x: entity_key(x[0])):
            out.write(f"{entity}\t{kind}\t{why}\n")
    rename_into_place(args.keep)
    print("\n".join(report))


def read_keep(path):
    """(kinds, reasons, report lines) of a keep list: kinds maps id -> "K" or "C"."""
    kinds, reasons, report = {}, {}, []
    try:
        f = open(path, encoding="utf-8")
    except OSError as e:
        raise Unusable(f"cannot read the keep list {path}: {e}")
    with f:
        if not f.readline().startswith(KEEP_FORMAT):
            raise Unusable(f"{path} is not a keep list written by wikidata-subset.py keep")
        for line in f:
            if line.startswith("# "):
                report.append(line[2:].rstrip("\n").split("\t"))
                continue
            entity, kind, why = line.rstrip("\n").split("\t")
            kinds[entity] = kind
            reasons[entity] = why.split(",")
    return kinds, reasons, report


# --------------------------------------------------------------------------
# extract

def item_object(statement):
    snak = statement.get("mainsnak", {})
    if snak.get("snaktype") != "value":
        return None
    value = snak.get("datavalue", {}).get("value")
    if not isinstance(value, dict) or value.get("entity-type") != "item":
        return None
    return value.get("id")


def slim_statement(statement, obj):
    """A statement as the importer reads it: the main snak in the dump's own
    key order, without a hash and with a numeric id, then type, id and rank."""
    snak = statement["mainsnak"]
    value = {"entity-type": "item", "numeric-id": int(obj[1:]), "id": obj}
    slim = {"mainsnak": {"snaktype": "value", "property": snak.get("property"),
                         "datavalue": {"value": value, "type": "wikibase-entityid"},
                         "datatype": snak.get("datatype", "wikibase-item")}}
    for k in ("type", "id", "rank"):
        if k in statement:
            slim[k] = statement[k]
    return slim


def as_object(value):
    """A JSON object of the dump; the dump writes an empty one as []."""
    return value if isinstance(value, dict) else {}


def extract(args):
    kinds, _, _ = read_keep(args.keep)
    criteria = load_criteria(args.criteria) if args.criteria else {}
    languages = set(args.language or criteria.get("languages", ["en", "mul"]))
    subclass_of = criteria.get("subclass_of", "P279")
    wanted = {e.encode(): k for e, k in kinds.items()}
    found = set()
    stream, process = open_dump(args.dump, args.decompress_threads)
    with write_atomically(args.out) as out:
        out.write(b"[\n")
        first = True
        for lines in dump_batches(stream):
            for line in lines:
                entity = entity_of(line)
                kind = wanted.get(entity) if entity is not None else None
                if kind is None or entity in found:
                    continue
                found.add(entity)
                source = json.loads(line.rstrip(b",\r\n"))
                # The dump's key order: the importer takes the initial "id" from
                # each line as the entity identifier and reads claims only after
                # the label.
                slim = {k: source[k] for k in ("type", "datatype", "id") if k in source}
                slim["labels"] = {k: v for k, v in as_object(source.get("labels")).items() if k in languages}
                claims = {}
                for prop, statements in as_object(source.get("claims")).items():
                    if kind == "C" and prop != subclass_of:
                        continue
                    kept = []
                    for statement in statements:
                        obj = item_object(statement)
                        if obj in kinds:
                            kept.append(slim_statement(statement, obj))
                    if kept:
                        claims[prop] = kept
                slim["claims"] = claims
                out.write((b"" if first else b",\n") + json.dumps(slim, separators=(",", ":")).encode())
                first = False
        out.write(b"\n]\n")
    close_dump(stream, process, args.dump)
    missing = sorted((e.decode() for e in set(wanted) - found), key=entity_key)
    if missing:
        os.remove(args.out + ".partial")
        print(f"extracted {len(found)} of {len(wanted)} entities; not in the dump: "
              + " ".join(missing[:20]) + (f" and {len(missing) - 20} more" if len(missing) > 20 else ""),
              file=sys.stderr)
        return 1
    rename_into_place(args.out)
    print(f"extracted {len(found)} of {len(wanted)} entities into {args.out}")
    return 0


# --------------------------------------------------------------------------
# accept and verdict

def janet(value):
    """A Janet literal for a string or a list of them (tuples)."""
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    return "[" + " ".join(janet(v) for v in value) + "]"


def wrapped(items, per_line=8):
    rows = [" ".join(janet(i) for i in items[k:k + per_line]) for k in range(0, len(items), per_line)]
    return "[" + "\n   ".join(rows) + "]"


JANET_HELPERS = r"""
(var ws-done 0)
(defn ws-check [ok what] (++ ws-done) (print "accept: " (if ok "ok    " "FAIL  ") what))
(defn ws-named [name lang] (find |(= (zelph/name $ lang) name) (zelph/closure name "~" true)))
(defn ws-id [node] (if node (zelph/name node "wikidata") "-"))
(defn ws-reaches [s path o]
  (var nodes (filter truthy? @[(ws-named s "wikidata")]))
  (each step (string/split "/" path)
    (def star (string/has-suffix? "*" step))
    (def p (if star (string/slice step 0 -2) step))
    (def next @[])
    (each n nodes (array/concat next (if star (zelph/closure n p true) (zelph/targets n p))))
    (set nodes next))
  (truthy? (find |(= (ws-id $) o) nodes)))
"""


def accept(args):
    criteria = load_criteria(args.criteria)
    check_criteria(criteria)
    spec = criteria.get("acceptance", {})
    _, reasons, report = read_keep(args.keep)
    if any(c in args.network for c in " \t\n\"\\"):
        raise Unusable(f"the network path cannot be written into a zelph command: {args.network}")
    facts = spec.get("facts", [])
    paths = spec.get("paths", [])
    no_instances = spec.get("no_instances_of", [])
    labels = [(lang, w, i) for lang, pairs in spec.get("labels", {}).items() for w, i in pairs]
    carried = [(lang, w, i) for lang, pairs in spec.get("carried_labels", {}).items() for w, i in pairs]
    samples = [(r[1], r[2]) for r in report if r[0] == "sample" and r[2] != "-"]
    present_reasons = spec.get("present_reasons", [])
    present = sorted((e for e, rs in reasons.items() if set(rs) & set(present_reasons)), key=entity_key)
    marker = spec.get("marker")
    instance_of = criteria.get("instance_of", "P31")
    expected = len(facts) + len(paths) + len(no_instances) + len(carried) + len(samples) \
        + (1 if present_reasons else 0) + len(labels) + (1 if marker else 0)

    out = [".timer off",
           "%", "(def ws-t0 (os/clock))", "nil", "%",
           f".load {args.network}",
           '%(print "accept: figure load-seconds " (string/format "%.3f" (- (os/clock) ws-t0)))',
           ".stat",
           ".lang wikidata",
           "%", JANET_HELPERS.strip(),
           f"(each [s p o] {wrapped(facts, 4)}",
           '  (ws-check (zelph/exists s p o) (string "fact " s " " p " " o)))',
           f"(each [s path o] {wrapped(paths, 4)}",
           '  (ws-check (ws-reaches s path o) (string "path " s " " path " " o)))',
           f"(each c {wrapped(no_instances)}",
           f'  (ws-check (empty? (zelph/sources {janet(instance_of)} c)) (string "no instance of " c)))',
           f"(each [name id] {wrapped(samples, 4)}",
           '  (ws-check (nil? (ws-named id "wikidata")) (string "absent " id ", the " name " no criterion keeps")))',
           f"(each [lang word id] {wrapped(carried, 4)}",
           '  (def n (ws-named id "wikidata"))',
           '  (ws-check (and n (= (zelph/name n lang) word)) (string "carried " id " " lang " \\"" word "\\"")))',
           "(def ws-present @[])",
           "(var ws-missing 0)",
           f"(each id {wrapped(present, 10)}",
           '  (def n (ws-named id "wikidata"))',
           '  (def label (and n (zelph/name n "en")))',
           "  (if (and label (not= label id))",
           "    (array/push ws-present [id label])",
           '    (do (++ ws-missing) (when (<= ws-missing 20) (print "accept: missing or without a label: " id)))))']
    if present_reasons:
        out.append(f'(ws-check (= ws-missing 0) (string "present " (length ws-present) " of {len(present)} entities kept for '
                   + ", ".join(present_reasons).replace('"', "'") + ', each with a label"))')
    out.append("%")
    for lang in sorted({lang for lang, _, _ in labels} | ({"en"} if present_reasons else set())):
        out += [f".lang {lang}", "%",
                f"(each [word id] {wrapped([(w, i) for l, w, i in labels if l == lang], 4)}",
                f'  (def got (ws-id (ws-named word "{lang}")))',
                f'  (ws-check (= got id) (string "label {lang} \\"" word "\\" -> " got (if (= got id) "" (string ", want " id)))))']
        if lang == "en" and present_reasons:
            out += ["(var ws-self 0)",
                    '(each [id label] ws-present (when (= (ws-id (ws-named label "en")) id) (++ ws-self)))',
                    '(print "accept: figure self-resolving " ws-self " of " (length ws-present))']
        out.append("%")
    out += [".lang zelph", "%"]
    if marker:
        out.append(f'(ws-check (not (empty? (zelph/closure {janet(marker)} "~" true))) (string "marker " {janet(marker)}))')
    out += [f'(print "accept: checks-done " ws-done " of {expected}")', "%", ".quit"]
    sys.stdout.write("\n".join(out) + "\n")


def verdict(args):
    criteria = load_criteria(args.criteria)
    targets = criteria.get("acceptance", {}).get("targets", {})
    try:
        with open(args.log, encoding="utf-8", errors="replace") as f:
            log = f.read().splitlines()
    except OSError as e:
        raise Unusable(f"cannot read the session log {args.log}: {e}")
    figures, failures, passed, done, names, section = {}, [], 0, None, {}, None
    for line in log:
        k = line.find("accept: ")
        if k >= 0:
            text = line[k + 8:]
            if text.startswith("ok"):
                passed += 1
            elif text.startswith("FAIL"):
                failures.append(text)
            elif text.startswith("checks-done "):
                done = text.split()[1:4:2]
            elif text.startswith("figure "):
                key, _, value = text[7:].partition(" ")
                figures[key] = value
            elif text.startswith("missing"):
                print(text)
            continue
        m = re.search(r"\bNodes: (\d+)$", line)
        if m:
            figures["nodes"] = m.group(1)
        m = re.search(r"RAM Usage: ([0-9.]+) GiB", line)
        if m:
            figures["ram-gib"] = m.group(1)
        if line.rstrip().endswith("Name-of-Node Entries by language:"):
            section, names = "names", {}
        elif section == "names" and re.match(r"^\s+\S+: \d+$", line):
            lang, count = line.strip().split(": ")
            names[lang] = count
        else:
            section = None
    for lang, count in sorted(names.items()):
        figures[f"names-{lang}"] = count
    try:
        figures["file-bytes"] = str(os.path.getsize(args.network))
    except OSError:
        failures.append(f"no network file {args.network}")
    warnings = []
    for key, figure in (("nodes", "nodes"), ("ram_gib", "ram-gib"), ("file_bytes", "file-bytes"),
                        ("load_seconds", "load-seconds")):
        if key in targets and figure in figures and float(figures[figure]) > float(targets[key]):
            warnings.append(f"{figure} {figures[figure]} above the target {targets[key]}")
    for key in sorted(figures):
        print(f"figure {key} {figures[key]}")
    for w in warnings:
        print(f"WARN  {w}")
    for f in failures:
        print(f"FAIL  {f}")
    complete = done is not None and done[0] == done[1]
    if not complete:
        print("FAIL  the session did not run every check" + (f" ({done[0]} of {done[1]})" if done else ""))
    total = done[1] if done else "?"
    print(f"summary checks={passed}/{total} failed={len(failures)} warnings={len(warnings)} "
          + " ".join(f"{k}={figures[k].replace(' of ', '/')}" for k in sorted(figures)))
    return 0 if complete and not failures else 1


# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     epilog="See the module documentation (the top of this file) for the criteria file.")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("scan", help="dump -> per-entity table")
    p.add_argument("dump")
    p.add_argument("table")
    p.add_argument("--criteria", help="record the quantity properties this criteria file names")
    p.add_argument("--quantity", action="append", help="a quantity property to record (repeatable)")
    p.add_argument("--jobs", type=int, default=1, help="worker processes (default 1)")
    p.add_argument("--batch-bytes", type=int, default=BATCH_BYTES,
                   help=f"bytes of dump lines handed to a worker at a time (default {BATCH_BYTES})")
    p.add_argument("--decompress-threads", type=int, help="threads of lbzip2 (default: its own)")
    p = sub.add_parser("keep", help="table + criteria -> keep list")
    p.add_argument("table")
    p.add_argument("criteria")
    p.add_argument("keep")
    p = sub.add_parser("extract", help="dump + keep list -> dump of the kept entities")
    p.add_argument("dump")
    p.add_argument("keep")
    p.add_argument("out")
    p.add_argument("--criteria", help="take the label languages and the class property from this file")
    p.add_argument("--language", action="append", help="a label language to keep (repeatable; default en, mul)")
    p.add_argument("--decompress-threads", type=int, help="threads of lbzip2 (default: its own)")
    p = sub.add_parser("accept", help="criteria + keep list -> zelph session checking the network")
    p.add_argument("criteria")
    p.add_argument("keep")
    p.add_argument("network")
    p = sub.add_parser("verdict", help="criteria + the session's output -> figures and the outcome")
    p.add_argument("criteria")
    p.add_argument("log")
    p.add_argument("network")
    p = sub.add_parser("criteria", help="check a criteria file")
    p.add_argument("criteria")
    args = parser.parse_args()
    try:
        return {"scan": scan, "keep": keep, "extract": extract, "accept": accept, "verdict": verdict,
                "criteria": criteria_command}[args.command](args) or 0
    except Unusable as e:
        fail(str(e))


if __name__ == "__main__":
    sys.exit(main())
