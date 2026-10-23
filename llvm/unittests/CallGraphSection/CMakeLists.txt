set(LLVM_LINK_COMPONENTS
  CallGraphSection
  Object
  ObjectYAML
  Support
  )

add_llvm_unittest(CallGraphSectionTests
  CallGraphTest.cpp
  ELFCallGraphTest.cpp
  RecordReaderTest.cpp
  StackSizeEstimatorTest.cpp
  )

target_link_libraries(CallGraphSectionTests PRIVATE LLVMTestingSupport)
