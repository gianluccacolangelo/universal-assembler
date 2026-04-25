#!/bin/bash

check_command_available() {
    command -v "$1" > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "go.sh: Error - Required command '$1' is not found. Please install it and try again."
        echo "go.sh: A list of all software dependencies can be found in README.md."
        exit 1
    fi
}

get_job_count() {
    if command -v nproc > /dev/null 2>&1; then
        nproc
        return
    fi

    if [ "$(uname)" = "Darwin" ]; then
        sysctl -n hw.ncpu
        return
    fi

    if command -v getconf > /dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN
        return
    fi

    echo 1
}

pick_cmake_generator() {
    if command -v ninja > /dev/null 2>&1; then
        echo "Ninja"
    else
        echo "Unix Makefiles"
    fi
}

env_flag_enabled() {
    local value
    value=$(printf "%s" "$1" | tr '[:upper:]' '[:lower:]')
    case "$value" in
        ""|0|off|false|no|disabled)
            return 1
            ;;
        *)
            return 0
            ;;
    esac
}

usage() {
    echo "go.sh: Suppose that in the Projects/ directory you have made a project called myproject.cpp."
    echo "go.sh: Usage: $0 <ProjectName> <VideoWidth> <VideoHeight> <Framerate> [optional extra flags]"
    echo "go.sh: Example: $0 myproject 640 360 30 -hx"
    echo "go.sh: Rendering options:"
    echo "  --format=mp4|mov|mkv   Select the output container. Defaults to mkv."
    echo "  --transparent          Preserve alpha and default to mov unless --format is specified."
    echo "  --test                 Render into a temporary output directory and delete it after the run."
    echo "go.sh: Build options:"
    echo "  --surgext=on|off       Enable or disable Surge XT integration. Defaults to on."
    echo "go.sh: Optional Surge XT env vars: SWAPTUBE_ENABLE_SURGE_XT=1 SWAPTUBE_SURGE_XT_SOURCE_DIR=/abs/path/to/surge"
}

# Check for required commands
check_command_available "cmake"
check_command_available "pkg-config"
JOB_COUNT=$(get_job_count)
CMAKE_GENERATOR=$(pick_cmake_generator)
REPO_ROOT=$(pwd)
SURGE_XT_ENABLED=${SWAPTUBE_ENABLE_SURGE_XT:-1}
SURGE_XT_SOURCE_DIR=${SWAPTUBE_SURGE_XT_SOURCE_DIR:-"${REPO_ROOT}/.tmp/vendor/surge-xt"}

if [ $# -eq 1 ] && [ "$1" = "--help" ]; then
    usage
    exit 0
fi

# Check if MicroTeX build exists.
# It is only needed by projects that render LaTeX, and LaTeX rendering can also
# fall back to TeX Live's xelatex plus pdftocairo when those tools are installed.
if [ ! -s "../MicroTeX-master/build/LaTeX" ]; then
    echo "go.sh: Warning - ../MicroTeX-master/build/LaTeX was not found."
    echo "go.sh: latex_to_pix() will fall back to TeX Live if xelatex and pdftocairo are available."
    echo "go.sh: MicroTeX install instructions are available at https://github.com/NanoMichael/MicroTeX"
fi

# Check if the number of arguments is less or more than expected
if [ $# -lt 4 ]; then
    usage
    exit 1
fi

PROJECT_NAME=$1
VIDEO_WIDTH=$2
VIDEO_HEIGHT=$3
FRAMERATE=$4
shift; shift; shift; shift;
# Check that the video dimensions are valid integers
if ! [[ "$VIDEO_WIDTH" =~ ^[0-9]+$ ]] || ! [[ "$VIDEO_HEIGHT" =~ ^[0-9]+$ ]] || ! [[ "$FRAMERATE" =~ ^[0-9]+$ ]]; then
    echo "go.sh: Error - Video width, height, and framerate must be valid integers."
    exit 1
fi
SAMPLERATE=48000

SKIP_RENDER=0
SKIP_SMOKETEST=0
AUDIO_HINTS=0
AUDIO_SFX=0
INVALID_FLAG=0
USE_HIP="FALSE"
TRANSPARENT_RENDER=0
if env_flag_enabled "${SWAPTUBE_TRANSPARENT:-}" || env_flag_enabled "${SWAPTUBE_TRANSPARENT_MOV:-}"; then
    TRANSPARENT_RENDER=1
fi
TEST_RENDER=0
VIDEO_FORMAT="${SWAPTUBE_VIDEO_FORMAT:-}"
VIDEO_FORMAT_EXPLICIT=0

set_compute_language() {
    case "$1" in
        CUDA)
            USE_HIP="FALSE"
            ;;
        HIP)
            USE_HIP="TRUE"
            ;;
        *)
            echo "go.sh: Invalid compute language specified: use CUDA or HIP"
            exit 1
            ;;
    esac
}

