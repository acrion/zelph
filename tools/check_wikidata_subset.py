#!/usr/bin/env python3
"""Hold dev_scripts/wikidata-subset.py to the subset it cuts, end to end.

The check writes a dump of its own in the layout of a real one (key order,
main snaks without a hash, qualifiers with one, references, sitelinks,
quantities, empty objects written as []), cuts a subset out of it with the
tool's scan, keep and extract, imports the extract with the zelph binary and
asks the network what a user of the subset relies on: labels that several
entities share, the mul label, the absence of what no criterion keeps, the
class graph between the kept entities. The tool's acceptance session and its
verdict run on the same network, once with a list that holds and once with a
wrong expectation.

Each case is a function below, and its docstring says why the case exists.

Usage: check_wikidata_subset.py ZELPH TOOL

Exit codes: 0 every case holds, 1 one of them does not, 2 the question could
not be put.
"""

import bz2
import json
import os
import shutil
import subprocess
import sys
import tempfile

TIMEOUT_SECONDS = 300


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


# --------------------------------------------------------------------------
# the dump

def item_snak(prop, obj):
    return {"snaktype": "value", "property": prop,
            "datavalue": {"value": {"entity-type": "item", "numeric-id": int(obj[1:]), "id": obj},
                          "type": "wikibase-entityid"}, "datatype": "wikibase-item"}


def amount_snak(prop, amount):
    return {"snaktype": "value", "property": prop,
            "datavalue": {"value": {"amount": f"+{amount}", "unit": "1"}, "type": "quantity"},
            "datatype": "quantity"}


def entity(eid, en=None, mul=None, de=None, claims=(), amounts=(), sitelinks=0, empty_as_list=False,
           qualifier=None, reference=None, no_value_first=(), hashed=False):
    """One entity in the key order of a real dump; claims are (property, object[, rank])."""
    labels = {}
    for lang, value in (("en", en), ("de", de), ("mul", mul)):
        if value:
            labels[lang] = {"language": lang, "value": value}
    statements = {}

    def add(prop, snak, rank="normal"):
        k = sum(len(v) for v in statements.values())
        s = {"mainsnak": snak, "type": "statement"}
        if qualifier and snak.get("datavalue", {}).get("type") == "wikibase-entityid":
            s["qualifiers"] = {"P805": [dict(item_snak("P805", qualifier), hash="0a1b2c")]}
            s["qualifiers-order"] = ["P805"]
        s["id"] = f"{eid}${k}"
        s["rank"] = rank
        if reference:
            s["references"] = [{"hash": "3d4e5f", "snaks": {"P248": [item_snak("P248", reference)]},
                                "snaks-order": ["P248"]}]
        statements.setdefault(prop, []).append(s)

    for prop in no_value_first:
        add(prop, {"snaktype": "somevalue", "property": prop, "datatype": "wikibase-item"})
    for claim in claims:
        snak = item_snak(claim[0], claim[1])
        if hashed:
            snak = {"snaktype": "value", "property": claim[0], "hash": "9f8e7d", **{k: snak[k] for k in ("datavalue", "datatype")}}
        add(claim[0], snak, *claim[2:])
    for prop, amount in amounts:
        add(prop, amount_snak(prop, amount))
    empty = [] if empty_as_list else {}
    e = {"type": "property" if eid.startswith("P") else "item"}
    if eid.startswith("P"):
        e["datatype"] = "quantity" if eid == "P1082" else "wikibase-item"
    e["id"] = eid
    e["labels"] = labels
    e["descriptions"] = {"en": {"language": "en", "value": "a description"}} if en else empty
    e["aliases"] = empty
    e["claims"] = statements or empty
    if not eid.startswith("P"):
        e["sitelinks"] = {f"x{k}wiki": {"site": f"x{k}wiki", "title": en or mul or eid, "badges": []}
                          for k in range(sitelinks)} or empty
    e["pageid"] = 1000 + int(eid[1:]) % 1000
    e["ns"] = 120 if eid.startswith("P") else 0
    e["title"] = ("Property:" if eid.startswith("P") else "") + eid
    e["lastrevid"] = 1
    e["modified"] = "2099-01-01T00:00:00Z"
    return e


