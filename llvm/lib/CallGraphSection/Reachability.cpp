//===- Reachability.cpp - Call graph reachability from roots --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/Reachability.h"
#include "llvm/ADT/SmallVector.h"

using namespace llvm;
using namespace llvm::callgraph;

BitVector
llvm::callgraph::computeReachable(const CallGraph &Graph,
                                  const ReachabilityOptions &Options) {
  BitVector Reachable(Graph.size());
  if (Graph.empty())
    return Reachable;

  SmallVector<NodeId, 0> Worklist;
  auto Visit = [&](NodeId N) {
    if (!Reachable.test(N)) {
      Reachable.set(N);
      Worklist.push_back(N);
    }
  };

  Visit(Graph.externalCallingNode());
  if (Options.UntypedAddressTakenAreRoots)
    Visit(Graph.untypedTargetsNode());
  for (NodeId N : Options.AdditionalRoots)
    if (Graph.isValid(N))
      Visit(N);

  while (!Worklist.empty()) {
    NodeId N = Worklist.pop_back_val();
    for (NodeId Callee : Graph.callees(N))
      Visit(Callee);
  }
  return Reachable;
}
