@0xabcdef1234567890;  # Unique ID

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("zelph::network");

struct ProbPair {
  hash @0 :UInt64;
  prob @1 :Float64;  # long double approx as double
}

struct AdjPair {
  node @0 :UInt64;
  adj @1 :List(UInt64);  # sorted adjacency_set
}

struct AdjChunk {
  which @0 :Text;  # "left" or "right"
  chunkIndex @1 :UInt32;
  pairs @2 :List(AdjPair);
}

struct ZelphImpl {
  probabilities @0 :List(ProbPair);
  last @1 :UInt64;
  lastVar @2 :UInt64;
  deprecated0 @3 :Void;  # was: nodeCount
  deprecated1 @4 :Void;  # was: nameOfNode (non-chunked, v1 only)
  deprecated2 @5 :Void;  # was: nodeOfName (non-chunked, v1 only)
  deprecated3 @6 :Void;  # was: formatFactLevel
  leftChunkCount @7 :UInt32;
  rightChunkCount @8 :UInt32;
  nameOfNodeChunkCount @9 :UInt32;
  nodeOfNameChunkCount @10 :UInt32;
  # True in every file an engine writes that gives the collections written
  # with a rule ids of their own (template ids). An older engine wrote them
  # under counter ids, as data, and leaves this false. So does a save of a
  # network in which a whole-file load found rules an older engine saved:
  # they keep those ids.
  templateIds @11 :Bool;
  # True in every file an engine writes that names a negated rule condition
  # through the fact ((P) ~ negation). An older engine wrote that fact as a
  # tag on the pattern P and named P directly, leaving this false. So does a
  # save of a network in which a whole-file load found such tags: they keep
  # their older meaning.
  negationConditions @12 :Bool;
  # The number of AnnotationChunk messages that come after the
  # nodeOfName chunks. 0 in every file produced by an older engine:
  # that engine reads the four sections and halts, thus loading a newer
  # file without its annotations.
  annotationChunkCount @13 :UInt32;
  # Was: singleObjectFacts, true if the network held no ground fact with
  # more than one object. No engine reads or writes it now that the record
  # below has superseded it. It stays a Bool to ensure no later field
  # takes its bit in the header of a file that set it.
  deprecated4 @14 :Bool;
  # The predicates for which the network might hold a ground fact involving
  # multiple objects (Zelph::may_hold_wider_statements), listed in
  # ascending order. Present and empty in every file of a network that held
  # no such fact. Absent in a file from an older engine and in a save of a
  # network whose record is unknown: the load then interprets a statement
  # within a condition, regardless of its predicate, as one that a fact
  # with multiple objects could potentially state.
  widerStatementPredicates @15 :List(UInt64);
}

struct NamePair {
  key @0 : UInt64;
  value @1 : Text;
}

struct NameChunk {
  lang @0 : Text;
  chunkIndex @1 : UInt32;
  pairs @2 : List(NamePair);
}

struct NodeNamePair {
  key @0 : Text;
  value @1 : UInt64;
}

struct NodeNameChunk {
  lang @0 : Text;
  chunkIndex @1 : UInt32;
  pairs @2 : List(NodeNamePair);
}

# One key of a single node, along with its values arranged in the
# order they were added.
struct AnnotationEntry {
  node @0 : UInt64;
  key @1 : Text;
  values @2 : List(Text);
}

struct AnnotationChunk {
  chunkIndex @0 : UInt32;
  entries @1 : List(AnnotationEntry);
}
