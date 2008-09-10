#!/bin/bash

uname=`uname`

clean=""
config="debug"
qmake="1"
servers=""

while true; do

    case "$1"  in
        release)
            config=release
            ;;
        clean)
            clean="clean"
            ;;
        noqmake)
            qmake=""
            ;;
        servers)
            servers="1"
            ;;
        *)
            ;;
    esac

    if shift; then 
        continue;
    else
        break;
    fi

done

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

if [ -z "$servers" ]; then

    if [ -n "$qmake" ]; then
        echo qmake -spec "$QMAKESPEC" -after "$after"
        qmake -spec "$QMAKESPEC" -after "$after" || exit 1
    fi
    echo make $clean
    make $clean || exit 1

fi
echo make -f Makefile.FSMServer config="$config" $clean
make -f Makefile.FSMServer config="$config" $clean || exit 1
echo make -f Makefile.SoundServer config="$config" $clean
make -f Makefile.SoundServer config="$config" $clean || exit 1

if [ "$uname" == "Darwin" ] && [ "$clean" != "clean" ]; then
    ./relink_libs_osx.sh
fi




