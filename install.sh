#!/usr/bin/env bash
# Installs Thistle: clones the engine (there's no prebuilt package — see
# docs/building.md, you vendor the source and build it every time, on
# purpose) to a stable location, then puts the `thistle` CLI on your PATH.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/StacikM/thistle/main/install.sh | bash
#   ./install.sh                    # or run it locally from a clone
#
# Env overrides:
#   THISTLE_HOME     where the engine lives (default: ~/.thistle)
#   THISTLE_BIN_DIR  where the `thistle` command gets symlinked (default: ~/.local/bin)
set -euo pipefail

REPO_URL="https://github.com/StacikM/thistle.git"
INSTALL_DIR="${THISTLE_HOME:-$HOME/.thistle}"
BIN_DIR="${THISTLE_BIN_DIR:-$HOME/.local/bin}"

if ! command -v git >/dev/null 2>&1; then
    echo "thistle: git is required (used to fetch the engine source)" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "thistle: python3 is required (the CLI is stdlib-only, but it does need an interpreter)" >&2
    exit 1
fi

if [ -d "$INSTALL_DIR/.git" ]; then
    echo "==> Updating existing install at $INSTALL_DIR"
    git -C "$INSTALL_DIR" pull --ff-only
else
    echo "==> Cloning Thistle to $INSTALL_DIR"
    git clone --depth 1 "$REPO_URL" "$INSTALL_DIR"
fi

mkdir -p "$BIN_DIR"
chmod +x "$INSTALL_DIR/tools/thistle-cli/thistle.py"
ln -sf "$INSTALL_DIR/tools/thistle-cli/thistle.py" "$BIN_DIR/thistle"

echo "==> Engine: $INSTALL_DIR"
echo "==> CLI:    $BIN_DIR/thistle"

case ":$PATH:" in
    *":$BIN_DIR:"*)
        echo ""
        echo "Try: thistle new mygame && cd mygame && thistle run"
        ;;
    *)
        echo ""
        echo "$BIN_DIR isn't on your PATH yet. Add this to your shell profile (~/.zshrc, ~/.bashrc, ...):"
        echo ""
        echo "    export PATH=\"$BIN_DIR:\$PATH\""
        echo ""
        echo "Then open a new shell and try: thistle new mygame && cd mygame && thistle run"
        ;;
esac
