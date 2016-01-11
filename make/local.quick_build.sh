#! /bin/bash


JOBS=1

which nproc
if [[ "x$?" == "x0" ]]; then
    JOBS=$(nproc)
fi

export JDK_IMPORT_PATH=/code/jdk/jdk1.8.0_65
export ALT_BOOTDIR=/code/jdk/jdk1.8.0_65

make HOTSPOT_BUILD_JOBS=$JOBS  ARCH_DATA_MODEL=64  fastdebug

