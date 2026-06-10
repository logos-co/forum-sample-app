#!/bin/bash

ensure_installed() {
    local dir="$1"
    local path="$2"
    if [ ! -d "$dir" ]; then
        echo "$dir not found. Building..."
        nix build "$path" --out-link "./$dir"
    fi
}


echo "Checking Logos Package Manager CLI..."
ensure_installed "pm" 'github:logos-co/logos-package-manager/tutorial-v3#cli'

cd "$(dirname "$0")/.."

# ensure modules dir exists, ignore warning if it already exists
mkdir modules 2>/dev/null || true

echo "Build and install each module to modules dir"
module_dirs="forum-comms forum-app-module"
for module in $module_dirs; do
    echo "Building $module..."
    cd ./$module
    nix build .#lgx
    cd -
    echo "Adding install files to ./modules..."
    ./pm/bin/lgpm --modules-dir ./modules install --file $module/result/*.lgx
done

echo
echo "Dirs in modules:" `ls ./modules`
echo
echo "You can now (re)start Logos Core to process the built modules using:"
echo "    ./logos/bin/logoscore -D -m ./modules"
echo "To stop a previously started logoscore: ./logos/bin/logoscore stop"
echo "NB: you may need to manually stop logos module processes if you still find them running."
echo
echo "Then run ./scripts/t_integration_tests.sh."