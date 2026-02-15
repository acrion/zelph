#!/usr/bin/env bash
# see https://dumps.wikimedia.org/wikidatawiki/entities/
aria2c -x 8 -s 8 -c --retry-wait=10 --timeout=120 https://dumps.wikimedia.your.org/wikidatawiki/entities/20260209/wikidata-20260209-all.json.bz2
#aria2c -x 3 -s 3 -c --retry-wait=30 --max-tries=0 --timeout=120 --connect-timeout=60 https://dumps.wikimedia.org/wikidatawiki/entities/20260209/wikidata-20260209-all.json.bz2
