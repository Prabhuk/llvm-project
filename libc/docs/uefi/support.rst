.. _libc_uefi_support:

===================
Supported Functions
===================

The following functions and headers are supported in UEFI. The tables are
derived from ``libc/config/uefi/entrypoints.txt`` in the source tree, which is
the source of truth for the implemented functions.

The standard streams are connected to the console protocols of the firmware,
see :ref:`libc_uefi_usage` for the details of their behavior. The ``FILE``
functions therefore only operate on ``stdin``, ``stdout`` and ``stderr``: there
is no support for opening files yet.

assert.h
--------

=============  =========
Function Name  Available
=============  =========
__assert_fail  |check|
=============  =========

ctype.h
-------

=============  =========
Function Name  Available
=============  =========
isalnum        |check|
isalpha        |check|
isascii        |check|
isblank        |check|
iscntrl        |check|
isdigit        |check|
isgraph        |check|
islower        |check|
isprint        |check|
ispunct        |check|
isspace        |check|
isupper        |check|
isxdigit       |check|
toascii        |check|
tolower        |check|
toupper        |check|
=============  =========

errno.h
-------

=============  =========
Function Name  Available
=============  =========
errno          |check|
=============  =========

inttypes.h
----------

=============  =========
Function Name  Available
=============  =========
imaxabs        |check|
imaxdiv        |check|
strtoimax      |check|
strtoumax      |check|
=============  =========

stdbit.h
--------

============================  =========
Function Name                 Available
============================  =========
stdc_bit_ceil_uc              |check|
stdc_bit_ceil_ui              |check|
stdc_bit_ceil_ul              |check|
stdc_bit_ceil_ull             |check|
stdc_bit_ceil_us              |check|
stdc_bit_floor_uc             |check|
stdc_bit_floor_ui             |check|
stdc_bit_floor_ul             |check|
stdc_bit_floor_ull            |check|
stdc_bit_floor_us             |check|
stdc_bit_width_uc             |check|
stdc_bit_width_ui             |check|
stdc_bit_width_ul             |check|
stdc_bit_width_ull            |check|
stdc_bit_width_us             |check|
stdc_count_ones_uc            |check|
stdc_count_ones_ui            |check|
stdc_count_ones_ul            |check|
stdc_count_ones_ull           |check|
stdc_count_ones_us            |check|
stdc_count_zeros_uc           |check|
stdc_count_zeros_ui           |check|
stdc_count_zeros_ul           |check|
stdc_count_zeros_ull          |check|
stdc_count_zeros_us           |check|
stdc_first_leading_one_uc     |check|
stdc_first_leading_one_ui     |check|
stdc_first_leading_one_ul     |check|
stdc_first_leading_one_ull    |check|
stdc_first_leading_one_us     |check|
stdc_first_leading_zero_uc    |check|
stdc_first_leading_zero_ui    |check|
stdc_first_leading_zero_ul    |check|
stdc_first_leading_zero_ull   |check|
stdc_first_leading_zero_us    |check|
stdc_first_trailing_one_uc    |check|
stdc_first_trailing_one_ui    |check|
stdc_first_trailing_one_ul    |check|
stdc_first_trailing_one_ull   |check|
stdc_first_trailing_one_us    |check|
stdc_first_trailing_zero_uc   |check|
stdc_first_trailing_zero_ui   |check|
stdc_first_trailing_zero_ul   |check|
stdc_first_trailing_zero_ull  |check|
stdc_first_trailing_zero_us   |check|
stdc_has_single_bit_uc        |check|
stdc_has_single_bit_ui        |check|
stdc_has_single_bit_ul        |check|
stdc_has_single_bit_ull       |check|
stdc_has_single_bit_us        |check|
stdc_leading_ones_uc          |check|
stdc_leading_ones_ui          |check|
stdc_leading_ones_ul          |check|
stdc_leading_ones_ull         |check|
stdc_leading_ones_us          |check|
stdc_leading_zeros_uc         |check|
stdc_leading_zeros_ui         |check|
stdc_leading_zeros_ul         |check|
stdc_leading_zeros_ull        |check|
stdc_leading_zeros_us         |check|
stdc_trailing_ones_uc         |check|
stdc_trailing_ones_ui         |check|
stdc_trailing_ones_ul         |check|
stdc_trailing_ones_ull        |check|
stdc_trailing_ones_us         |check|
stdc_trailing_zeros_uc        |check|
stdc_trailing_zeros_ui        |check|
stdc_trailing_zeros_ul        |check|
stdc_trailing_zeros_ull       |check|
stdc_trailing_zeros_us        |check|
============================  =========

stdio.h
-------

=============  =========
Function Name  Available
=============  =========
asprintf       |check|
fclose         |check|
feof           |check|
ferror         |check|
fflush         |check|
fgetc          |check|
fgets          |check|
fprintf        |check|
fputc          |check|
fputs          |check|
fread          |check|
fscanf         |check|
fseek          |check|
fseeko         |check|
ftell          |check|
ftello         |check|
fwrite         |check|
getc           |check|
getchar        |check|
printf         |check|
putc           |check|
putchar        |check|
puts           |check|
remove         |check|
scanf          |check|
snprintf       |check|
sprintf        |check|
sscanf         |check|
stderr         |check|
stdin          |check|
stdout         |check|
ungetc         |check|
vasprintf      |check|
vfprintf       |check|
vfscanf        |check|
vprintf        |check|
vscanf         |check|
vsnprintf      |check|
vsprintf       |check|
vsscanf        |check|
=============  =========

stdlib.h
--------

==================  =========
Function Name       Available
==================  =========
_Exit               |check|
abort               |check|
abs                 |check|
aligned_alloc       |check|
at_quick_exit       |check|
atexit              |check|
atof                |check|
atoi                |check|
atol                |check|
atoll               |check|
bsearch             |check|
calloc              |check|
div                 |check|
exit                |check|
free                |check|
labs                |check|
ldiv                |check|
llabs               |check|
lldiv               |check|
malloc              |check|
malloc_usable_size  |check|
memalignment        |check|
qsort               |check|
qsort_r             |check|
quick_exit          |check|
rand                |check|
realloc             |check|
srand               |check|
strtod              |check|
strtof              |check|
strtol              |check|
strtold             |check|
strtoll             |check|
strtoul             |check|
strtoull            |check|
==================  =========

string.h
--------

===============  =========
Function Name    Available
===============  =========
memccpy          |check|
memchr           |check|
memcmp           |check|
memcpy           |check|
memmem           |check|
memmove          |check|
mempcpy          |check|
memrchr          |check|
memset           |check|
memset_explicit  |check|
stpcpy           |check|
stpncpy          |check|
strcasestr       |check|
strcat           |check|
strchr           |check|
strchrnul        |check|
strcmp           |check|
strcoll          |check|
strcpy           |check|
strcspn          |check|
strdup           |check|
strerror         |check|
strerror_r       |check|
strlcat          |check|
strlcpy          |check|
strlen           |check|
strncat          |check|
strncmp          |check|
strncpy          |check|
strndup          |check|
strnlen          |check|
strpbrk          |check|
strrchr          |check|
strsep           |check|
strspn           |check|
strstr           |check|
strtok           |check|
strtok_r         |check|
strxfrm          |check|
===============  =========

strings.h
---------

=============  =========
Function Name  Available
=============  =========
bcmp           |check|
bcopy          |check|
bzero          |check|
ffs            |check|
ffsl           |check|
ffsll          |check|
index          |check|
rindex         |check|
strcasecmp     |check|
strncasecmp    |check|
=============  =========
