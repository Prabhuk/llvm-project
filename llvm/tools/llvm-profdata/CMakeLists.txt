set(LLVM_LINK_COMPONENTS
  Core
  Object
  ProfileData
  Support
  )

set(LLVM_ENABLE_RTTI ON)
set(CMAKE_CXX_STANDARD 17)
add_llvm_tool(llvm-profdata
  llvm-profdata.cpp

  DEPENDS
  intrinsics_gen
  )

target_link_libraries(llvm-profdata PRIVATE LLVMDebuginfod)
