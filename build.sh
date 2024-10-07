#!/usr/bin/env bash

: ${CC=gcc}
: ${BIN=libremote.so}

CFLAGS="$CFLAGS -fPIC -Ilib/lite-xl/resources/include"
LDFLAGS=""

if [[ ! -e "zstd.o" ]]; then
  cd lib/zstd/build/single_file_libs && ./combine.sh -r ../../lib -x legacy/zstd_legacy.h -k zstd.h -o zstd.c zstd-in.c && gcc -c $CFLAGS $@ zstd.c -o ../../../../zstd.o;  cd -
fi

[[ "$@" == "clean" ]] && rm -f *.so *.dll *.o && exit 0
$CC $CFLAGS libremote.c zstd.o $@ -shared -o $BIN $LDFLAGS