PROPERTIES = [("P31", "instance of"), ("P279", "subclass of"), ("P50", "author"), ("P106", "occupation"),
              ("P36", "capital"), ("P17", "country"), ("P131", "located in the administrative territorial entity"),
              ("P21", "sex or gender"), ("P105", "taxon rank"), ("P1082", "population"), ("P6259", "epoch"),
              ("P735", "given name"), ("P805", "statement is subject of"), ("P248", "stated in")]


def dump_entities():
    e = [entity(p, label) for p, label in PROPERTIES]
    e += [
        entity("Q35120", "entity", sitelinks=40, empty_as_list=True),
        entity("Q7239", "organism", claims=[("P279", "Q35120")], sitelinks=40),
        entity("Q16521", "taxon", claims=[("P279", "Q35120")], sitelinks=40),
        entity("Q215627", "person", claims=[("P279", "Q35120")], sitelinks=40),
        entity("Q5", "human", claims=[("P279", "Q215627")], sitelinks=40),
        entity("Q482980", "author", claims=[("P279", "Q215627")]),
        entity("Q36180", "writer", claims=[("P279", "Q482980")], sitelinks=40),
        entity("Q6256", "country", claims=[("P279", "Q35120")], sitelinks=40),
        entity("Q3624078", "sovereign state", claims=[("P279", "Q6256")], sitelinks=40),
        entity("Q486972", "human settlement", claims=[("P279", "Q35120")]),
        entity("Q515", "city", claims=[("P279", "Q486972")], sitelinks=40),
        entity("Q1549591", "big city", claims=[("P279", "Q515")]),
        entity("Q532", "village", claims=[("P279", "Q486972")]),
        entity("Q17442446", "Wikimedia internal item", claims=[("P279", "Q35120")]),
        entity("Q4167836", "Wikimedia category", claims=[("P279", "Q17442446")]),
        entity("Q202444", "given name", claims=[("P279", "Q35120")]),
        entity("Q12308941", "male given name", claims=[("P279", "Q202444")]),
        entity("Q7725634", "literary work", claims=[("P279", "Q35120")]),
        entity("Q11424", "film", claims=[("P279", "Q35120")]),
        entity("Q7187", "gene", claims=[("P279", "Q35120")]),
        entity("Q36732", "kingdom"), entity("Q37517", "class"), entity("Q7432", "species"),
        entity("Q6581097", "male", sitelinks=16),
        entity("Q729", "animal", claims=[("P31", "Q16521"), ("P279", "Q7239"), ("P105", "Q36732")], sitelinks=50),
        entity("Q7377", "mammal", claims=[("P31", "Q16521"), ("P279", "Q729"), ("P105", "Q37517")], sitelinks=40),
        entity("Q90000001", "obscure beetle", claims=[("P31", "Q16521"), ("P105", "Q7432")]),
        entity("Q110551885", "mammal", claims=[("P279", "Q729")]),
        entity("Q144", "dog", mul="Canis lupus familiaris", claims=[("P279", "Q110551885")], sitelinks=40),
        entity("Q183", "Germany", de="Deutschland", no_value_first=["P36"],
               claims=[("P31", "Q3624078"), ("P31", "Q6256"), ("P17", "Q183"), ("P36", "Q64", "preferred"),
                       ("P36", "Q586")], sitelinks=60),
        entity("Q64", "Berlin", claims=[("P31", "Q1549591"), ("P131", "Q183"), ("P17", "Q183")],
               amounts=[("P1082", 3600000)], sitelinks=50),
        entity("Q586", "Bonn", claims=[("P31", "Q515"), ("P17", "Q183")], sitelinks=3),
        entity("Q1726", "Munich", claims=[("P31", "Q1549591"), ("P17", "Q183")], amounts=[("P1082", 1500000)],
               sitelinks=2),
        entity("Q90000002", "Kleindorf", claims=[("P31", "Q532"), ("P17", "Q183")], amounts=[("P1082", 500)],
               sitelinks=1),
        entity("Q42", mul="Douglas Adams", qualifier="Q90000003", reference="Q90000006",
               claims=[("P31", "Q5"), ("P106", "Q36180"), ("P21", "Q6581097"), ("P735", "Q4925477")], sitelinks=40),
        entity("Q90000003", "Obscure Person", claims=[("P31", "Q5"), ("P21", "Q6581097")]),
        entity("Q90000005", "Second Person", claims=[("P31", "Q5"), ("P21", "Q6581097")], sitelinks=35),
        entity("Q90000007", "Third Person", claims=[("P31", "Q5"), ("P21", "Q6581097")], sitelinks=31),
        entity("Q4925477", "John", claims=[("P31", "Q12308941")], sitelinks=2),
        entity("Q3107329", "The Hitchhiker's Guide to the Galaxy", claims=[("P31", "Q7725634"), ("P50", "Q42")],
               sitelinks=35, hashed=True),
        entity("Q90000006", "Obscure Book", claims=[("P31", "Q7725634"), ("P50", "Q90000003")]),
        entity("Q90000004", "Category:Germany", claims=[("P31", "Q4167836")], sitelinks=40),
        entity("Q90000010", "some gene", claims=[("P31", "Q7187"), ("P279", "Q35120")]),
        entity("Q90000011", "some gene variant", claims=[("P31", "Q7187"), ("P279", "Q90000010")]),
        entity("Q90000012", "orphan class", claims=[("P31", "Q11424"), ("P279", "Q35120"), ("P279", "Q90000020")]),
        entity("Q99999996", "Berlin", empty_as_list=True),
        entity("Q99999997", "animal", claims=[("P31", "Q11424")], sitelinks=35),
        entity("Q99999998", "Germany"),
    ]
    return e


