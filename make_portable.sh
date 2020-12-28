#!/usr/bin/env bash

# Example input:
# ./make_portable.sh mycoolbinary
# Or:
# USE_RPATH=1 ./make_portable.sh mycoolbinary
#
# where mycoolbinary is a mach-o object file
# (for example an executable binary or a .dylib)
#
# this script rewrites your file's every environment-specific
# dynamic link (recursively!)
# such that they point to local .dylibs.
# these .dylibs are then copied to a folder lib, next to your binary
#
# by "environment-specific" I mean any link to a .dylib under /usr/local

set -o pipefail

error() {
  local parent_lineno="$1"
  local message="$2"
  local code="${3:-1}"
  if [[ -n "$message" ]] ; then
    echo "Error on or near line ${parent_lineno}: ${message}; exiting with status ${code}"
  else
    echo "Error on or near line ${parent_lineno}; exiting with status ${code}"
  fi
  exit "${code}"
}
trap 'error ${LINENO}' ERR

BINARY="$1"
BINARYDIR=$(dirname "$BINARY")
LIBDIRNAME=${LIBDIRNAME:-lib}
LIBOUTPUTDIR="$BINARYDIR/$LIBDIRNAME"

# assume that our binary is running from:
# CoolBinary.app/Contents/MacOS/coolbinary
# and wants to navigate to:
# CoolBinary.app/Contents/lib/coollibrary.dylib
RPATH_FALLBACK="@loader_path/../$LIBDIRNAME"

if [ -z ${USE_RPATH+x} ]; then
	# don't use rpath
	OBJLOADREL="$RPATH_FALLBACK"
else
	OBJLOADREL="@rpath"
	# define in our binary what it should expand the
	# runtime search path @rpath to
	install_name_tool -add_rpath "$RPATH_FALLBACK" "$BINARY"
fi

# make a lib folder
mkdir -p "$LIBOUTPUTDIR"

# find every LC_LOAD_DYLIB command in the obj file
# filter to just loads under /usr/local
# print the absolute path of each such dylib
get_env_specific_direct_dependencies () {
	# otool -L shows us every LC_LOAD_DYLIB plus LC_ID_DYLIB
	# otool -D shows us just LC_ID_DYLIB
	ALL_DYLIBS=$(otool -L "$1" | awk 'NR>1')
	DYLIB_ID=$(otool -D "$1" | awk 'NR>1')
	if [ -z "$DYLIB_ID" ]; then
		DIRECT_DEPS="$ALL_DYLIBS"
	else
		DIRECT_DEPS=$(echo "$ALL_DYLIBS" | grep -v "$DYLIB_ID")
	fi
	echo "$DIRECT_DEPS" \
	| awk '/\/usr\/local\//,/.dylib/ {print $1}'
}

# lookup LC_LOAD_DYLIB commands in an obj file,
# then follow those loads and ask the same of each
# of its dylibs, recursively
get_env_specific_dependencies_recursive () {
	while read -r obj; do
		[ -z "$obj" ] && continue
		echo "$obj"
		get_env_specific_dependencies_recursive "$obj"
	done < <(get_env_specific_direct_dependencies "$1")
}

DEP_PATHS=$(get_env_specific_dependencies_recursive "$BINARY")

#mkdir -p "$LIBOUTPUTDIR"
# copy each distinct dylib in the dependency tree into our lib folder
echo "$DEP_PATHS" \
| xargs -n1 realpath \
| sort \
| uniq \
| xargs -I'{}' cp {} "$LIBOUTPUTDIR/"

chmod +w "$BINARY" "$LIBOUTPUTDIR"/*.dylib

while read -r obj; do
	[ -z "$obj" ] && continue
	OBJ_LEAF_NAME=$(echo "$obj" | awk -F'/' '{print $NF}')
	# rewrite the install name of this obj file. completely optional.
	# provides good default for future people who link to it.
	install_name_tool -id "$OBJLOADREL/$OBJ_LEAF_NAME" "$obj"

	# iterate over every LC_LOAD_DYLIB command in the objfile
	while read -r load; do
		[ -z "$load" ] && continue
		LOAD_LEAF_NAME=$(echo "$load" | awk -F'/' '{print $NF}')
		# rewrite a LC_LOAD_DYLIB command in this obj file
		# to point relative to @rpath
		install_name_tool -change "$load" "$OBJLOADREL/$LOAD_LEAF_NAME" "$obj"
	done < <(get_env_specific_direct_dependencies "$obj")
done < <(cat <(echo "$BINARY") <(echo "$DEP_PATHS" | awk -F'/' -v l="$LIBOUTPUTDIR" -v OFS='/' '{print l,$NF}'))
