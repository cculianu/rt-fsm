#!/bin/bash

uname=`uname`

clean=""
config="debug"
case "$1"  in
    release)
        config=release
        ;;
    clean)
        clean="clean"
        ;;
    *)
        ;;
esac

case "$2" in
    release)
        config=release
        ;;
    clean)
        clean="clean"
        ;;
    *)
        ;;
esac

if [ "$uname" == "Darwin" ]; then
    QMAKESPEC=macx-g++
elif [ "$uname" == "Linux" ]; then
    QMAKESPEC=linux-g++
else
    echo "*** uname needs to be Linux or Darwin for this script to work."
    exit 1
fi

if [ "$config" == "release" ]; then
    after="'CONFIG+=release'"
else
    after="'CONFIG+=debug'"
fi

echo qmake -spec "$QMAKESPEC" -after "$after"
qmake -spec "$QMAKESPEC" -after "$after"
echo make $clean
make $clean
echo make -f Makefile.FSMServer config="$config" $clean
make -f Makefile.FSMServer config="$config" $clean
echo make -f Makefile.SoundServer config="$config" $clean
make -f Makefile.SoundServer config="$config" $clean

if [ "$uname" == "Darwin" ] && [ "$clean" != "clean" ]; then
    ./relink_libs_osx.sh
fi




