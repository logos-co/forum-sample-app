#!/bin/bash

# Creates compile_commands.json for a C++ project using cmake and `nix develop`
# This is used by clangd for code navigation and autocompletion in IDEs

usage() {
    echo "Usage: $0 [nix_project_path]"
    echo "(defaults to current directory)"
}

error() {
    echo "Error: $1"
    echo
    usage
    exit 1
}

if [ -n "$1" ]; then
    if [ ! -d "$1" ]; then
        error "Directory $1 does not exist or is not a directory."
    fi
    cd "$1"
fi

if [ -f "flake.nix" ]; then
    echo "flake.nix found, using cmake from nix develop env to create compile_commands.json..."
    nix develop --command cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -Wno-dev
    echo
    echo "Done."
    echo "Now add the include path for logos-cpp-sdk (LOGOS_CPP_SDK_ROOT above) to a .clangd config file in the project root:"
    echo "
    CompileFlags:
        Add: [-I/nix/store/<hash>-logos-cpp-sdk/include]"
    echo
else
    error "flake.nix not found in the specified directory."
fi
