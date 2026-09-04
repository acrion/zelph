#!/usr/bin/env bash
# dev_scripts/paper/mermaid_print_style.sh
# Rewrites the zelph's dark-theme mermaid export to a print palette
# and renders it. Usage: mermaid_print_style.sh mermaid-part-of-generated-html.mmd
# Requires mermaid-cli (mmdc) + system chromium.
set -euo pipefail

if ! command -v mmdc >/dev/null 2>&1; then
  echo "Error: mmdc not found in PATH" >&2
  exit 1
fi

if [[ ! -x /usr/bin/chromium ]]; then
  echo "Error: /usr/bin/chromium not found or not executable" >&2
  exit 1
fi

cat > /tmp/mermaid-puppeteer.json << 'EOF'
{
    "executablePath": "/usr/bin/chromium"
}
EOF


src="$1"
base="${src%.mmd}"
print_mmd="/tmp/$(basename "$base")_print.mmd"

sed -e 's/fill:#2d2d38/fill:#ffffff/g' \
    -e 's/fill:#1a1a2e/fill:#f7f7f7/g' \
    -e 's/fill:#8a5c00/fill:#ffd9a0/g' \
    -e 's/fill:#404040/fill:#afafaf/g' \
    -e 's/fill:#3b6327/fill:#bfff9f/g' \
    -e 's/fill:#3d3022/fill:#ffffff/g' \
    -e 's/color:#e0e0e0/color:#000000/g' \
    -e 's/stroke:#666666/stroke:#444444/g' \
    -e 's/stroke:#e6c300/stroke:#b8860b/g' \
    "${src}" > "${print_mmd}"

mmdc -i "${print_mmd}" -o "${base}.pdf" --backgroundColor white -p /tmp/mermaid-puppeteer.json --pdfFit