CRITERIA = {
    "instance_of": "P31", "subclass_of": "P279", "quantities": ["P1082"], "languages": ["en", "mul"],
    "keep": [
        {"reason": "property", "entity_type": "property"},
        {"reason": "sitelinks>=30", "min_sitelinks": 30, "not_instance_of_subclass_of": ["Q17442446"]},
        {"reason": "state", "instance_of": ["Q3624078", "Q6256"], "keep_values_of": {"P36": "capital"}},
        {"reason": "settlement pop>=100000", "instance_of_subclass_of": ["Q486972"],
         "min_quantity": {"P1082": 100000}},
        {"reason": "taxon rank", "instance_of": ["Q16521"], "has_value": {"P105": ["Q36732", "Q37517"]}},
        {"reason": "given name", "instance_of_subclass_of": ["Q202444"]},
    ],
    "value_items": {"reason": "value of >=3 kept", "min_referrers": 3, "ignore_properties": ["P31", "P279"]},
    "ancestors": {"reason": "ancestor"},
    "class_graph": {"reason": "class graph", "exclude_instances_of": ["Q7187"]},
    "absent_samples": [{"name": "taxon", "instance_of": "Q16521"}, {"name": "person", "instance_of": "Q5"},
                       {"name": "village", "instance_of": "Q532"}],
    "acceptance": {
        "labels": {"en": [["animal", "Q729"], ["mammal", "Q7377"], ["dog", "Q144"], ["Douglas Adams", "Q42"],
                          ["Germany", "Q183"], ["Berlin", "Q64"], ["country", "P17"], ["capital", "P36"]]},
        "carried_labels": {"en": [["country", "Q6256"]]},
        "facts": [["Q42", "P31", "Q5"], ["Q183", "P36", "Q64"], ["Q183", "P36", "Q586"], ["Q3107329", "P50", "Q42"]],
        "paths": [["Q144", "P279*", "Q729"], ["Q64", "P31/P279*", "Q515"], ["Q183", "P31/P279*", "Q6256"]],
        "no_instances_of": ["Q4167836"],
        "present_reasons": ["state", "capital"],
        "marker": "wikidata:labels-v2",
        "targets": {"nodes": 100000, "ram_gib": 1, "file_bytes": 10000000, "load_seconds": 60},
    },
}


