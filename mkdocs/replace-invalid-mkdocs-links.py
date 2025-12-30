#!/usr/bin/env python
import os
import re


# Zelph Markdown Link Processor
# Purpose: Process markdown files in subdirectories of 'docs/' to convert invalid internal links to Wikidata URLs.
#          Only processes files in subdirectories (e.g., docs/tree/*.md), not direct files in docs/.
#          For each subdirectory, collects existing .md files in that subdir only (since subdirs are independent).
#          Replaces [text](file.md) with [text](https://www.wikidata.org/wiki/file) or Property: if file starts with P.
#          Skips links with :// (external). Modifies files in-place if changes are made.
# Usage: python replace_invalid_mkdocs_links.py

def process_subdirectory(subdir_path):
    # Get all markdown files in this subdirectory only
    md_files = {f for f in os.listdir(subdir_path) if f.endswith('.md')}

    print(f"Processing subdirectory: {subdir_path}")
    print(f"Found {len(md_files)} markdown files.")

    files_processed = 0
    files_changed = 0
    links_changed = 0

    for filename in sorted(md_files):
        files_processed += 1

        # Progress indicator every 200 files
        if files_processed % 200 == 0:
            print(f"... processed {files_processed} files so far in {subdir_path}")

        file_path = os.path.join(subdir_path, filename)

        # Read the file content
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()

        original = content
        changes = 0

        # Regex to match markdown links: [text](file.md)
        md_link_regex = re.compile(r'\[(.*?)\]\((.*?)\.md\)', re.DOTALL)

        # Find all matches
        matches = md_link_regex.finditer(content)

        # We need to replace in reverse order to avoid offset issues
        replacements = []

        for match in matches:
            full_match = match.group(0)
            link_text = match.group(1)
            file = match.group(2) + '.md'  # Reconstruct the file part

            # Skip if it's an external link (contains ://)
            if '://' in file:
                continue

            # Check if the referenced file exists in this subdirectory's md_files set
            if file not in md_files:
                # Extract the base ID (without .md)
                base_id = file[:-3]  # Remove .md

                # Determine Wikidata URL
                if base_id.startswith('P'):
                    wikidata_url = f"https://www.wikidata.org/wiki/Property:{base_id}"
                else:
                    wikidata_url = f"https://www.wikidata.org/wiki/{base_id}"

                # Construct replacement: [link_text](wikidata_url)
                replacement = f"[{link_text}]({wikidata_url})"

                # Record the replacement (start, end, replacement text)
                replacements.append((match.start(), match.end(), replacement))
                changes += 1

        # Apply replacements in reverse order to preserve positions
        if changes > 0:
            new_content = list(content)
            for start, end, repl in reversed(replacements):
                new_content[start:end] = repl
            content = ''.join(new_content)

        # Special replacements as workarounds
        temp_content = content
        content = content.replace('[!](https://www.wikidata.org/wiki/!)',
                                  '[Q363948](https://www.wikidata.org/wiki/Q363948)')
        if content != temp_content:
            changes += 1

        temp_content = content
        content = content.replace('(!.md)', '(https://www.wikidata.org/wiki/Q363948)')
        if content != temp_content:
            changes += 1

        # Save changes if any (including specials)
        if changes > 0:
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(content)

        files_changed += 1
        links_changed += changes

        print(f"Modified {changes} items in {filename}")

        print(f"\nSubdirectory Summary for {subdir_path}:")
        print(f"Total files processed: {files_processed}")
        print(f"Files changed: {files_changed}")
        print(f"Total items modified: {links_changed}")


def main():
    base_dir = 'docs'

    # Find all subdirectories in docs/
    subdirs = [os.path.join(base_dir, d) for d in os.listdir(base_dir) if os.path.isdir(os.path.join(base_dir, d))]

    if not subdirs:
        print("No subdirectories found in 'docs/'.")
        return

    for subdir_path in sorted(subdirs):
        process_subdirectory(subdir_path)

    print("\nAll subdirectories processed.")
    print("Ready.")


if __name__ == "__main__":
    main()
