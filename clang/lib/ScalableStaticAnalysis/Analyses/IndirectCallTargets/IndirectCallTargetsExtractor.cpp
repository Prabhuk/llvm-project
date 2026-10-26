//===- IndirectCallTargetsExtractor.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DynamicRecursiveASTVisitor.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecordLayout.h"
#include "clang/ScalableStaticAnalysis/Analyses/IndirectCallTargets/IndirectCallTargets.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/ExtractorRegistry.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryBuilder.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryExtractor.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ErrorHandling.h"
#include <memory>

using namespace clang;
using namespace ssaf;

using Summary = IndirectCallTargetsSummary;

namespace {

std::string getUSR(const Decl *D) {
  llvm::SmallString<128> Buf;
  if (index::generateUSRForDecl(D, Buf))
    return std::string();
  return std::string(Buf);
}

/// Mirrors FindOverriddenMethodInPrimaryBases in CodeGenModule.cpp: the
/// method whose vtable entry \p MD is placed in, if it overrides one in the
/// primary base chain of its class.
const CXXMethodDecl *findOverriddenInPrimaryBases(const ASTContext &Ctx,
                                                  const CXXMethodDecl *MD) {
  llvm::SmallSetVector<const CXXMethodDecl *, 8> Overridden;
  SmallVector<const CXXMethodDecl *, 8> Worklist(MD->overridden_methods());
  while (!Worklist.empty()) {
    const CXXMethodDecl *OM = Worklist.pop_back_val();
    if (Overridden.insert(OM))
      llvm::append_range(Worklist, OM->overridden_methods());
  }
  if (Overridden.empty())
    return nullptr;
  for (const CXXRecordDecl *RD = MD->getParent()->getDefinition(); RD;) {
    if (RD->isInvalidDecl() || RD->isDependentType())
      return nullptr;
    const CXXRecordDecl *PrimaryBase =
        Ctx.getASTRecordLayout(RD).getPrimaryBase();
    if (!PrimaryBase)
      return nullptr;
    for (const CXXMethodDecl *OM : Overridden)
      if (OM->getParent()->getCanonicalDecl() ==
          PrimaryBase->getCanonicalDecl())
        return OM;
    RD = PrimaryBase->getDefinition();
  }
  return nullptr;
}

void collectBases(const CXXRecordDecl *RD, std::vector<std::string> &Out,
                  llvm::SmallPtrSetImpl<const CXXRecordDecl *> &Seen) {
  RD = RD ? RD->getDefinition() : nullptr;
  if (!RD)
    return;
  for (const CXXBaseSpecifier &B : RD->bases()) {
    const CXXRecordDecl *Base = B.getType()->getAsCXXRecordDecl();
    if (!Base || !Seen.insert(Base->getCanonicalDecl()).second)
      continue;
    Out.push_back(getUSR(Base));
    collectBases(Base, Out, Seen);
  }
}

struct FunctionState {
  Summary::Function Facts;
  llvm::StringSet<> References;
};

class IndirectCallTargetsExtractor final : public TUSummaryExtractor,
                                           ConstDynamicRecursiveASTVisitor {
public:
  explicit IndirectCallTargetsExtractor(TUSummaryBuilder &Builder)
      : TUSummaryExtractor(Builder) {
    ShouldVisitTemplateInstantiations = true;
    ShouldVisitImplicitCode = true;
    ShouldVisitLambdaBody = true;
    ShouldWalkTypesOfTypeLocs = false;
  }

private:
  void HandleTranslationUnit(ASTContext &Ctx) override;

  bool TraverseFunctionDecl(const FunctionDecl *FD) override;
  bool TraverseCXXMethodDecl(const CXXMethodDecl *MD) override;
  bool TraverseCXXConstructorDecl(const CXXConstructorDecl *CD) override;
  bool TraverseCXXDestructorDecl(const CXXDestructorDecl *DD) override;
  bool TraverseCXXConversionDecl(const CXXConversionDecl *CD) override;

  bool VisitCXXRecordDecl(const CXXRecordDecl *RD) override;
  bool VisitCXXMemberCallExpr(const CXXMemberCallExpr *E) override;
  bool VisitCXXOperatorCallExpr(const CXXOperatorCallExpr *E) override;
  bool VisitCXXDeleteExpr(const CXXDeleteExpr *E) override;
  bool VisitDeclRefExpr(const DeclRefExpr *E) override;
  bool VisitMemberExpr(const MemberExpr *E) override;
  bool VisitCXXConstructExpr(const CXXConstructExpr *E) override;
  bool VisitCXXNewExpr(const CXXNewExpr *E) override;
  bool VisitCXXBindTemporaryExpr(const CXXBindTemporaryExpr *E) override;
  bool VisitVarDecl(const VarDecl *VD) override;

  template <typename Fn> bool withFunction(const FunctionDecl *FD, Fn Base);

  std::vector<std::string> mangledNames(const FunctionDecl *FD) const;
  std::string typeIdFor(QualType FnType) const;
  std::string typeIdForCallee(const CXXMethodDecl *MD) const;
  FunctionState *current();
  void addSite(Summary::Site Site);
  void addMethodSite(Summary::SiteKind Kind, const CXXMethodDecl *MD,
                     const Expr *Object, bool IsArrow);
  void addReference(const FunctionDecl *Callee);
  void addDestructorOf(QualType T);

  ASTContext *Ctx = nullptr;
  std::unique_ptr<MangleContext> Mangler;
  SmallVector<const FunctionDecl *, 4> FunctionStack;
  llvm::MapVector<const Decl *, FunctionState> Functions;
  llvm::MapVector<const Decl *, Summary::Class> Classes;
  llvm::StringSet<> ConstructedClasses;
};

} // namespace

