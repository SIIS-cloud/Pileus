dnl The libsanlock_client.so library
dnl
dnl Copyright (C) 2012-2013 Red Hat, Inc.
dnl
dnl This library is free software; you can redistribute it and/or
dnl modify it under the terms of the GNU Lesser General Public
dnl License as published by the Free Software Foundation; either
dnl version 2.1 of the License, or (at your option) any later version.
dnl
dnl This library is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
dnl Lesser General Public License for more details.
dnl
dnl You should have received a copy of the GNU Lesser General Public
dnl License along with this library.  If not, see
dnl <http://www.gnu.org/licenses/>.
dnl

AC_DEFUN([LIBVIRT_CHECK_SANLOCK],[
  LIBVIRT_CHECK_LIB([SANLOCK], [sanlock_client], [sanlock_init], [sanlock.h])

  if test "x$with_sanlock" = "xyes" ; then
    AC_CHECK_DECLS([SANLK_INQ_WAIT], [sanlock_inq_wait=1], [sanlock_inq_wait=0], [[
      #include <stdint.h>
      #include <sanlock_admin.h>
    ]])

    old_cppflags="$CPPFLAGS"
    old_libs="$LIBS"
    CPPFLAGS="$CPPFLAGS $SANLOCK_CFLAGS"
    LIBS="$LIBS $SANLOCK_LIBS"

    AC_CHECK_LIB([sanlock_client], [sanlock_killpath],
                 [sanlock_killpath=yes], [sanlock_killpath=no])
    if test "x$sanlock_killpath" = "xyes" ; then
      AC_DEFINE_UNQUOTED([HAVE_SANLOCK_KILLPATH], 1,
        [whether Sanlock supports sanlock_killpath])
    fi

    AC_CHECK_LIB([sanlock_client], [sanlock_inq_lockspace],
               [sanlock_inq_lockspace=yes], [sanlock_inq_lockspace=no])
    if test "x$sanlock_inq_lockspace" = "xyes" && \
       test $sanlock_inq_wait = 1; then
      AC_DEFINE_UNQUOTED([HAVE_SANLOCK_INQ_LOCKSPACE], 1,
        [whether sanlock supports sanlock_inq_lockspace])
    fi

    CPPFLAGS="$old_cppflags"
    LIBS="$old_libs"
  fi
])

AC_DEFUN([LIBVIRT_RESULT_SANLOCK],[
  LIBVIRT_RESULT_LIB([SANLOCK])
])
