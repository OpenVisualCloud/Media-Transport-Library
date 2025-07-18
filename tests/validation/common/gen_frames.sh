#!/bin/bash

# In order to properly generate the videos using FFmpeg, go through the instructions available on:
# https://github.com/OpenVisualCloud/Media-Communications-Mesh/blob/main/docs/LatencyMeasurement.md#build-and-install-steps

FONT_SIZE_DIVISOR=10

resolutions=("3840x2160" "1920x1080" "1280x720" "640x360")
pixel_formats=("yuv422p" "yuv422p10le")

function check_if_ffmpeg_properly_configured(){
    cmd_result=$(ffmpeg -version)
    echo $cmd_result | grep "\--enable-libfreetype" > /dev/null; libfreetype=$?
    echo $cmd_result | grep "\--enable-libharfbuzz" > /dev/null; libharfbuzz=$?
    echo $cmd_result | grep "\--enable-libfontconfig" > /dev/null; libfontconfig=$?

    if (( libfreetype + libharfbuzz + libfontconfig == 0 )); then
        echo "All required FFmpeg libraries are properly enabled."
    else
        echo "FFmpeg does not have proper libraries enabled. Issues may be encountered while generating the videos."
    fi
}

function generate_frames_yuv(){
    local resolution=$1
    local pixel_format=$2
    local fontsize=$3
    local x=$4
    local y=$5
    ffmpeg -an -y -f lavfi -i testsrc=d=30:s="${resolution}":r=120,format="${pixel_format}" \
    -vf "drawtext=fontsize=${fontsize}: text='%{eif\:n\:d\:5}': start_number=1: x=${x}: y=${y}: fontcolor=white: box=1: boxcolor=black: boxborderw=10" \
    -f rawvideo -video_size "${resolution}" -pix_fmt "${pixel_format}" "${resolution}"_"${pixel_format}".yuv
}

function generate_frames_mp4(){
    local resolution=$1
    local pixel_format=$2
    local fontsize=$3
    local x=$4
    local y=$5
    ffmpeg -an -y -f lavfi -i testsrc=d=30:s="${resolution}":r=120,format="${pixel_format}" \
    -vf "drawtext=fontsize=${fontsize}: text='%{eif\:n\:d\:5}': start_number=1: x=${x}: y=${y}: fontcolor=white: box=1: boxcolor=black: boxborderw=10" \
    -vcodec mpeg4 -qscale:v 3 "${resolution}"_"${pixel_format}".mp4
}

function compress_frames(){
    local input_file=$1
    tar czf "${input_file}.tgz" "${input_file}"
    rm "${input_file}"
}


# Main section:
check_if_ffmpeg_properly_configured

for resolution in "${resolutions[@]}"; do
  for pixel_format in "${pixel_formats[@]}"; do
    # width=$(echo $resolution | cut -d'x' -f1)
    height=$(echo "${resolution}" | cut -d'x' -f2)
    fontsize=$(( height / FONT_SIZE_DIVISOR )) # Calculate fontsize based on video height
    x=0
    y=20

    generate_frames_yuv "${resolution}" "${pixel_format}" "${fontsize}" "${x}" "${y}"
    md5sum "${resolution}_${pixel_format}.yuv"
    compress_frames "${resolution}_${pixel_format}.yuv"

    # Uncomment the following lines if you want to generate MP4 files as well
    # generate_frames_mp4 "${resolution}" "${pixel_format}" "${fontsize}" "${x}" "${y}"
    done
done
