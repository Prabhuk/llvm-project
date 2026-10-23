set(LLVM_LINK_COMPONENTS
  Object
  Support
  )

add_llvm_unittest(StackSizeTests
  StackSizeEstimatorTest.cpp
  )
target_link_libraries(StackSizeTests PRIVATE LLVMStackSize LLVMTestingSupport)