# --------------------------------------------------------------------------
# running the tool and zelph

class Run:
    def __init__(self, zelph, tool, directory):
        self.zelph, self.tool, self.dir = zelph, tool, directory
        self.entities = dump_entities()
        self.dump = self.path("dump.json")
        with open(self.dump, "w", encoding="ascii", newline="\n") as f:
            f.write("[\n" + ",\n".join(json.dumps(e, separators=(",", ":")) for e in self.entities) + "\n]\n")
        self.write_criteria("criteria.json", CRITERIA)

    def path(self, name):
        return os.path.join(self.dir, name)

    def write_criteria(self, name, criteria):
        with open(self.path(name), "w", encoding="utf-8") as f:
            json.dump(criteria, f, indent=1)

    def tool_run(self, *arguments, stdout=None):
        done = subprocess.run([sys.executable, self.tool, *arguments], cwd=self.dir, capture_output=True,
                              encoding="utf-8", errors="replace", timeout=TIMEOUT_SECONDS)
        if stdout:
            with open(self.path(stdout), "w", encoding="utf-8", newline="\n") as f:
                f.write(done.stdout)
        return done

    def zelph_run(self, script):
        env = dict(os.environ, ZELPH_NO_RLWRAP="1")
        return subprocess.run([self.zelph], input=script, cwd=self.dir, capture_output=True, encoding="utf-8",
                              errors="replace", timeout=TIMEOUT_SECONDS, env=env)

    def read(self, name):
        with open(self.path(name), encoding="utf-8") as f:
            return f.read()

    def table(self, name="table.tsv"):
        rows = {}
        for line in self.read(name).splitlines()[1:]:
            fields = line.split("\t")
            rows[fields[0]] = fields
        return rows

    def keep(self):
        kinds = {}
        for line in self.read("keep.tsv").splitlines():
            if not line.startswith("#"):
                entity, kind, reasons = line.split("\t")
                kinds[entity] = (kind, reasons.split(","))
        return kinds

    def extract(self):
        with open(self.path("subset.json"), encoding="ascii") as f:
            return {e["id"]: e for e in (json.loads(line.rstrip(",\n")) for line in f if line.startswith("{"))}


PROBE = r"""
.timer off
.load subset.json
.lang en
%
(defn named [name lang] (find |(= (zelph/name $ lang) name) (zelph/closure name "~" true)))
(defn id [n] (if n (zelph/name n "wikidata") "-"))
(defn ids [nodes] (string/join (sorted (map id nodes)) ","))
(print "probe\tanimal\t" (id (named "animal" "en")))
(print "probe\tDouglas Adams\t" (id (named "Douglas Adams" "en")))
%
.lang wikidata
%
(print "probe\tQ99999997 en\t" (zelph/name (named "Q99999997" "wikidata") "en"))
(each q ["Q90000001" "Q90000003" "Q90000004" "Q90000006" "Q90000010" "Q90000011" "Q99999996"]
  (print "probe\t" q "\t" (if (named q "wikidata") "present" "absent")))
(print "probe\tQ144 P279\t" (ids (zelph/targets "Q144" "P279")))
(print "probe\tQ144 P279*\t" (ids (zelph/closure "Q144" "P279" true)))
(print "probe\tQ183 P36\t" (ids (zelph/targets "Q183" "P36")))
(print "probe\tQ3107329 P50\t" (ids (zelph/targets "Q3107329" "P50")))
%
"""


