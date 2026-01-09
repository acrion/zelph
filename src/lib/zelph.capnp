@0xabcdef1234567890;  # Unique ID

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("zelph::network");

struct NodeValuePair {  # For _name_of_node: key=Node (UInt64), value=Text (UTF-8 wstring)
  key @0 :UInt64;
  value @1 :Text;
}

struct ValueNodePair {  # For _node_of_name: key=Text (UTF-8 wstring), value=Node (UInt64)
  key @0 :Text;
  value @1 :UInt64;
}

struct NameLangMap {  # For _name_of_node
  lang @0 :Text;  # std::string (language)
  pairs @1 :List(NodeValuePair);
}

struct NodeLangMap {  # For _node_of_name
  lang @0 :Text;  # std::string (language)
  pairs @1 :List(ValueNodePair);
}

struct ProbPair {
  hash @0 :UInt64;
  prob @1 :Float64;  # long double approx as double
}

struct AdjPair {
  node @0 :UInt64;
  adj @1 :List(UInt64);  # sorted adjacency_set
}

struct AdjChunk {  # New: For chunked _left/_right
  which @0 :Text;  # "left" or "right"
  chunkIndex @1 :UInt32;
  pairs @2 :List(AdjPair);
}

struct ZelphImpl {  # Main message, now without direct left/right
  probabilities @0 :List(ProbPair);
  last @1 :UInt64;
  lastVar @2 :UInt64;
  nodeCount @3 :UInt64;
  nameOfNode @4 :List(NameLangMap);
  nodeOfName @5 :List(NodeLangMap);
  formatFactLevel @6 :Int32;
  leftChunkCount @7 :UInt32;  # Number of chunks for left
  rightChunkCount @8 :UInt32;  # Number of chunks for right
}