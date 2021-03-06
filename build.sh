#!/bin/bash

cd $(dirname $0)

if [[ ! -e /usr/include/ncurses.h ]]
then
    echo 'You must install libncurses development package' >&2
    exit 1
fi

if [[ ! $(which convert) ]]
then
    echo 'You must install ImageMagick' >&2
    exit 1
fi

set -ex

if [[ ! -d assets/maps ]]
then
    mkdir assets/maps

    maps=(
        mariocircuit-1
        ghostvalley-3
        bowsercastle-3
        chocoisland-2
        mariocircuit-3
        donutplains-3
        koopabeach-1
        vanillalake-2
    )

    for map in ${maps[@]}
    do
        wget http://www.mariouniverse.com/wp-content/img/maps/snes/smk/${map}.png
        convert ${map}.png -colors 256 -compress none BMP3:${map}.bmp
        # this second convert is required for some image to force removal of RLE compression
        convert ${map}.bmp -colors 256 -compress none BMP3:${map}.bmp
        mv ${map}.bmp assets/maps
        rm ${map}.png
    done
fi

gcc -Werror -O3 main.c -lncurses -lm -o build/term-mode7
