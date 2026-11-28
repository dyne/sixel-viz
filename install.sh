#!/usr/bin/env bash
set -eu

main() {
  case "$(uname -s):$(uname -m)" in
    Linux:x86_64) platform=linux-x86_64 ;;
    Linux:aarch64|Linux:arm64) platform=linux-aarch64 ;;
    Linux:armv7l|Linux:armv8l) platform=linux-armhf ;;
    Linux:riscv64) platform=linux-riscv64 ;;
    Darwin:arm64) platform=macos-arm64 ;;
    *) echo "No release for $(uname -s) $(uname -m)." >&2; exit 1 ;;
  esac

  asset=si-$platform.tar.gz
  url=https://github.com/dyne/sixel-viz/releases/latest/download
  prefix=${SI_PREFIX:-"$HOME/.local"}
  [[ $prefix = /* ]] || { echo 'SI_PREFIX must be an absolute path.' >&2; exit 1; }
  mkdir -p -- "$prefix/bin" "$prefix/lib/sixel-viz"
  tmp=$(mktemp -d "$prefix/.si.XXXXXXXX")
  trap 'rm -rf -- "$tmp"' EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  cd -- "$tmp"

  curl -fsSL --fail-early --retry 2 --connect-timeout 10 --max-time 180 -O "$url/SHA256SUMS.txt" -O "$url/$asset"
  expected=$(awk -v asset="$asset" '$2 == asset { print $1; exit }' SHA256SUMS.txt)
  if command -v sha256sum >/dev/null; then
    actual=$(sha256sum "$asset")
  else
    actual=$(shasum -a 256 "$asset")
  fi
  [[ $expected =~ ^[0-9a-f]{64}$ && ${actual%% *} = "$expected" ]] ||
    { echo "SHA-256 verification failed for $asset." >&2; exit 1; }

  bundle="$prefix/lib/sixel-viz/$expected"
  if [[ ! -x $bundle/si ]]; then
    mkdir package
    tar -xzf "$asset" -C package
    test -x package/si
    if [[ ! -x $bundle/si ]]; then
      [[ ! -e $bundle ]] || { echo "Incomplete installation at $bundle." >&2; exit 1; }
      mv -- package "$bundle"
    fi
  fi
  printf '#!/usr/bin/env bash\nexec %q "$@"\n' "$bundle/si" > si
  chmod +x si
  [[ ! -d $prefix/bin/si ]] || { echo "$prefix/bin/si is a directory." >&2; exit 1; }
  mv -f -- si "$prefix/bin/si"
  printf 'Installed %s\n' "$prefix/bin/si"
}

main
