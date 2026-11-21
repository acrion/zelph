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
  # Was: nodeCount, the total number of nodes jointly held by the left and
  # right sections. No engine reads or writes it anymore, since the chunk
  # directory now records the entries of each chunk. It stays a UInt64 to
  # guarantee that no later field takes its word in the header of a file
  # that set it.
  deprecated5 @16 :UInt64;
  # True in every file an engine generates that places a ChunkDirectory
  # message immediately after its header. An older engine wrote the first
  # chunk there and left this value set to false.
  chunkDirectory @17 :Bool;
  # Whether the network may hold a rule's own collection, and whether it may
  # hold a set pattern constructed outside a rule
  # (Zelph::may_hold_set_patterns). An engine that is aware of this field
  # writes it into every file; a load of a file lacking this reads the two
  # from the ids of the nodes it reads.
  patternKinds @18 :PatternKinds;
  # The template-variable store (Zelph::var_in_closure): every node whose
  # statement holds a variable, along with those variables, and every node
  # whose rule text holds one that its statement lacks. Present in a file
  # only if the saving session's store was complete, or if no node of the
  # file is a variable, in which case the record has no entry; a load that
  # finds it enters it and draws answers from it for each node within the
  # file. A load of a file lacking it reads each loaded node relevant to
  # the question via the reconstruction walk, unless the file also lacks
  # patternKinds and the load has read the whole of it without finding a
  # variable among its nodes.
  variableRecord @19 :VariableRecord;
}

struct PatternKinds {
  ruleCollections @0 :Bool;
  setPatterns @1 :Bool;
}

struct VariableRecord {
  # In sequence according to node, ordered from lowest to highest.
  nodes @0 :List(VariableEntry);
  # In the order of increasing value.
  ruleText @1 :List(UInt64);
}

struct VariableEntry {
  node @0 :UInt64;
  # In the order of increasing value.
  variables @1 :List(UInt64);
}

# The location of each chunk within a file, and the quantity of entries it
# holds. A save writes it as the message immediately following the header,
# unpacked: its size is then determined solely by the number of chunks, so
# the save writes it with zeros preceding the chunks and updates it after
# they are written. A load sizes the node map from it, once it matches the
# file.
struct ChunkDirectory {
  # The sections appear in the order of the file, each containing its
  # chunks in their respective order. A section that an engine does not
  # recognize still contributes its part of the file.
  sections @0 :List(DirectorySection);
}

struct DirectorySection {
  section @0 :FileSection;
  chunks @1 :List(DirectoryChunk);
}

enum FileSection {
  left @0;
  right @1;
  nameOfNode @2;
  nodeOfName @3;
  annotations @4;
}

struct DirectoryChunk {
  # Bytes from the start of the file to the chunk's packed message.
  offset @0 :UInt64;
  # The number of bytes contained in that message.
  length @1 :UInt64;
  # Pairs of the chunk, or its entries within the annotation
  # section.
  entries @2 :UInt64;
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
