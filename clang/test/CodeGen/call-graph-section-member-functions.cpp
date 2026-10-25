// Non-static member functions, and the virtual and pointer-to-member calls
// that reach them, get type identifiers separate from free functions with the
// same signature. An ordinary function pointer cannot hold a non-static member
// function, and a virtual or pointer-to-member call cannot reach a free
// function, so the split loses no edge. Static member functions and the
// function a captureless lambda converts to are free functions.

// RUN: %clang_cc1 -triple aarch64-unknown-fuchsia -emit-llvm -o - \
// RUN:   -fexperimental-call-graph-section %s | FileCheck %s

struct A {
  virtual void f();
  virtual ~A();
  void g();
  static void s();
};
struct B {
  virtual void f();
  virtual ~B();
};
struct C : A, B {
  void f() override;
  ~C() override;
};

// CHECK-DAG: define {{.*}} @_ZN1A1fEv({{.*}} !callgraph [[MEMBER:![0-9]+]]
// CHECK-DAG: define {{.*}} @_ZN1AD0Ev({{.*}} !callgraph [[MEMBER]]
// CHECK-DAG: define {{.*}} @_ZN1A1gEv({{.*}} !callgraph [[MEMBER]]
// CHECK-DAG: define {{.*}} @_ZN1A1sEv({{.*}} !callgraph [[FREE:![0-9]+]]
// CHECK-DAG: define {{.*}} @_ZThn8_N1C1fEv({{.*}} !callgraph [[MEMBER]]
// CHECK-DAG: define {{.*}} @_Z6freefnv({{.*}} !callgraph [[FREE]]
void A::f() {}
A::~A() {}
void A::g() {}
void A::s() {}
void B::f() {}
B::~B() {}
void C::f() {}
C::~C() {}
void freefn() {}

void (*volatile FreeFnPtr)();

// CHECK-LABEL: define {{.*}} @_Z3useP1AP1BMS_FvvE(
void use(A *a, B *b, void (A::*pmf)()) {
  // CHECK: call void %{{.*}}, !callee_type [[MEMBER_CT:![0-9]+]]
  a->f();
  // CHECK: call void %{{.*}}, !callee_type [[MEMBER_CT]]
  delete a;
  // CHECK: call void %{{.*}}, !callee_type [[MEMBER_CT]]
  (a->*pmf)();
  // CHECK: call void %{{.*}}, !callee_type [[FREE_CT:![0-9]+]]
  FreeFnPtr();
  void (*sp)() = &A::s;
  // CHECK: call void %{{.*}}, !callee_type [[FREE_CT]]
  sp();
  auto l = [] {};
  void (*lp)() = l;
  // CHECK: call void %{{.*}}, !callee_type [[FREE_CT]]
  lp();
  // CHECK: call void %{{.*}}, !callee_type [[MEMBER_CT]]
  b->f();
}

// CHECK-DAG: [[MEMBER]] = !{!"_ZTSFvvE.member"}
// CHECK-DAG: [[FREE]] = !{!"_ZTSFvvE"}
// CHECK-DAG: [[MEMBER_CT]] = !{[[MEMBER]]}
// CHECK-DAG: [[FREE_CT]] = !{[[FREE]]}