def probe_answers(output):
    answers = {}
    for line in output.splitlines():
        k = line.find("probe\t")
        if k >= 0:
            _, key, value = line[k:].split("\t", 2)
            answers[key] = value
    return answers


# --------------------------------------------------------------------------
# the cases

def scan_writes_a_row_per_entity(run):
    """The brackets and separators of a dump are no entities; one row too many or
    too few shifts every count keep makes."""
    rows = run.table()
    want = {e["id"] for e in run.entities}
    if set(rows) != want:
        return f"rows for {sorted(set(rows) ^ want)} differ from the dump's entities"


def scan_reads_main_snaks_only(run):
    """Qualifiers and references hold item values in the same snak shape as a
    claim; the importer reads only the main snak, and so must the table.
    Q42's qualifier points at Q90000003 and its references at Q90000006."""
    claims = run.table()["Q42"][3]
    if "P106=Q36180" not in claims.split(" ") or "Q90000003" in claims or "Q90000006" in claims:
        return f"Q42's claims in the table: {claims}"


def scan_counts_sitelinks_and_quantities(run):
    """The sitelink count and the largest population are the two figures the
    importer drops and the criteria need; the dump writes an empty object as []."""
    rows = run.table()
    got = (rows["Q64"][1], rows["Q64"][2], rows["Q99999996"][1], rows["P31"][1])
    if got != ("50", "P1082=3600000", "0", "0"):
        return f"sitelinks and quantity of Q64, sitelinks of Q99999996 and P31: {got}"


def parallel_scan_writes_the_same_table(run):
    """--jobs hands batches of lines to worker processes; their rows must come
    back in the order of the dump. Batches of 4 KB make the small dump many of
    them."""
    done = run.tool_run("scan", "dump.json", "table-2.tsv", "--criteria", "criteria.json", "--jobs", "2",
                        "--batch-bytes", "4096")
    if done.returncode != 0:
        return f"scan --jobs 2 exited {done.returncode}: {done.stderr}"
    if run.read("table-2.tsv") != run.read("table.tsv"):
        return "the tables differ"


def compressed_dump_is_read_through_a_pipe(run):
    """A real dump is a .json.bz2 of about 100 GB, decompressed by lbzip2 (or
    bzip2) in a process of its own."""
    if not (shutil.which("lbzip2") or shutil.which("bzip2")):
        return "skipped: neither lbzip2 nor bzip2 is on PATH"
    with open(run.dump, "rb") as f, bz2.open(run.path("dump.json.bz2"), "wb") as out:
        out.write(f.read())
    done = run.tool_run("scan", "dump.json.bz2", "table-bz2.tsv", "--criteria", "criteria.json")
    if done.returncode != 0:
        return f"scan of the .bz2 exited {done.returncode}: {done.stderr}"
    if run.read("table-bz2.tsv") != run.read("table.tsv"):
        return "the table of the .bz2 differs from that of the .json"


def keep_report_names_every_reason(run):
    """A criterion that keeps nothing is how a typo in a class id shows; the
    report therefore lists every reason, a count of 0 included."""
    lines = [line.split("\t") for line in run.read("keep.out").splitlines()]
    counts = {f[1]: f[2] for f in lines if f[0] == "reason"}
    want = {"property": "14", "sitelinks>=30": "19", "state": "1", "capital": "2", "settlement pop>=100000": "2",
            "taxon rank": "2", "given name": "1", "value of >=3 kept": "1", "ancestor": "3", "class graph": "10"}
    if counts != want:
        return f"reasons and counts: {counts}"


