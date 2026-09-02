# Security Policy

## Reporting a vulnerability

Please do not report a security problem via a public issue, a pull request or a
discussion. Both routes below reach the maintainers privately.

The best option is GitHub’s private vulnerability reporting: open the
[Security tab](https://github.com/acrion/zelph/security) of this repository and
choose *Report a vulnerability*. The report stays private, you keep a link to the
conversation, and a fix can be prepared on a temporary private fork before
anything becomes visible.

If you would prefer not to use GitHub, write to info@acrion.ch. Please provide
sufficient detail to reproduce the issue: the zelph version or commit, the
platform, and the script or input that triggers it.

No timeline is promised here, because zelph is maintained by a small team and a
promise nobody can keep is worth less than none. Reports are read and answered.

## Supported versions

Only the latest release receives fixes. There are no maintained branches for
older versions.

## What the attack surface looks like

Worth knowing about the shape of the project: zelph is a local reasoning engine
with no network service of its own, so the interesting surface is what it parses.
Untrusted input reaches the engine through a Wikidata JSON dump, a binary network
file, a `.zph` script or a Janet script, and a Janet script is arbitrary code by
design and is not a vulnerability in itself. The WebAssembly playground runs
entirely in the visitor’s browser and sends nothing anywhere.
