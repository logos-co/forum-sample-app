#!/bin/bash


cd "$(dirname "$0")/.."

echo "Ensure logoscore has been started (or restarted) to process the ./modules"
echo "  ./logos/bin/logoscore -D -m ./modules"

echo "Loading modules..."
module_names="forum_comms forum_app"
for module in $module_names; do
    if [ ! -d "./modules/$module" ]; then
        echo "Module $module not found in ./modules. Please build and install it using ./scripts/t_build_install_all.sh and restart logoscore."
        exit 1
    fi
    ./logos/bin/logoscore load-module $module
done

test="call forum_app getModuleInfo"
echo "# Testing module call: $test"
./logos/bin/logoscore $test

