#! /bin/bash

DIR=$( pushd $(dirname $BASH_SOURCE[0]) >> /dev/null && pwd -P && popd >> /dev/null  )


pushd $DIR > /dev/null

find $DIR \
    -path "${DIR}/src/os_cpu/windows_x86/*" -prune -o \
    -path "${DIR}/src/os_cpu/solaris_sparc/*" -prune -o \
    -path "${DIR}/src/os_cpu/linux_sparc/*" -prune -o \
    -path "${DIR}/src/os_cpu/bsd_x86/*" -prune -o \
    -path "${DIR}/src/os_cpu/solaris_x86/*" -prune -o \
    -path "${DIR}/src/os_cpu/linux_ppc/*" -prune -o \
    -path "${DIR}/src/os_cpu/bsd_zero/*" -prune -o \
    -path "${DIR}/src/os_cpu/aix_ppc/*" -prune -o \
    -path "${DIR}/src/os_cpu/linux_zero/*" -prune -o \
    -path "${DIR}/src/cpu/sparc/*" -prune -o \
    -path "${DIR}/src/cpu/ppc/*" -prune -o \
    -path "${DIR}/src/cpu/zero/*" -prune -o \
    -path "${DIR}/src/os/solaris/*" -prune -o \
    -path "${DIR}/src/os/bsd/*" -prune -o \
    -path "${DIR}/src/os/windows/*" -prune -o \
    -path "${DIR}/src/os/aix/*" -prune -o \
    -path "${DIR}/src/os/posix/*" -prune -o \
    -path "$DIR/src/*32*" -prune -o \
    -name "*.[ch]" -print -o \
    -name "*.[ch]pp" -print  > ${DIR}/cscope.files



cscope -b -q -k

popd > /dev/null
