//===- IndirectCallTargetsFormat.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/IndirectCallTargets/IndirectCallTargets.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "clang/ScalableStaticAnalysis/Core/Support/ErrorBuilder.h"
#include "llvm/Support/JSON.h"
#include <memory>

using namespace llvm;
using namespace clang;
using namespace ssaf;

using Summary = IndirectCallTargetsSummary;

static json::Array toArray(const std::vector<std::string> &Strings) {
  json::Array A;
  for (const std::string &S : Strings)
    A.push_back(S);
  return A;
}

static json::Object serialize(const EntitySummary &ES,
                              JSONFormat::EntityIdToJSONFn) {
  const auto &S = static_cast<const Summary &>(ES);
  json::Array Functions;
  for (const Summary::Function &F : S.Functions) {
    json::Array Sites;
    for (const Summary::Site &Site : F.Sites)
      Sites.push_back(json::Object{
          {"kind", toString(Site.Kind)},
          {"type_id", Site.TypeId},
          {"method", Site.Method},
          {"static_class", Site.StaticClass},
      });
    Functions.push_back(json::Object{
        {"usr", F.USR},
        {"mangled_names", toArray(F.MangledNames)},
        {"references", toArray(F.References)},
        {"sites", std::move(Sites)},
    });
  }
  json::Array Classes;
  for (const Summary::Class &C : S.Classes) {
    json::Array Methods;
    for (const Summary::VirtualMethod &VM : C.VirtualMethods)
      Methods.push_back(json::Object{
          {"usr", VM.USR},
          {"mangled_names", toArray(VM.MangledNames)},
          {"overridden", toArray(VM.Overridden)},
          {"pure", VM.IsPure},
      });
    Classes.push_back(json::Object{
        {"usr", C.USR},
        {"bases", toArray(C.Bases)},
        {"virtual_methods", std::move(Methods)},
    });
  }
  return json::Object{
      {"functions", std::move(Functions)},
      {"classes", std::move(Classes)},
      {"constructed_classes", toArray(S.ConstructedClasses)},
  };
}

static Error malformed(StringRef Field) {
  return ErrorBuilder::create(std::errc::invalid_argument,
                              "malformed IndirectCallTargets field '{0}'",
                              Field)
      .build();
}

static Error readStrings(const json::Object &Obj, StringRef Field,
                         std::vector<std::string> &Out) {
  const json::Array *A = Obj.getArray(Field);
  if (!A)
    return malformed(Field);
  for (const json::Value &V : *A) {
    std::optional<StringRef> Str = V.getAsString();
    if (!Str)
      return malformed(Field);
    Out.push_back(Str->str());
  }
  return Error::success();
}

static Error readString(const json::Object &Obj, StringRef Field,
                        std::string &Out) {
  std::optional<StringRef> Str = Obj.getString(Field);
  if (!Str)
    return malformed(Field);
  Out = Str->str();
  return Error::success();
}

static Error readSites(const json::Object &Obj,
                       std::vector<Summary::Site> &Out) {
  const json::Array *Sites = Obj.getArray("sites");
  if (!Sites)
    return malformed("sites");
  for (const json::Value &V : *Sites) {
    const json::Object *SiteObj = V.getAsObject();
    if (!SiteObj)
      return malformed("sites");
    Summary::Site Site;
    std::string Kind;
    if (Error E = readString(*SiteObj, "kind", Kind))
      return E;
    std::optional<Summary::SiteKind> K = siteKindFromString(Kind);
    if (!K)
      return malformed("kind");
    Site.Kind = *K;
    if (Error E = readString(*SiteObj, "type_id", Site.TypeId))
      return E;
    if (Error E = readString(*SiteObj, "method", Site.Method))
      return E;
    if (Error E = readString(*SiteObj, "static_class", Site.StaticClass))
      return E;
    Out.push_back(std::move(Site));
  }
  return Error::success();
}

static Expected<std::unique_ptr<EntitySummary>>
deserialize(const json::Object &Obj, EntityIdTable &,
            JSONFormat::EntityIdFromJSONFn) {
  auto S = std::make_unique<Summary>();
  const json::Array *Functions = Obj.getArray("functions");
  if (!Functions)
    return malformed("functions");
  for (const json::Value &V : *Functions) {
    const json::Object *FnObj = V.getAsObject();
    if (!FnObj)
      return malformed("functions");
    Summary::Function F;
    if (Error E = readString(*FnObj, "usr", F.USR))
      return std::move(E);
    if (Error E = readStrings(*FnObj, "mangled_names", F.MangledNames))
      return std::move(E);
    if (Error E = readStrings(*FnObj, "references", F.References))
      return std::move(E);
    if (Error E = readSites(*FnObj, F.Sites))
      return std::move(E);
    S->Functions.push_back(std::move(F));
  }
  const json::Array *Classes = Obj.getArray("classes");
  if (!Classes)
    return malformed("classes");
  for (const json::Value &V : *Classes) {
    const json::Object *ClassObj = V.getAsObject();
    if (!ClassObj)
      return malformed("classes");
    Summary::Class C;
    if (Error E = readString(*ClassObj, "usr", C.USR))
      return std::move(E);
    if (Error E = readStrings(*ClassObj, "bases", C.Bases))
      return std::move(E);
    const json::Array *Methods = ClassObj->getArray("virtual_methods");
    if (!Methods)
      return malformed("virtual_methods");
    for (const json::Value &MV : *Methods) {
      const json::Object *MethodObj = MV.getAsObject();
      if (!MethodObj)
        return malformed("virtual_methods");
      Summary::VirtualMethod VM;
      if (Error E = readString(*MethodObj, "usr", VM.USR))
        return std::move(E);
      if (Error E = readStrings(*MethodObj, "mangled_names", VM.MangledNames))
        return std::move(E);
      if (Error E = readStrings(*MethodObj, "overridden", VM.Overridden))
        return std::move(E);
      std::optional<bool> Pure = MethodObj->getBoolean("pure");
      if (!Pure)
        return malformed("pure");
      VM.IsPure = *Pure;
      C.VirtualMethods.push_back(std::move(VM));
    }
    S->Classes.push_back(std::move(C));
  }
  if (Error E = readStrings(Obj, "constructed_classes", S->ConstructedClasses))
    return std::move(E);
  return std::move(S);
}

namespace {
struct IndirectCallTargetsJSONFormatInfo final : JSONFormat::FormatInfo {
  IndirectCallTargetsJSONFormatInfo()
      : JSONFormat::FormatInfo(SummaryName(Summary::Name.str()), serialize,
                               deserialize) {}
};
} // namespace

static llvm::Registry<JSONFormat::FormatInfo>::Add<
    IndirectCallTargetsJSONFormatInfo>
    RegisterFormatInfo(Summary::Name,
                       "JSON Format info for IndirectCallTargets summary");

namespace clang::ssaf {
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int IndirectCallTargetsJSONFormatAnchorSource = 0;
} // namespace clang::ssaf
