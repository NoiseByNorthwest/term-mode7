#!/bin/bash

cd $(dirname $0)

if [[ ! -x build/term-mode7 ]]
then
    ./build.sh
fi

if [[ $TERM == @(xterm|screen) ]]
then
    export TERM=$TERM-256color
fi

./build/term-mode7
