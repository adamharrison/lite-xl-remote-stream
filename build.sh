#!/usr/bin/env bash

: ${CC=gcc}
: ${BIN=libremote.so}

CFLAGS="$CFLAGS -fPIC -Ilib/lite-xl/resources/include"
LDFLAGS=""

[[ "$@" == "clean" ]] && rm -f *.so *.dll && exit 0
$CC $CFLAGS *.c $@ -shared -o $BIN
