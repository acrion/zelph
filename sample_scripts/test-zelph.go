package main

import "zelph.org/src/lib"
//import "encoding/json"

func main() {

  zelph.Process("peter \"is ancestor of\" paul")
  zelph.Process("paul is ancestor of pius")
  zelph.Run();
}


