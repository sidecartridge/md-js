#!/bin/bash

set -euo pipefail

# Ensure an argument is provided
if [ -z "${1:-}" ] || [ -z "${2:-}" ]; then
    echo "Usage: $0 <working_folder> all|release"
    exit 1
fi

working_folder=$1
build_type=$2
target_firmware="target_firmware.h"

stcmd_run() {
    STCMD_NO_TTY=1 ST_WORKING_FOLDER="$working_folder" stcmd "$@"
}

# ST_WORKING_FOLDER=$working_folder/configurator stcmd make $build_type
stcmd_run make "$build_type"

#filename_tos="./dist/SIDECART.TOS"

# Copy the SIDECART.TOS file for testing purposes
#ST_WORKING_FOLDER=$working_folder stcmd cp ./configurator/dist/SIDECART.TOS $filename_tos

filename="./dist/FIRMWARE.IMG"

# Copy the BOOT.BIN file to a ROM size file for testing
stcmd_run cp ./dist/BOOT.BIN "$filename"

# Determine the file size accordingly
filesize=$(stcmd_run stat -c %s "$filename" | tail -n1 | tr -d '\r')
if ! [[ "$filesize" =~ ^[0-9]+$ ]]; then
    echo "Unable to determine firmware size from stcmd output: '$filesize'"
    exit 2
fi

# Size for 64Kbytes in bytes
targetsize=$((64 * 1024))

# Check if the file is larger than 64Kbytes
if [ "$filesize" -gt "$targetsize" ]; then
    echo "The file is already larger than 64Kbytes."
    exit 3
fi

# Resize the file to 64Kbytes
stcmd_run truncate -s "$targetsize" "$filename"

echo "File has been resized."

echo "Creating the firmware.h file."
python3 firmware.py --input=dist/FIRMWARE.IMG --output="$target_firmware" --array_name=target_firmware

cp "$target_firmware" "../../rp/src/include/$target_firmware"
echo "Copied $target_firmware to rp/src/include/$target_firmware"

rm "$target_firmware"
echo "Removed $target_firmware"
