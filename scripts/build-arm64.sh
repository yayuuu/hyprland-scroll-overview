#!/usr/bin/env bash
# Reproducible plugin-only ARM build; does not replace or load Hyprland.
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dobby_revision=2f49a2ab60ca65cb8add43f60bda20e78bd6ab86
dobby_source=${DOBBY_SOURCE:-$repo_root/.cache/dobby}
if ! git -C "$dobby_source" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git clone --no-checkout "${DOBBY_REPOSITORY:-https://github.com/jmpews/Dobby.git}" "$dobby_source"
  git -C "$dobby_source" checkout --detach "$dobby_revision"
fi
[[ $(git -C "$dobby_source" rev-parse HEAD) == "$dobby_revision" ]] || {
  echo 'Dobby checkout does not match the tested ARM revision.' >&2; exit 1;
}
[[ -z $(git -C "$dobby_source" status --porcelain) ]] || {
  echo 'Dobby checkout has local changes; refusing an unverified dependency.' >&2; exit 1;
}
build_root=$repo_root/build/arm64
cmake -S "$dobby_source" -B "$build_root/dobby" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DDOBBY_DEBUG=OFF \
  -DPlugin.SymbolResolver=OFF -DDOBBY_BUILD_TEST=OFF -DDOBBY_BUILD_EXAMPLE=OFF
cmake --build "$build_root/dobby" --target dobby_static -j "${BUILD_JOBS:-4}"
for smoke in hook-smoke gesture-smoke; do
  libraries=()
  [[ $smoke != hook-smoke ]] || libraries=("$build_root/dobby/libdobby.a" -ldl -lpthread)
  g++ -O2 -std=c++23 -fno-gnu-unique -I "$repo_root" -I "$dobby_source/include" \
    "$repo_root/tests/arm64/$smoke.cpp" "${libraries[@]}" -o "$build_root/$smoke"
  "$build_root/$smoke"
done
cmake -S "$repo_root" -B "$build_root/plugin" -DCMAKE_BUILD_TYPE=Release \
  -DDOBBY_SOURCE="$dobby_source" -DDOBBY_LIBRARY="$build_root/dobby/libdobby.a"
cmake --build "$build_root/plugin" -j "${BUILD_JOBS:-4}"
cp "$build_root/plugin/libscrolloverview.so" "$repo_root/scrolloverview.so"
echo 'Built ARM ScrollOverview; no compositor or configuration changes made.'
