#!/usr/bin/env raku
# Zelph Markdown Link Processor
# Purpose: Process markdown files to convert broken internal links (due to interrupted md generation) to text references
# Usage: ./replace_invalid_mkdocs_links.raku

use v6;

my $base-dir = 'docs/tree';
# Get all markdown files in $base-dir
my $md-files = dir($base-dir, :test(*.ends-with('.md'))).map(*.basename).Set;

say "Found {$md-files.elems} markdown files.";

my $files-processed = 0;
my $files-changed = 0;
my $links-changed = 0;

say "\nProcessing all files...";

for $md-files.keys.sort -> $filename {
    $files-processed++;
    
    # Progress indicator every 200 files
    if $files-processed %% 200 {
        say "... processed $files-processed files so far";
    }
    
    # Read the file content
    my $content = slurp($base-dir ~ "/" ~ $filename);
    my $original = $content;
    my $changes = 0;
    
    my regex md-link { \[ (.*?) \]\((.*?\.md)\) };
    
    my @matches = $content.match(/<md-link>/, :g);
    
    for @matches -> $match {
        my $link-text = ~$match<md-link>[0];
        my $file = ~$match<md-link>[1];
        
        if $file.contains("://") {
            next;
        }
        
        # Check if the referenced file exists
        if !($file ∈ $md-files) {
            # Extract file ID (P-number or Q-number) if present
            my $id-code = "";
            if $file ~~ /^ (P|Q) (\d+) \.md $/ {
                $id-code = " ({$0}{$1})";
            }
            
             my $full-match = "[$link-text]($file)";

            # Check if link text is surrounded by asterisks
            my $replacement;
            if $link-text ~~ /^\*(.*)\*$/ {
                # If surrounded by asterisks, move the trailing asterisk to the end
                my $inner-text = $0.Str;
                $replacement = "*$inner-text$id-code*";
            } else {
                # Otherwise, construct the replacement normally
                $replacement = "$link-text$id-code";
            }

            # Replace this specific occurrence only
            my $new-content = $content.subst($full-match, $replacement, :x(1));
            if $new-content ne $content {
                $content = $new-content;
                $changes++;
            } else {
                say "  WARNING: Failed to replace: $full-match"
            }
        }
    }
    
    # Save changes
    if $changes > 0 {
        $files-changed++;
        $links-changed += $changes;
        
        say "Modified $changes links in $filename";
        spurt($base-dir ~ "/" ~ $filename, $content);
    }
}

say "\nSummary:";
say "Total files processed: $files-processed";
say "Files changed: $files-changed";
say "Total links modified: $links-changed";
say "Ready.";