set_video_format() {
    VIDEO_FORMAT=$(printf "%s" "$1" | tr '[:upper:]' '[:lower:]')
    VIDEO_FORMAT_EXPLICIT=1
}

set_surge_xt_enabled() {
    local value
    value=$(printf "%s" "$1" | tr '[:upper:]' '[:lower:]')
    case "$value" in
        1|on|true|yes|enabled)
            SURGE_XT_ENABLED=1
            ;;
        0|off|false|no|disabled)
            SURGE_XT_ENABLED=0
            ;;
        *)
            echo "go.sh: Invalid Surge XT option '${1}'. Use on or off."
            exit 1
            ;;
    esac
}

set_surge_xt_enabled "$SURGE_XT_ENABLED"

# Parse flags. Supports legacy short bundles like -hx and newer long flags.
while [ $# -gt 0 ]; do
    case "$1" in
        --transparent)
            TRANSPARENT_RENDER=1
            shift
            ;;
        --test)
            TEST_RENDER=1
            shift
            ;;
        --format=*)
            set_video_format "${1#*=}"
            shift
            ;;
        --format)
            if [ $# -lt 2 ]; then
                echo "go.sh: --format requires one of: mp4, mov, mkv"
                exit 1
            fi
            set_video_format "$2"
            shift 2
            ;;
        --surgext=*|--surge-xt=*)
            set_surge_xt_enabled "${1#*=}"
            shift
            ;;
        --surgext|--surge-xt)
            if [ $# -lt 2 ]; then
                echo "go.sh: --surgext requires on or off"
                exit 1
            fi
            set_surge_xt_enabled "$2"
            shift 2
            ;;
        --no-surgext|--no-surge-xt)
            SURGE_XT_ENABLED=0
            shift
            ;;
        --help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            short_flags="${1#-}"
            shift
            while [ -n "$short_flags" ]; do
                flag="${short_flags%"${short_flags#?}"}"
                short_flags="${short_flags#?}"
                case "$flag" in
                    s)
                        SKIP_RENDER=1
                        ;;
                    n)
                        SKIP_SMOKETEST=1
                        ;;
                    h)
                        AUDIO_HINTS=1
                        ;;
                    x)
                        AUDIO_SFX=1
                        ;;
                    c)
                        if [ -n "$short_flags" ]; then
                            set_compute_language "$short_flags"
                            short_flags=""
                        else
                            if [ $# -lt 1 ]; then
                                echo "go.sh: -c requires CUDA or HIP"
                                exit 1
                            fi
                            set_compute_language "$1"
                            shift
                        fi
                        ;;
                    *)
                        INVALID_FLAG=1
                        ;;
                esac
            done
            ;;
        *)
            echo "go.sh: Unexpected argument: $1"
            INVALID_FLAG=1
            shift
            ;;
    esac
done

# If the final flag is illegal, print an error message and exit
if [ $INVALID_FLAG -eq 1 ]; then
    echo "go.sh: Error - Invalid flag:"
    echo "-s means to only run the smoketest."
    echo "-n means to only run the render"
    echo "-h means to include audio hints."
    echo "-x means to include sound effects."
    echo "-c means to specify compute language (takes arguments \"CUDA\" or \"HIP\")"
    echo "--transparent preserves alpha in a MOV render."
    echo "--test renders into a temporary directory and deletes the output after running."
    echo "--format=mp4|mov|mkv selects the output container."
    echo "--surgext=on|off enables or disables Surge XT integration."
    exit 1
