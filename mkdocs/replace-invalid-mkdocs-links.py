#!/usr/bin/env python
import os
import re
import sys

# zelph Markdown Link Processor
# Purpose: In the generated Wikidata report tree, a link target that names no file
#          in the same directory is a Wikidata identifier rather than a page.
#          Replaces [text](Q42.md) with [text](https://www.wikidata.org/wiki/Q42),
#          and Property: for a target starting with P. Modifies files in place.
# Usage: python replace-invalid-mkdocs-links.py [directory ...]
#
# THE ASSUMPTION ONLY HOLDS INSIDE THE GENERATED TREE. When this script was
# written, docs/missing-superclass/ was the only subdirectory of docs/. It is not
# any more: docs/math/ and docs/internals/ are hand-written, and their
# cross-directory links (../janet.md, ../quickstart.md, ../internals/performance.md)
# are exactly the shape this script converts. Pointing it at docs/ therefore
# rewrites real documentation into Wikidata URLs, and mkdocs --strict stays green
# because the results are valid external links. Hence the explicit directory
# argument, and the guards in is_wikidata_target().


def process_subdirectory(subdir_path):
    # Get all markdown files in this subdirectory only
    md_files = {f for f in os.listdir(subdir_path) if f.endswith(".md")}

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
        temp_file_path = file_path + ".tmp"  # Temporary file for writing changes

        changes = 0
        line_count = 0

        # Neither the text nor the target may contain a bracket, a parenthesis or
        # whitespace. With the original `.*?` the match could span from one link
        # across a whole clause to the next ".md)", so the replacement swallowed a
        # sentence of prose into a URL.
        md_link_regex = re.compile(r"\[([^\][]*)\]\(([^()\s]*?)\.md\)")

        with (
            open(file_path, "r", encoding="utf-8") as f_in,
            open(temp_file_path, "w", encoding="utf-8") as f_out,
        ):
            for line in f_in:
                line_count += 1
                new_line = line

                # Special replacements as workarounds (applied to all lines)
                temp_line = new_line
                new_line = new_line.replace("[!](!.md)", "!")
                if new_line != temp_line:
                    changes += 1

                # Apply regex to the line
                matches = list(md_link_regex.finditer(new_line))

                # Build new line by collecting parts
                new_line_parts = []
                last_end = 0

                for match in matches:
                    # Always append the part before this match
                    new_line_parts.append(new_line[last_end : match.start()])

                    link_text = match.group(1)
                    file_base = match.group(2)
                    file = file_base + ".md"
                    full_match = match.group(0)

                    # Skip if it's an external link (contains ://)
                    if "://" in file_base:
                        # Append original match
                        new_line_parts.append(full_match)
                    else:
                        # Check if the referenced file exists in this subdirectory's md_files set
                        if file in md_files or not is_wikidata_target(file_base):
                            # Append original match
                            new_line_parts.append(full_match)
                        else:
                            # Extract the base ID
                            base_id = file_base

                            # Determine Wikidata URL
                            if base_id.startswith("P"):
                                wikidata_url = (
                                    f"https://www.wikidata.org/wiki/Property:{base_id}"
                                )
                            else:
                                wikidata_url = (
                                    f"https://www.wikidata.org/wiki/{base_id}"
                                )

                            # Construct replacement: [link_text](wikidata_url)
                            replacement = f"[{link_text}]({wikidata_url})"

                            # Append the replacement
                            new_line_parts.append(replacement)
                            changes += 1

                    last_end = match.end()

                # Append the remaining part of the line after all matches
                new_line_parts.append(new_line[last_end:])

                # Join parts for the new line
                new_line = "".join(new_line_parts)

                # Write the (possibly changed) line to temp file
                f_out.write(new_line)

                # Progress indicator every 10000 lines
                if line_count % 10000 == 0:
                    print(f"Processed {line_count} lines in {filename}")

        # If changes were made, replace original with temp; else delete temp
        if changes > 0:
            os.replace(temp_file_path, file_path)
            files_changed += 1
            links_changed += changes
            print(f"Modified {changes} items in {filename}")
        else:
            os.remove(temp_file_path)

    print(f"\nSubdirectory Summary for {subdir_path}:")
    print(f"Total files processed: {files_processed}")
    print(f"Files changed: {files_changed}")
    print(f"Total items modified: {links_changed}")


def is_wikidata_target(file_base):
    """A Wikidata identifier is a bare Q- or P-number, never a path."""
    return re.fullmatch(r"[QP]\d+", file_base) is not None


def main():
    dirs = sys.argv[1:] or ["docs/missing-superclass"]

    for base_dir in dirs:
        if not os.path.isdir(base_dir):
            print(f"{base_dir} does not exist - skipped.")
            continue
        process_subdirectory(base_dir)

    print("\nReady.")


if __name__ == "__main__":
    main()
