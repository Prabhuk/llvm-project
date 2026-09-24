add_llvm_component_library(LLVMCallGraphSection
  CallGraph.cpp
  LayoutWeights.cpp
  Reachability.cpp
  RecordReader.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/CallGraphSection

  LINK_COMPONENTS
  BinaryFormat
  Support
  )
