//===- IndirectCallTargets.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Facts that let a whole-program consumer narrow the targets of the indirect
// calls that the call graph section (-fexperimental-call-graph-section)
// matches by type identifier.
//
// For a function, the summary records the symbol names it is emitted under,
// every function its body refers to (the candidates for inlining into it),
// and every member call site of its body that is lowered to an indirect call,
// with the call graph type identifier string the call site gets. For a
// dynamic class, it records the bases and the virtual methods, including
// implicitly declared ones, with what they override and their symbol names.
//
// A consumer must treat a site it cannot narrow as reaching every function of
// the site's type identifier. The extractor never drops facts. A site it
// cannot classify is recorded with kind Unknown, and the facts of functions
// and classes that have no entity of their own (implicit functions, local
// classes) are appended to another summary of the same translation unit.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_INDIRECTCALLTARGETS_INDIRECTCALLTARGETS_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_INDIRECTCALLTARGETS_INDIRECTCALLTARGETS_H

#include "clang/ScalableStaticAnalysis/Core/Model/SummaryName.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/EntitySummary.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>
#include <vector>

namespace clang::ssaf {

struct IndirectCallTargetsSummary final : public EntitySummary {
  static constexpr llvm::StringLiteral Name = "IndirectCallTargets";

  SummaryName getSummaryName() const override {
    return SummaryName(Name.str());
  }

  /// How a call site reaches its callee.
  enum class SiteKind {
    /// A virtual member function call, including explicit destructor calls.
    Virtual,
    /// The virtual deleting destructor call of a delete-expression.
    Delete,
    /// A call through a pointer to member function.
    MemberPointer,
    /// A call the extractor does not classify. Reaches anything of its type.
    Unknown,
  };

  struct Site {
    SiteKind Kind = SiteKind::Unknown;
    /// The call graph type identifier string of the call site, exactly as
    /// CodeGen attaches it in !callee_type metadata.
    std::string TypeId;
    /// USR of the method named at the call site (Virtual, Delete).
    std::string Method;
    /// USR of the static class of the object expression (Virtual, Delete).
    std::string StaticClass;
  };

  struct Function {
    std::string USR;
    /// Mangled names the function is emitted under (a constructor or
    /// destructor has several).
    std::vector<std::string> MangledNames;
    /// Mangled names of every function the body refers to, called or not,
    /// including implicit destructor calls.
    std::vector<std::string> References;
    /// Indirect member call sites of the body.
    std::vector<Site> Sites;
  };

  struct VirtualMethod {
    std::string USR;
    std::vector<std::string> MangledNames;
    /// USRs of the methods this method directly overrides.
    std::vector<std::string> Overridden;
    bool IsPure = false;
  };

  struct Class {
    std::string USR;
    /// USRs of all direct and indirect bases.
    std::vector<std::string> Bases;
    std::vector<VirtualMethod> VirtualMethods;
  };

  /// Normally the entity itself (one function or one class). Facts of
  /// functions and classes without an entity of their own are appended.
  std::vector<Function> Functions;
  std::vector<Class> Classes;

  /// USRs of the dynamic classes whose complete objects this translation
  /// unit constructs. Recorded on one summary per translation unit.
  std::vector<std::string> ConstructedClasses;
};

llvm::StringRef toString(IndirectCallTargetsSummary::SiteKind Kind);
std::optional<IndirectCallTargetsSummary::SiteKind>
siteKindFromString(llvm::StringRef Str);

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_INDIRECTCALLTARGETS_INDIRECTCALLTARGETS_H