std::string IndirectCallTargetsExtractor::typeIdFor(QualType T) const {
  // Mirrors CodeGenModule::CreateMetadataIdentifierImpl for call graph member
  // identifiers with integer normalization.
  if (const auto *FnType = T->getAs<FunctionProtoType>())
    T = Ctx->getFunctionType(
        FnType->getReturnType(), FnType->getParamTypes(),
        FnType->getExtProtoInfo().withExceptionSpec(EST_None));
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  Mangler->mangleCanonicalTypeName(T, OS, /*NormalizeIntegers=*/true);
  OS << ".normalized.member";
  return Out;
}

std::string
IndirectCallTargetsExtractor::typeIdForCallee(const CXXMethodDecl *MD) const {
  // Mirrors CodeGenModule::GetCallGraphFunctionType.
  if (MD->isVirtual())
    while (const CXXMethodDecl *Overridden =
               findOverriddenInPrimaryBases(*Ctx, MD))
      MD = Overridden;
  return typeIdFor(MD->getType());
}

std::vector<std::string>
IndirectCallTargetsExtractor::mangledNames(const FunctionDecl *FD) const {
  std::vector<std::string> Names;
  auto Add = [&](GlobalDecl GD) {
    std::string Name;
    if (Mangler->shouldMangleDeclName(FD)) {
      llvm::raw_string_ostream OS(Name);
      Mangler->mangleName(GD, OS);
    } else {
      Name = FD->getName().str();
    }
    if (llvm::find(Names, Name) == Names.end())
      Names.push_back(std::move(Name));
  };
  if (const auto *CD = dyn_cast<CXXConstructorDecl>(FD)) {
    Add(GlobalDecl(CD, Ctor_Complete));
    Add(GlobalDecl(CD, Ctor_Base));
  } else if (const auto *DD = dyn_cast<CXXDestructorDecl>(FD)) {
    if (DD->isVirtual())
      Add(GlobalDecl(DD, Dtor_Deleting));
    Add(GlobalDecl(DD, Dtor_Complete));
    Add(GlobalDecl(DD, Dtor_Base));
  } else {
    Add(GlobalDecl(FD));
  }
  return Names;
}

FunctionState *IndirectCallTargetsExtractor::current() {
  if (FunctionStack.empty())
    return nullptr;
  return &Functions[FunctionStack.back()->getCanonicalDecl()];
}

template <typename Fn>
bool IndirectCallTargetsExtractor::withFunction(const FunctionDecl *FD,
                                                Fn Base) {
  if (FD->isDependentContext() || FD->isInvalidDecl() ||
      !FD->doesThisDeclarationHaveABody())
    return Base();
  FunctionState &State = Functions[FD->getCanonicalDecl()];
  if (State.Facts.MangledNames.empty()) {
    State.Facts.USR = getUSR(FD);
    State.Facts.MangledNames = mangledNames(FD);
  }
  FunctionStack.push_back(FD);
  if (const auto *DD = dyn_cast<CXXDestructorDecl>(FD)) {
    // A destructor implicitly destroys its members and bases.
    if (const CXXRecordDecl *RD = DD->getParent()->getDefinition()) {
      for (const FieldDecl *F : RD->fields())
        addDestructorOf(F->getType());
      for (const CXXBaseSpecifier &B : RD->bases())
        addDestructorOf(B.getType());
    }
  }
  bool Result = Base();
  FunctionStack.pop_back();
  return Result;
}