fi

if [ -z "$VIDEO_FORMAT" ]; then
    if [ "$TRANSPARENT_RENDER" -eq 1 ]; then
        VIDEO_FORMAT="mov"
    else
        VIDEO_FORMAT="mkv"
    fi
fi

case "$VIDEO_FORMAT" in
    mp4|mov|mkv)
        ;;
    *)
        echo "go.sh: Invalid --format value '${VIDEO_FORMAT}'. Use mp4, mov, or mkv."
        exit 1
        ;;
esac

if [ "$TRANSPARENT_RENDER" -eq 1 ] && [ "$VIDEO_FORMAT" != "mov" ]; then
    if [ "$VIDEO_FORMAT_EXPLICIT" -eq 1 ]; then
        echo "go.sh: --transparent currently requires --format=mov."
        exit 1
    fi
    VIDEO_FORMAT="mov"
fi

export SWAPTUBE_VIDEO_FORMAT="$VIDEO_FORMAT"
export SWAPTUBE_TRANSPARENT="$TRANSPARENT_RENDER"
export SWAPTUBE_TRANSPARENT_MOV="$TRANSPARENT_RENDER"

# Find the project file in any subdirectory under src/Projects
PROJECT_PATH=$(find src/Projects -type f -name "${PROJECT_NAME}.cpp" 2>/dev/null | head -n 1)

# Check if the desired project exists
if [ -z "$PROJECT_PATH" ]; then
    echo "go.sh: Project ${PROJECT_NAME} does not exist."
    exit 1
fi
PROJECT_PATH=$(cd "$(dirname "$PROJECT_PATH")" && pwd)/$(basename "$PROJECT_PATH")
ACTIVE_PROJECT_FILE="$(pwd)/src/Projects/.active_project.cpp"
ACTIVE_PROJECT_BACKUP=""
OUTPUT_DIR=""

if [ -f "$ACTIVE_PROJECT_FILE" ]; then
    ACTIVE_PROJECT_BACKUP=$(mktemp "${TMPDIR:-/tmp}/swaptube-active-project.XXXXXX.cpp")
    cp "$ACTIVE_PROJECT_FILE" "$ACTIVE_PROJECT_BACKUP"
fi

# Clean up legacy per-run temp files so stale build state cannot target an old source path.
find src/Projects -maxdepth 1 -type f -name '.active_project.*.cpp' -delete

cp "$PROJECT_PATH" "$ACTIVE_PROJECT_FILE"

cleanup() {
    rm -f "$ACTIVE_PROJECT_FILE"
    if [ -n "$ACTIVE_PROJECT_BACKUP" ] && [ -f "$ACTIVE_PROJECT_BACKUP" ]; then
        mv "$ACTIVE_PROJECT_BACKUP" "$ACTIVE_PROJECT_FILE"
    fi
    if [ "$TEST_RENDER" -eq 1 ] && [ -n "$OUTPUT_DIR" ] && [ -d "$OUTPUT_DIR" ]; then
        rm -rf "$OUTPUT_DIR"
    fi
}

trap cleanup EXIT

# Generate a timestamp for this build
OUTPUT_FOLDER_NAME=$(date +"%Y-%m-%d_%H.%M.%S")
if [ "$TEST_RENDER" -eq 1 ]; then
    OUTPUT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/swaptube-${PROJECT_NAME}.XXXXXX")
    echo "go.sh: Test render enabled. Temporary output directory: ${OUTPUT_DIR}"
else
    OUTPUT_DIR="${REPO_ROOT}/out/${PROJECT_NAME}/${OUTPUT_FOLDER_NAME}"
fi
mkdir -p "$OUTPUT_DIR"

INPUT_DIR="media/${PROJECT_NAME}"
mkdir -p "$INPUT_DIR/latex"