def capital_of_normal_rank_is_kept(run):
    """Germany's preferred capital is Berlin, and Bonn is a capital of normal
    rank with three sitelinks; the importer reads every rank, so the subset
    keeps both."""
    keep = run.keep()
    if "capital" not in keep.get("Q586", ("", []))[1]:
        return f"Q586 in the keep list: {keep.get('Q586')}"
    if "values\tstate\tP36\t1\t1" not in run.read("keep.out"):
        return "the report does not say that the one state has a kept capital"


def value_item_of_enough_kept_entities_is_kept(run):
    """"male" (Q6581097) has 16 sitelinks, and the three kept people point at
    it; the obscure person who points at it as well is not kept and does not
    count."""
    if run.keep().get("Q6581097", ("", []))[1] != ["value of >=3 kept"]:
        return f"Q6581097 in the keep list: {run.keep().get('Q6581097')}"


def instance_of_excluded_class_leaves_the_class_graph(run):
    """The class graph of a real dump holds millions of chemistry and biology
    classes, each an instance of one of a few classes; excluding those keeps
    the graph small."""
    keep = run.keep()
    found = [q for q in ("Q90000010", "Q90000011") if q in keep]
    if found or keep.get("Q7187", ("",))[0] != "C":
        return f"excluded classes in the keep list: {found}; gene itself: {keep.get('Q7187')}"


def class_the_dump_does_not_hold_is_left_out(run):
    """A subclass claim can point at an entity the dump lacks (a deleted or
    redirected item); listing it would make the extract miss an entity."""
    keep = run.keep()
    if "Q90000020" in keep or keep.get("Q90000012", ("",))[0] != "C":
        return f"Q90000020: {keep.get('Q90000020')}, Q90000012: {keep.get('Q90000012')}"


def extract_holds_exactly_the_kept_entities(run):
    """The extract is what zelph imports; an entity in it that the keep list
    does not name would bring in what no criterion chose."""
    if set(run.extract()) != set(run.keep()):
        return f"extract and keep list differ by {sorted(set(run.extract()) ^ set(run.keep()))}"


def class_only_line_brings_its_subclass_claims_alone(run):
    """A class kept for the graph contributes its place in the hierarchy, not
    its other claims: Q90000012 is an instance of a kept class."""
    claims = run.extract()["Q90000012"]["claims"]
    objects = [s["mainsnak"]["datavalue"]["value"]["id"] for s in claims.get("P279", [])]
    if set(claims) != {"P279"} or objects != ["Q35120"]:
        return f"claims of Q90000012: {json.dumps(claims)[:300]}"


def extract_keeps_labels_of_the_chosen_languages_only(run):
    """Descriptions, aliases, sitelinks and other languages make up most of a
    dump's bytes and nothing of a network."""
    germany = run.extract()["Q183"]
    if set(germany) != {"type", "id", "labels", "claims"} or set(germany["labels"]) != {"en"}:
        return f"keys {sorted(germany)}, labels {sorted(germany['labels'])}"


def claim_reaches_the_extract_without_qualifiers_and_references(run):
    """Qualifiers and references are a large share of a dump's bytes, and the
    import reads neither."""
    statement = run.extract()["Q42"]["claims"]["P106"][0]
    if list(statement) != ["mainsnak", "type", "id", "rank"]:
        return f"Q42's occupation claim in the extract: {json.dumps(statement)[:300]}"


def main_snak_with_a_hash_imports(run):
    """The Wikidata API writes a hash into every main snak, the dumps do not; the
    extract writes the dump's shape whatever it read, and the claim imports."""
    statement = run.extract()["Q3107329"]["claims"]["P50"][0]
    if "hash" in statement["mainsnak"] or run.probe().get("Q3107329 P50") != "Q42":
        return f"main snak {json.dumps(statement['mainsnak'])}; Q3107329 P50: {run.probe().get('Q3107329 P50')}"


def shared_label_resolves_to_the_kept_taxon(run):
    """"animal" is the label of the taxon Q729 and of a kept film with a higher
    id; the name resolves to the lowest id, so the taxon must be in the subset
    while the film keeps the label as its name."""
    answers = run.probe()
    if answers.get("animal") != "Q729" or answers.get("Q99999997 en") != "animal":
        return f"animal -> {answers.get('animal')}, Q99999997's name: {answers.get('Q99999997 en')}"


