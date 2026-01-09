@0xabcdef1234567891;  # Unique ID for index schema

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("zelph::network");

struct IndexPair {
  key @0 :Text;  # std::string
  value @1 :UInt64;  # std::streamoff as uint64
}

struct IndexChunk {
  chunkIndex @0 :UInt32;
  pairs @1 :List(IndexPair);
}

struct WikidataIndex {
  chunkCount @0 :UInt32;  # Number of chunks for the index map
}