bool IndirectCallTargetsExtractor::TraverseFunctionDecl(
    const FunctionDecl *FD) {
  return withFunction(FD, [&] {
    return ConstDynamicRecursiveASTVisitor::TraverseFunctionDecl(FD);
  });
}

bool IndirectCallTargetsExtractor::TraverseCXXMethodDecl(
    const CXXMethodDecl *MD) {
  return withFunction(MD, [&] {
    return ConstDynamicRecursiveASTVisitor::TraverseCXXMethodDecl(MD);
  });
}

bool IndirectCallTargetsExtractor::TraverseCXXConstructorDecl(
    const CXXConstructorDecl *CD) {
  return withFunction(CD, [&] {
    return ConstDynamicRecursiveASTVisitor::TraverseCXXConstructorDecl(CD);
  });
}

bool IndirectCallTargetsExtractor::TraverseCXXDestructorDecl(
    const CXXDestructorDecl *DD) {
  return withFunction(DD, [&] {
    return ConstDynamicRecursiveASTVisitor::TraverseCXXDestructorDecl(DD);
  });
}

bool IndirectCallTargetsExtractor::TraverseCXXConversionDecl(
    const CXXConversionDecl *CD) {
  return withFunction(CD, [&] {
    return ConstDynamicRecursiveASTVisitor::TraverseCXXConversionDecl(CD);
  });
}

bool IndirectCallTargetsExtractor::VisitCXXRecordDecl(const CXXRecordDecl *RD) {
  if (!RD->isThisDeclarationADefinition() || RD->isDependentType() ||
      RD->isInvalidDecl() || !RD->isDynamicClass())
    return true;
  const Decl *Key = RD->getCanonicalDecl();
  if (Classes.count(Key))
    return true;
  Summary::Class &C = Classes[Key];
  C.USR = getUSR(RD);
  llvm::SmallPtrSet<const CXXRecordDecl *, 16> Seen;
  collectBases(RD, C.Bases, Seen);
  for (const CXXMethodDecl *MD : RD->methods()) {
    if (!MD->isVirtual() || MD->isInvalidDecl())
      continue;
    Summary::VirtualMethod VM;
    VM.USR = getUSR(MD);
    VM.MangledNames = mangledNames(MD);
    VM.IsPure = MD->isPureVirtual();
    for (const CXXMethodDecl *OM : MD->overridden_methods())
      VM.Overridden.push_back(getUSR(OM));
    C.VirtualMethods.push_back(std::move(VM));
  }
  return true;
}

void IndirectCallTargetsExtractor::addSite(Summary::Site Site) {
  if (FunctionState *State = current())
    State->Facts.Sites.push_back(std::move(Site));
}

void IndirectCallTargetsExtractor::addReference(const FunctionDecl *Callee) {
  FunctionState *State = current();
  if (!State || !Callee || Callee->isDependentContext() ||
      Callee->isInvalidDecl() || Callee->getBuiltinID() ||
      isa<CXXDeductionGuideDecl>(Callee))
    return;
  for (std::string &Name : mangledNames(Callee))
    State->References.insert(Name);
}

void IndirectCallTargetsExtractor::addDestructorOf(QualType T) {
  if (T.isNull() || T->isDependentType())
    return;
  T = Ctx->getBaseElementType(T);
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition() || RD->isDependentType())
    return;
  if (const CXXDestructorDecl *DD = RD->getDefinition()->getDestructor())
    addReference(DD);
}

void IndirectCallTargetsExtractor::addMethodSite(Summary::SiteKind Kind,
                                                 const CXXMethodDecl *MD,
                                                 const Expr *Object,
                                                 bool IsArrow) {
  Summary::Site Site;
  Site.Kind = Kind;
  Site.TypeId = typeIdForCallee(MD);
  Site.Method = getUSR(MD);
  QualType ObjectType = Object ? Object->getType() : QualType();
  if (!ObjectType.isNull() && IsArrow)
    ObjectType = ObjectType->getPointeeType();
  const CXXRecordDecl *Static =
      ObjectType.isNull() ? nullptr : ObjectType->getAsCXXRecordDecl();
  if (!Static || Site.Method.empty())
    Site.Kind = Summary::SiteKind::Unknown;
  else
    Site.StaticClass = getUSR(Static);
  addSite(std::move(Site));
}

