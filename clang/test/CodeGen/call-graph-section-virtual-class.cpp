// Tests that virtual calls are identified after the method they name, and that
// virtual methods, destructors and thunks carry the identifiers of every method
// they override besides the member function type identifier. Methods of one
// class with the same type, such as a destructor and a void() method, get
// separate identifiers.

// RUN: %clang_cc1 -triple x86_64-unknown-linux -fexperimental-call-graph-section \
// RUN:   -O1 -disable-llvm-passes -mconstructor-aliases -emit-llvm -o - %s \
// RUN:   | FileCheck %s

struct Base {
  virtual int vf(char *a);
  virtual void reset();
  virtual ~Base();
};

struct Derived : Base {
  int vf(char *a) override;
  ~Derived() override;
};

struct Other {
  virtual void g();
};

// Multiple inheritance: the entry of Other::g in the vtable of Multi for its
// Other base is filled by a thunk.
struct Multi : Base, Other {
  void g() override;
};

// A dynamic class with an inline trivial destructor body: its base object
// destructor is emitted rather than replaced by the destructor of its base
// class, which is not reached by the virtual calls typed after Trivial.
struct Trivial : Base {
  ~Trivial() override {}
};

// The base object destructors of dynamic classes are not aliases of the
// destructors of their base classes.
// CHECK-NOT: @_ZN7DerivedD2Ev = {{.*}}alias
// CHECK: @_ZN7DerivedD1Ev = unnamed_addr alias void (ptr), ptr @_ZN7DerivedD2Ev
// CHECK-NOT: @_ZN7DerivedD2Ev = {{.*}}alias

// CHECK-LABEL: define {{.*}} @_ZN4Base2vfEPc(
// CHECK-SAME: !callgraph [[MEMBER_VF:![0-9]+]] !callgraph [[BASE_VF:![0-9]+]] {
int Base::vf(char *a) { return 0; }

// CHECK-LABEL: define {{.*}} @_ZN4Base5resetEv(
// CHECK-SAME: !callgraph [[MEMBER_VOID:![0-9]+]] !callgraph [[BASE_RESET:![0-9]+]] {
void Base::reset() {}

// CHECK-LABEL: define {{.*}} @_ZN4BaseD2Ev(
// CHECK-SAME: !callgraph [[MEMBER_VOID]] !callgraph [[BASE_DTOR:![0-9]+]] {
// CHECK-LABEL: define {{.*}} @_ZN4BaseD0Ev(
// CHECK-SAME: !callgraph [[MEMBER_VOID]] !callgraph [[BASE_DEL:![0-9]+]] {
Base::~Base() {}

// CHECK-LABEL: define {{.*}} @_ZN7Derived2vfEPc(
// CHECK-SAME: !callgraph [[MEMBER_VF]] !callgraph [[DERIVED_VF:![0-9]+]] !callgraph [[BASE_VF]] {
int Derived::vf(char *a) { return 1; }

// CHECK-LABEL: define {{.*}} @_ZN7DerivedD2Ev(
// CHECK-SAME: !callgraph [[MEMBER_VOID]] !callgraph [[DERIVED_DTOR:![0-9]+]] !callgraph [[BASE_DTOR]] {
// CHECK-LABEL: define {{.*}} @_ZN7DerivedD0Ev(
// CHECK-SAME: !callgraph [[MEMBER_VOID]] !callgraph [[DERIVED_DEL:![0-9]+]] !callgraph [[BASE_DEL]] {
Derived::~Derived() {}

// CHECK-LABEL: define {{.*}} @_ZN5Multi1gEv(
// CHECK-SAME: !callgraph [[MEMBER_VOID]] !callgraph [[MULTI_G:![0-9]+]] !callgraph [[OTHER_G:![0-9]+]] {
// CHECK-LABEL: define {{.*}} @_ZThn8_N5Multi1gEv(
// CHECK-SAME: !callgraph [[MEMBER_VOID]] !callgraph [[MULTI_G]] !callgraph [[OTHER_G]] {
void Multi::g() {}

// CHECK-LABEL: define {{.*}} @_Z5callsP4BaseP7DerivedP5OtherMS_FiPcE(
void calls(Base *B, Derived *D, Other *O, int (Base::*MemPtr)(char *)) {
  // CHECK: call noundef i32 %{{.*}}, !callee_type [[BASE_VF_CT:![0-9]+]]
  B->vf(nullptr);
  // CHECK: call noundef i32 %{{.*}}, !callee_type [[DERIVED_VF_CT:![0-9]+]]
  D->vf(nullptr);
  // CHECK: call void %{{.*}}, !callee_type [[OTHER_G_CT:![0-9]+]]
  O->g();
  // CHECK: call void %{{.*}}, !callee_type [[BASE_RESET_CT:![0-9]+]]
  B->reset();
  // Calls through member function pointers keep the member function type
  // identifier.
  // CHECK: call noundef i32 %{{.*}}, !callee_type [[MEMBER_VF_CT:![0-9]+]]
  (B->*MemPtr)(nullptr);
  // CHECK: call void %{{.*}}, !callee_type [[BASE_DTOR_CT:![0-9]+]]
  B->~Base();
  // CHECK: call void %{{.*}}, !callee_type [[BASE_DEL_CT:![0-9]+]]
  delete B;
}

void trivial() { Trivial T; }

// CHECK-LABEL: define {{.*}} @_ZN7TrivialD2Ev(
// CHECK-SAME: !callgraph [[MEMBER_VOID]] !callgraph [[TRIVIAL_DTOR:![0-9]+]] !callgraph [[BASE_DTOR]] {

// CHECK-DAG: [[MEMBER_VF]] = !{!"_ZTSFiPcE.member"}
// CHECK-DAG: [[BASE_VF]] = !{!"_ZN4Base2vfEPc.virtual"}
// CHECK-DAG: [[DERIVED_VF]] = !{!"_ZN7Derived2vfEPc.virtual"}
// CHECK-DAG: [[MEMBER_VOID]] = !{!"_ZTSFvvE.member"}
// CHECK-DAG: [[BASE_RESET]] = !{!"_ZN4Base5resetEv.virtual"}
// CHECK-DAG: [[BASE_DTOR]] = !{!"_ZN4BaseD1Ev.virtual"}
// CHECK-DAG: [[BASE_DEL]] = !{!"_ZN4BaseD0Ev.virtual"}
// CHECK-DAG: [[DERIVED_DTOR]] = !{!"_ZN7DerivedD1Ev.virtual"}
// CHECK-DAG: [[DERIVED_DEL]] = !{!"_ZN7DerivedD0Ev.virtual"}
// CHECK-DAG: [[TRIVIAL_DTOR]] = !{!"_ZN7TrivialD1Ev.virtual"}
// CHECK-DAG: [[MULTI_G]] = !{!"_ZN5Multi1gEv.virtual"}
// CHECK-DAG: [[OTHER_G]] = !{!"_ZN5Other1gEv.virtual"}
// CHECK-DAG: [[BASE_VF_CT]] = !{[[BASE_VF]]}
// CHECK-DAG: [[DERIVED_VF_CT]] = !{[[DERIVED_VF]]}
// CHECK-DAG: [[OTHER_G_CT]] = !{[[OTHER_G]]}
// CHECK-DAG: [[MEMBER_VF_CT]] = !{[[MEMBER_VF]]}
// CHECK-DAG: [[BASE_RESET_CT]] = !{[[BASE_RESET]]}
// CHECK-DAG: [[BASE_DTOR_CT]] = !{[[BASE_DTOR]]}
// CHECK-DAG: [[BASE_DEL_CT]] = !{[[BASE_DEL]]}
