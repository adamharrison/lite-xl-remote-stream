#!/usr/bin/env bash

: ${CC=gcc}
: ${BIN=libremotestream.so}

CFLAGS="$CFLAGS -fPIC -Ilib/lite-xl/resources/include -Ilib/zstd/lib"
LDFLAGS=""

if [[ ! -e "zstd.o" ]]; then
  cd lib/zstd/build/single_file_libs && ./combine.sh -r ../../lib -x legacy/zstd_legacy.h -k zstd.h -o zstd.c zstd-in.c && $CC -c $CFLAGS $@ zstd.c -o ../../../../zstd.o;  cd -
fi

[[ "$@" == "clean" ]] && rm -f *.so *.dll *.o && exit 0
$CC $CFLAGS libremotestream.c zstd.o $@ -shared -o $BIN $LDFLAGS