def mul_label_names_an_item_without_an_english_one(run):
    """Douglas Adams (Q42) has only a mul label; an extract that keeps en alone
    would leave the item without a name."""
    answers = run.probe()
    if answers.get("Douglas Adams") != "Q42":
        return f"Douglas Adams -> {answers.get('Douglas Adams')}"


def what_no_criterion_keeps_is_absent(run):
    """A beetle without sitelinks, an obscure person and book, a category with
    40 sitelinks, the excluded gene classes and an unkept item that shares a
    label: none of them may become a node, not even as the object of a kept
    entity's claim."""
    answers = run.probe()
    present = [q for q in ("Q90000001", "Q90000003", "Q90000004", "Q90000006", "Q90000010", "Q90000011",
                           "Q99999996") if answers.get(q) != "absent"]
    if present:
        return f"present: {present}"


def dog_reaches_animal_through_a_class_that_is_no_taxon(run):
    """dog's only superclass is the non-taxon class Q110551885 "mammal", whose
    only superclass is the taxon Q729; the subset keeps the hierarchy between
    the kept entities whole."""
    answers = run.probe()
    if answers.get("Q144 P279") != "Q110551885" or "Q729" not in answers.get("Q144 P279*", "").split(","):
        return f"Q144 P279: {answers.get('Q144 P279')}; closure {answers.get('Q144 P279*')}"


def property_with_a_valueless_first_claim_keeps_its_claims(run):
    """Germany's first capital claim has no value (somevalue); both capitals
    after it reach the network."""
    if run.probe().get("Q183 P36") != "Q586,Q64":
        return f"Q183 P36: {run.probe().get('Q183 P36')}"


def acceptance_session_passes_on_its_network(run):
    """The verdict is the gate of a regeneration: it must pass when the network
    holds what the list says."""
    done = run.verdict("criteria.json", "accept.zph", "accept.log")
    if done.returncode != 0 or "failed=0" not in done.stdout:
        return f"verdict exited {done.returncode}:\n{done.stdout}{done.stderr}"


def acceptance_session_reports_a_wrong_expectation(run):
    """A verdict that passes whatever the network holds checks nothing: "mammal"
    resolves to the taxon Q7377, not to the class Q110551885."""
    wrong = json.loads(json.dumps(CRITERIA))
    wrong["acceptance"]["labels"]["en"].append(["mammal", "Q110551885"])
    run.write_criteria("criteria-wrong.json", wrong)
    done = run.verdict("criteria-wrong.json", "accept-wrong.zph", "accept-wrong.log")
    if done.returncode != 1 or 'FAIL  label en "mammal" -> Q7377, want Q110551885' not in done.stdout:
        return f"verdict exited {done.returncode}:\n{done.stdout}{done.stderr}"


def figure_above_its_target_warns_without_failing(run):
    """The size targets are estimates; a network above them is reported, and
    only the content checks fail a regeneration."""
    small = json.loads(json.dumps(CRITERIA))
    small["acceptance"]["targets"]["nodes"] = 10
    run.write_criteria("criteria-small.json", small)
    done = run.tool_run("verdict", "criteria-small.json", "accept.log", "subset.bin")
    if done.returncode != 0 or "WARN  nodes" not in done.stdout or "above the target 10" not in done.stdout:
        return f"verdict exited {done.returncode}:\n{done.stdout}{done.stderr}"


def session_that_stops_early_fails_the_verdict(run):
    """A zelph session ends at its first error; the checks after it never ran,
    and a log without them must not pass."""
    log = run.read("accept.log")
    with open(run.path("accept-cut.log"), "w", encoding="utf-8") as f:
        f.write(log[:log.find("accept: checks-done")])
    done = run.tool_run("verdict", "criteria.json", "accept-cut.log", "subset.bin")
    if done.returncode != 1 or "did not run every check" not in done.stdout:
        return f"verdict exited {done.returncode}:\n{done.stdout}{done.stderr}"


