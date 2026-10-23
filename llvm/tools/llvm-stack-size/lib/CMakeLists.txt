set(LLVM_LINK_COMPONENTS
  Object
  Support
  )

add_llvm_library(LLVMStackSize
  STATIC
  StackSizeEstimator.cpp
  StackSizeEstimator.h
  StackSizeGraph.cpp
  StackSizeGraph.h
  )
