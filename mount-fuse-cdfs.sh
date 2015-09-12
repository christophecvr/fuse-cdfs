#!/bin/bash

FUSE_CDFS_COMMAND="$PWD/fuse-cdfs"


TMP_device="$1"

if [ -z "$TMP_device" ]; then

    echo "No device as parameter. Usage $(basename $0) /dev/sd0 mountdirectory for example."
    exit

fi

if [ ! -e "$TMP_device" ]; then

    echo "Device $TMP_device does not exist."
    exit

fi

# no check here for presence of media...

TMP_mountpoint="$2"

if [ -z "$TMP_mountpoint" ]; then

    install --directory "$TMP_mountpoint"

fi


if [ -z "$TMP_cache" ]; then

    TMP_cache="/var/cache/fuse-cdfs"

fi

if [ ! -d "$TMP_cache" ]; then

    install --directory "$TMP_cache"

fi

if [ ! -x "$FUSE_CDFS_COMMAND" ]; then

    echo "Command fuse-cdfs not execuatble. Cannot continue."
    exit

fi


$FUSE_CDFS_COMMAND --device="$TMP_device" --cache-directory="$TMP_cache" --logging=3 --cachebackend=sqlite "$TMP_mountpoint"