CASES = [scan_writes_a_row_per_entity, scan_reads_main_snaks_only, scan_counts_sitelinks_and_quantities,
         parallel_scan_writes_the_same_table, compressed_dump_is_read_through_a_pipe,
         keep_report_names_every_reason, capital_of_normal_rank_is_kept, value_item_of_enough_kept_entities_is_kept,
         instance_of_excluded_class_leaves_the_class_graph, class_the_dump_does_not_hold_is_left_out,
         extract_holds_exactly_the_kept_entities, class_only_line_brings_its_subclass_claims_alone,
         extract_keeps_labels_of_the_chosen_languages_only,
         claim_reaches_the_extract_without_qualifiers_and_references, main_snak_with_a_hash_imports,
         shared_label_resolves_to_the_kept_taxon,
         mul_label_names_an_item_without_an_english_one, what_no_criterion_keeps_is_absent,
         dog_reaches_animal_through_a_class_that_is_no_taxon,
         property_with_a_valueless_first_claim_keeps_its_claims, acceptance_session_passes_on_its_network,
         acceptance_session_reports_a_wrong_expectation, figure_above_its_target_warns_without_failing,
         session_that_stops_early_fails_the_verdict]


def main():
    if len(sys.argv) != 3:
        fail("usage: check_wikidata_subset.py ZELPH TOOL")
    zelph, tool = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    for path in (zelph, tool):
        if not os.path.isfile(path):
            fail(f"no such file: {path}")

    with tempfile.TemporaryDirectory(prefix="zelph_wikidata_subset_") as directory:
        run = Run(zelph, tool, directory)
        for name, arguments, stdout in (("scan", ("scan", "dump.json", "table.tsv", "--criteria", "criteria.json"), None),
                                        ("keep", ("keep", "table.tsv", "criteria.json", "keep.tsv"), "keep.out"),
                                        ("extract", ("extract", "dump.json", "keep.tsv", "subset.json",
                                                     "--criteria", "criteria.json"), None)):
            done = run.tool_run(*arguments, stdout=stdout)
            if done.returncode != 0:
                fail(f"{name} exited {done.returncode}: {done.stdout}{done.stderr}")

        probe = run.zelph_run(PROBE)
        if probe.returncode != 0:
            fail(f"zelph could not import the extract (exit {probe.returncode}):\n{probe.stdout}{probe.stderr}")
        answers = probe_answers(probe.stdout)
        run.probe = lambda: answers

        def verdict(criteria, script, log):
            accept = run.tool_run("accept", criteria, "keep.tsv", "subset.bin", stdout=script)
            if accept.returncode != 0:
                return accept
            session = run.zelph_run(run.read(script))
            with open(run.path(log), "w", encoding="utf-8") as f:
                f.write(session.stdout + session.stderr)
            return run.tool_run("verdict", criteria, log, "subset.bin")
        run.verdict = verdict

        violations, skipped = [], []
        for case in CASES:
            try:
                outcome = case(run)
            except (KeyError, IndexError, TypeError, ValueError, OSError) as e:
                outcome = f"the outputs lack what the case reads: {type(e).__name__}: {e}"
            if outcome and outcome.startswith("skipped:"):
                skipped.append(f"{case.__name__}: {outcome}")
            elif outcome:
                violations.append(f"{case.__name__}: {outcome}")

    for line in skipped:
        print(line)
    if violations:
        for violation in violations:
            print(f"error: {violation}", file=sys.stderr)
        return 1
    print(f"{len(CASES) - len(skipped)} of {len(CASES)} cases hold"
          + (f", {len(skipped)} skipped." if skipped else "."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
