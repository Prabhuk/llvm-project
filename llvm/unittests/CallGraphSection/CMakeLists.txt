set(LLVM_LINK_COMPONENTS
  CallGraphSection
  Support
  )

add_llvm_unittest(CallGraphSectionTests
  CallGraphTest.cpp
  ReachabilityTest.cpp
  RecordReaderTest.cpp
  )

target_link_libraries(CallGraphSectionTests PRIVATE LLVMTestingSupport)