if [ "$SURGE_XT_ENABLED" = "1" ]; then
    echo "go.sh: Surge XT support enabled."
    echo "go.sh: Preparing checkout at ${SURGE_XT_SOURCE_DIR}"
    "${REPO_ROOT}/scripts/setup_surge_xt.sh" "${SURGE_XT_SOURCE_DIR}"
    if [ $? -ne 0 ]; then
        echo "go.sh: Surge XT setup failed."
        exit 1
    fi
else
    echo "go.sh: Surge XT support disabled."
fi

echo "go.sh: Building project ${PROJECT_NAME} with output folder name ${OUTPUT_FOLDER_NAME}"
echo "go.sh: Render format ${VIDEO_FORMAT}, transparent ${TRANSPARENT_RENDER}, test ${TEST_RENDER}, Surge XT ${SURGE_XT_ENABLED}"
(
    mkdir -p build
    cd build

    if [ $? -ne 0 ]; then
        echo "go.sh: Unable to create and enter build directory."
        exit 1
    fi

    # Print the command as run
    echo "$0 $*"
    echo ""

    echo "==============================================="
    echo "=================== COMPILE ==================="
    echo "==============================================="
    echo "go.sh: Running \`cmake -G \"$CMAKE_GENERATOR\" ..\` from build directory"

    # Pass the variables to CMake as options
    cmake -G "$CMAKE_GENERATOR" .. \
        -DPROJECT_NAME_MACRO="${PROJECT_NAME}" \
        -DPROJECT_SOURCE_FILE:FILEPATH="${ACTIVE_PROJECT_FILE}" \
        -DAUDIO_HINTS="${AUDIO_HINTS}" \
        -DAUDIO_SFX="${AUDIO_SFX}" \
        -DUSE_HIP="${USE_HIP}" \
        -DSWAPTUBE_ENABLE_SURGE_XT="${SURGE_XT_ENABLED}" \
        -DSURGE_XT_SOURCE_DIR:PATH="${SURGE_XT_SOURCE_DIR}"

    echo "go.sh: Compiling..."
    # build the project
    cmake --build . -j"$JOB_COUNT"

    # Check if the build was successful
    if [ $? -ne 0 ]; then
        echo "go.sh: Build failed. Please check the build errors."
        exit 1
    fi

    echo "==============================================="
    echo "===================== RUN ====================="
    echo "==============================================="

    # Symlink "io_out" to the output directory for this project
    unlink io_out 2>/dev/null
    ln -s "${OUTPUT_DIR}" io_out

    # Symlink "io_in" to the media assets directory
    unlink io_in 2>/dev/null
    ln -s "../${INPUT_DIR}" io_in

    # We redirect stderr to null since FFMPEG's encoder libraries tend to dump all sorts of junk there.
    # Swaptube errors are printed to stdout.

    # Smoketest
    if [ $SKIP_SMOKETEST -eq 0 ]; then
        ./swaptube 160 90 $FRAMERATE $SAMPLERATE smoketest 2>/dev/null
        if [ $? -ne 0 ]; then
            echo "go.sh: Execution failed in smoketest."
            exit 2
        fi
    fi

    # True render
    if [ $SKIP_RENDER -eq 0 ]; then
        # Clear all files from the smoketest
        rm -rf io_out/*
        ./swaptube $VIDEO_WIDTH $VIDEO_HEIGHT $FRAMERATE $SAMPLERATE render 2>/dev/null
        if [ $? -ne 0 ]; then
            echo "go.sh: Execution failed in render."
            exit 2
        fi
    fi

    exit 0
)
RESULT=$?

unlink "build/io_in"
unlink "build/io_out"
if [ "$TEST_RENDER" -eq 0 ] && [ -d "$OUTPUT_DIR" ]; then
    cp "$PROJECT_PATH" "$OUTPUT_DIR/$(basename "$PROJECT_PATH")"
fi

# Play video if compilation succeeded, and not in smoketest-only mode
if [ $RESULT -ne 1 ] && [ $SKIP_RENDER -eq 0 ] && [ "$TEST_RENDER" -eq 0 ]; then
    ./play.sh ${PROJECT_NAME}
fi

exit $RESULT
