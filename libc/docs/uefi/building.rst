.. _libc_uefi_building:

======================
Building libc for UEFI
======================

Building LLVM libc for UEFI
===========================

This document will present recipes to build the LLVM C library for UEFI.
UEFI builds use the same :ref:`cross build<full_cross_build>` support as
the other targets. However, the UEFI target has the restriction that it *must*
be built with an up-to-date ``clang`` compiler. This is because UEFI support
in ``clang`` is still an experimental feature.

LLVM libc can be built for UEFI on ``x86_64`` and ``aarch64`` CPUs. The latter
needs a ``clang`` that supports the ``aarch64-unknown-uefi`` target. There is
no support for ``riscv64`` yet, as the target is not enabled in ``clang``.

Once you have finished building, refer to :ref:`libc_uefi_usage` to get started
with the newly built C library.

Standard runtimes build
-----------------------

The simplest way to build for UEFI is to use the existing LLVM runtimes
support. This will automatically handle bootstrapping an up-to-date ``clang``
compiler and use it to build the C library. The following CMake invocation
will instruct it to build the ``libc`` runtime targeting ``x86_64`` CPUs.

.. code-block:: sh

   $> cd llvm-project  # The llvm-project checkout
   $> mkdir build
   $> cd build
   $> cmake ../llvm -G Ninja                                                 \
      -DLLVM_ENABLE_PROJECTS="clang;lld"                                     \
      -DCMAKE_BUILD_TYPE=<Debug|Release>  \ # Select build type
      -DCMAKE_INSTALL_PREFIX=<PATH>       \ # Where the libraries will live
      -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-uefi-llvm                  \
      -DLLVM_RUNTIME_TARGETS=x86_64-unknown-uefi-llvm                        \
      -DRUNTIMES_x86_64-unknown-uefi-llvm_LLVM_ENABLE_RUNTIMES=libc          \
      -DRUNTIMES_x86_64-unknown-uefi-llvm_LLVM_LIBC_FULL_BUILD=true          \
   $> ninja install

We need ``clang`` to build the UEFI C library and ``lld`` to link UEFI PE
executables, so we enable them in ``LLVM_ENABLE_PROJECTS``. We then set
``RUNTIMES_<triple>_LLVM_ENABLE_RUNTIMES`` to enable ``libc`` for the UEFI
targets. Replace ``x86_64`` with ``aarch64`` in the triple to build for
``aarch64`` CPUs instead; several ``LLVM_RUNTIME_TARGETS`` can be listed to
build both at once.

Standalone runtimes build
-------------------------

When a suitable ``clang`` is already available, the C library can be built on
its own from the ``runtimes`` directory. This is considerably faster as it does
not build the compiler. The following CMake invocation builds the ``libc``
runtime targeting ``aarch64`` CPUs with the given compiler.

.. code-block:: sh

   $> cd llvm-project  # The llvm-project checkout
   $> mkdir build
   $> cd build
   $> cmake ../runtimes -G Ninja                                             \
      -DLLVM_ENABLE_RUNTIMES=libc                                            \
      -DLLVM_LIBC_FULL_BUILD=ON                                              \
      -DCMAKE_BUILD_TYPE=<Debug|Release>  \ # Select build type
      -DCMAKE_INSTALL_PREFIX=<PATH>       \ # Where the libraries will live
      -DCMAKE_C_COMPILER=<PATH>/clang     \ # A clang with UEFI support
      -DCMAKE_CXX_COMPILER=<PATH>/clang++                                    \
      -DLLVM_USE_LINKER=lld                                                  \
      -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-unknown-uefi-llvm                 \
      -DLIBC_TARGET_TRIPLE=aarch64-unknown-uefi-llvm                         \
   $> ninja install

The library is installed as ``lib/<triple>/libc.a`` together with the startup
object ``lib/<triple>/crt1.o`` and the headers in ``include/<triple>``.

Configuration options
---------------------

The UEFI build uses the configuration in ``libc/config/uefi/config.json``,
which tunes the library for firmware applications: it is single threaded,
``malloc`` is served from the boot services pool and the rarely used parts of
``printf`` and ``scanf`` are left out. Any option can still be overridden on
the CMake command line, see :ref:`configure`. Two options worth knowing about
when size matters are ``LIBC_CONF_PRINTF_DISABLE_FLOAT``, which drops the
floating point conversions along with their tables, and
``LIBC_CONF_PRINTF_FLOAT_TO_STR_USE_DYADIC_FLOAT``, which keeps them but trades
some accuracy for much smaller tables.
