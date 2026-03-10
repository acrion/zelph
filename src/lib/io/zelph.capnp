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