bool IndirectCallTargetsExtractor::VisitCXXMemberCallExpr(
    const CXXMemberCallExpr *E) {
  const Expr *Callee = E->getCallee()->IgnoreParens();
  if (const auto *BO = dyn_cast<BinaryOperator>(Callee);
      BO && BO->isPtrMemOp()) {
    Summary::Site Site;
    Site.Kind = Summary::SiteKind::MemberPointer;
    if (const auto *MPT = BO->getRHS()->getType()->getAs<MemberPointerType>())
      Site.TypeId = typeIdFor(MPT->getPointeeType());
    addSite(std::move(Site));
    return true;
  }
  const auto *ME = dyn_cast<MemberExpr>(Callee);
  if (!ME) {
    // Some other callee form of a member call. Record it so that a consumer
    // abstains for its type.
    Summary::Site Site;
    if (const CXXMethodDecl *MD = E->getMethodDecl())
      Site.TypeId = typeIdForCallee(MD);
    addSite(std::move(Site));
    return true;
  }
  const auto *MD = dyn_cast<CXXMethodDecl>(ME->getMemberDecl());
  // A qualified call (obj.Base::f()) is not dispatched virtually.
  if (!MD || !MD->isVirtual() || ME->hasQualifier())
    return true;
  addMethodSite(Summary::SiteKind::Virtual, MD, ME->getBase(), ME->isArrow());
  return true;
}

bool IndirectCallTargetsExtractor::VisitCXXOperatorCallExpr(
    const CXXOperatorCallExpr *E) {
  const auto *MD = dyn_cast_or_null<CXXMethodDecl>(E->getDirectCallee());
  if (!MD || !MD->isVirtual() || E->getNumArgs() == 0)
    return true;
  addMethodSite(Summary::SiteKind::Virtual, MD, E->getArg(0),
                /*IsArrow=*/false);
  return true;
}

bool IndirectCallTargetsExtractor::VisitCXXDeleteExpr(const CXXDeleteExpr *E) {
  addReference(E->getOperatorDelete());
  addDestructorOf(E->getDestroyedType());
  if (E->isArrayForm())
    return true;
  QualType T = E->getDestroyedType();
  const CXXRecordDecl *RD = T.isNull() ? nullptr : T->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return true;
  const CXXDestructorDecl *DD = RD->getDefinition()->getDestructor();
  if (!DD || !DD->isVirtual())
    return true;
  addMethodSite(Summary::SiteKind::Delete, DD, E->getArgument(),
                /*IsArrow=*/true);
  return true;
}

bool IndirectCallTargetsExtractor::VisitDeclRefExpr(const DeclRefExpr *E) {
  addReference(dyn_cast<FunctionDecl>(E->getDecl()));
  return true;
}

bool IndirectCallTargetsExtractor::VisitMemberExpr(const MemberExpr *E) {
  addReference(dyn_cast<FunctionDecl>(E->getMemberDecl()));
  return true;
}

bool IndirectCallTargetsExtractor::VisitCXXConstructExpr(
    const CXXConstructExpr *E) {
  addReference(E->getConstructor());
  // A complete object of a dynamic class gets that class as its dynamic
  // type. Base and delegating constructions do not create objects.
  if (E->getConstructionKind() == CXXConstructionKind::Complete)
    if (const CXXRecordDecl *RD = E->getConstructor()->getParent();
        RD && RD->hasDefinition() && RD->getDefinition()->isDynamicClass())
      ConstructedClasses.insert(getUSR(RD));
  return true;
}

bool IndirectCallTargetsExtractor::VisitCXXNewExpr(const CXXNewExpr *E) {
  addReference(E->getOperatorNew());
  addReference(E->getOperatorDelete());
  return true;
}

bool IndirectCallTargetsExtractor::VisitCXXBindTemporaryExpr(
    const CXXBindTemporaryExpr *E) {
  addReference(E->getTemporary()->getDestructor());
  return true;
}

bool IndirectCallTargetsExtractor::VisitVarDecl(const VarDecl *VD) {
  if (VD->hasLocalStorage() || VD->isStaticLocal())
    addDestructorOf(VD->getType());
  return true;
}

