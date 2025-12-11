#!/usr/bin/env bash

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <tag>"
    exit 1
fi

tag=$1

modules=$(git config --file .gitmodules --get-regexp path | awk '{ print $2 }')

for module in $modules; do
    if [[ $module == *boost* ]]; then
        echo "Updating $module to tag $tag"
        git -C $module fetch --tags
        git -C $module checkout $tag
        if [ $? -ne 0 ]; then
            echo "Error: Checkout failed for $module on $tag"
            exit 1
        fi
        # Verify, because `git submodule status` does not show all tags of the current commit
        current_commit=$(git -C $module rev-parse HEAD)
        tag_commit=$(git -C $module rev-parse $tag 2>/dev/null)
        if [[ "$current_commit" = "$tag_commit" ]]; then
            echo "Success: $module now at $current_commit ($tag)"
        else
            echo "Error: $module at $current_commit, but $tag points to $tag_commit"
        fi
        git add $module
    fi
done