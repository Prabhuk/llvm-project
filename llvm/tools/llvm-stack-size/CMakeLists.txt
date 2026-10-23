set(LLVM_LINK_COMPONENTS
  Object
  Option
  Support
  )

set(LLVM_TARGET_DEFINITIONS Opts.td)
tablegen(LLVM Opts.inc -gen-opt-parser-defs)
add_public_tablegen_target(StackSizeOptsTableGen)

add_llvm_tool(llvm-stack-size
  llvm-stack-size.cpp
  DEPENDS
  StackSizeOptsTableGen
  GENERATE_DRIVER
  )

add_subdirectory(lib)
target_link_libraries(llvm-stack-size PRIVATE LLVMStackSize)