void IndirectCallTargetsExtractor::HandleTranslationUnit(ASTContext &C) {
  Ctx = &C;
  Mangler.reset(C.createMangleContext());
  TraverseAST(C);

  // Facts of functions and classes without an entity of their own (implicit
  // functions, local entities) are appended to the first summary emitted.
  std::vector<Summary::Function> OrphanFunctions;
  std::vector<Summary::Class> OrphanClasses;
  Summary *Carrier = nullptr;
  auto Emit = [&](std::optional<EntityId> Id, std::unique_ptr<Summary> S) {
    if (!Id) {
      llvm::append_range(OrphanFunctions, S->Functions);
      llvm::append_range(OrphanClasses, S->Classes);
      return;
    }
    Summary Copy = *S;
    auto [Stored, Inserted] = SummaryBuilder.addSummary(*Id, std::move(S));
    auto *Existing = static_cast<Summary *>(Stored);
    if (!Inserted) {
      // Another declaration of the same entity already has a summary.
      llvm::append_range(Existing->Functions, Copy.Functions);
      llvm::append_range(Existing->Classes, Copy.Classes);
    }
    if (!Carrier)
      Carrier = Existing;
  };
  for (auto &[D, Cls] : Classes) {
    auto S = std::make_unique<Summary>();
    S->Classes.push_back(std::move(Cls));
    Emit(addEntity(cast<CXXRecordDecl>(D)), std::move(S));
  }
  for (auto &[D, State] : Functions) {
    for (const auto &Entry : State.References)
      State.Facts.References.push_back(Entry.getKey().str());
    llvm::sort(State.Facts.References);
    // Functions without sites or references are emitted too, so that a
    // consumer can tell a C++ leaf from a function it knows nothing about.
    auto S = std::make_unique<Summary>();
    S->Functions.push_back(std::move(State.Facts));
    Emit(addEntity(cast<FunctionDecl>(D)), std::move(S));
  }
  bool HasConstructed = !ConstructedClasses.empty();
  if (!OrphanFunctions.empty() || !OrphanClasses.empty() || HasConstructed) {
    if (!Carrier) {
      // Nothing carries them yet. Fall back to any function of the
      // translation unit that has an entity.
      for (const Decl *D : Ctx->getTranslationUnitDecl()->decls()) {
        const auto *FD = dyn_cast<FunctionDecl>(D);
        auto Id = FD ? addEntity(FD) : std::nullopt;
        if (!Id)
          continue;
        Carrier = static_cast<Summary *>(
            SummaryBuilder.addSummary(*Id, std::make_unique<Summary>()).first);
        break;
      }
    }
    if (!Carrier) {
      // Only facts without call sites or classes can be left out: a consumer
      // treats a function it knows nothing about as opaque, which costs
      // precision, not soundness.
      bool HasSites = llvm::any_of(
          OrphanFunctions, [](const auto &F) { return !F.Sites.empty(); });
      if (HasSites || !OrphanClasses.empty() || HasConstructed)
        llvm::report_fatal_error(
            "IndirectCallTargets: no entity to carry the facts of this TU");
    } else {
      llvm::append_range(Carrier->Functions, OrphanFunctions);
      llvm::append_range(Carrier->Classes, OrphanClasses);
      for (const auto &Entry : ConstructedClasses)
        Carrier->ConstructedClasses.push_back(Entry.getKey().str());
      llvm::sort(Carrier->ConstructedClasses);
    }
  }
  Functions.clear();
  Classes.clear();
  ConstructedClasses.clear();
}

StringRef clang::ssaf::toString(Summary::SiteKind Kind) {
  switch (Kind) {
  case Summary::SiteKind::Virtual:
    return "virtual";
  case Summary::SiteKind::Delete:
    return "delete";
  case Summary::SiteKind::MemberPointer:
    return "member_pointer";
  case Summary::SiteKind::Unknown:
    return "unknown";
  }
  llvm_unreachable("unknown site kind");
}

std::optional<Summary::SiteKind>
clang::ssaf::siteKindFromString(StringRef Str) {
  return llvm::StringSwitch<std::optional<Summary::SiteKind>>(Str)
      .Case("virtual", Summary::SiteKind::Virtual)
      .Case("delete", Summary::SiteKind::Delete)
      .Case("member_pointer", Summary::SiteKind::MemberPointer)
      .Case("unknown", Summary::SiteKind::Unknown)
      .Default(std::nullopt);
}

static TUSummaryExtractorRegistry::Add<IndirectCallTargetsExtractor>
    RegisterExtractor(Summary::Name,
                      "Extracts facts that narrow indirect call targets");

namespace clang::ssaf {
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int IndirectCallTargetsExtractorAnchorSource = 0;
} // namespace clang::ssaf
