.. _libc_uefi_usage:

===================
Using libc for UEFI
===================

Using the UEFI C library
========================

Once you have finished :ref:`building<libc_uefi_building>` the UEFI C library
it can be used to run libc functions inside of UEFI applications. Currently,
not all C standard functions are supported in UEFI. Consult the :ref:`list of
supported functions<libc_uefi_support>` for a comprehensive list.

.. note::
   LLVM-libc support for UEFI is currently in the early bring-up phase. For
   the current source of truth on implemented functions, please refer to
   ``libc/config/uefi/entrypoints.txt`` in the source tree.

Building an application
-----------------------

A UEFI application is a PE/COFF executable with the ``EFI_APPLICATION``
subsystem, which ``clang`` produces when given a UEFI target triple. The C
library ships a startup object, ``crt1.o``, that provides the ``EfiMain``
entry point the firmware calls and invokes the application's ``main``. The
following command builds a self contained application from ``hello.c`` with
the library installed in ``<PATH>``.

.. code-block:: sh

   $> clang --target=x86_64-unknown-uefi -fuse-ld=lld -nostdlib              \
      -isystem <PATH>/include/x86_64-unknown-uefi-llvm                        \
      <PATH>/lib/x86_64-unknown-uefi-llvm/crt1.o hello.c                      \
      <PATH>/lib/x86_64-unknown-uefi-llvm/libc.a -o hello.efi

The resulting ``hello.efi`` can be started from the UEFI shell, from a boot
option, or as the default boot application of a FAT partition when installed
as ``EFI/BOOT/BOOTX64.EFI`` (``BOOTAA64.EFI`` on ``aarch64``). The ``Uefi.h``
header exposes the firmware tables and protocols for applications that want to
talk to the firmware directly.

Program startup and termination
-------------------------------

The arguments of ``main`` are taken from the load options of the image. When
the application is started from the UEFI shell, or from a boot option that
carries a textual argument, the load options hold the command line as a UCS-2
string. ``crt1.o`` converts it to UTF-8 and splits it into arguments at
whitespace, with double quotes grouping words that contain whitespace as
described by the UEFI Shell specification. ``argv[0]`` is the command as it was
typed. Applications started without options get ``argc == 0`` with an ``argv``
that only holds the terminating null pointer. The environment is always empty.

Returning from ``main`` or calling ``exit`` runs the ``atexit`` handlers and
then exits the image through the boot services. A zero exit status is reported
to the firmware as ``EFI_SUCCESS``, any other value as ``EFI_ABORTED``.
``abort`` and failed assertions exit the image with ``EFI_ABORTED`` without
running the handlers.

.. note::
   Static constructors are not run at startup yet. Applications should not
   rely on dynamic initialization of global objects.

Standard streams
----------------

The standard streams are connected to the console protocols of the system
table: ``stdin`` reads from ``ConIn`` while ``stdout`` and ``stderr`` write to
``ConOut`` and ``StdErr``. Output is converted to the UCS-2 strings the console
expects, and the carriage returns the console needs are inserted in front of
line feeds, so applications can write ``"\n"`` as usual. Input is line
buffered to give applications a terminal like experience: the characters are
echoed as they are typed, backspace erases the last one, enter hands the line
over with a ``'\n'`` at its end and Ctrl-D at the start of a line signals
end-of-file. Only ASCII is exchanged with the console for now.

The ``FILE`` functions only operate on the three standard streams, which
cannot be seeked and report ``ESPIPE`` when asked to. There is no support for
opening files yet.

Memory allocation
-----------------

``malloc`` and the related functions allocate from the boot services pool as
``EfiLoaderData``. The allocations have the fundamental alignment of the
target; ``aligned_alloc`` supports larger alignments. Allocating memory is not
possible once ``ExitBootServices`` has been called.
