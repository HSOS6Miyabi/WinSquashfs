#!/usr/bin/env bash
set -euo pipefail

version="$(python3 -c 'import xml.etree.ElementTree as E; print(E.parse("Directory.Build.props").findtext("./PropertyGroup/Version"))')"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
tag="v$version"
commit="$(git rev-parse HEAD)"
if [[ "$GITHUB_REF" == refs/tags/* ]]; then
  [[ "$GITHUB_REF" == "refs/tags/$tag" ]] || { echo "Tag does not match project version"; exit 1; }
elif [[ "$GITHUB_REF" == refs/heads/main ]]; then
  latest="$(git ls-remote origin refs/heads/main | cut -f1)"
  if [[ "$latest" != "$commit" ]]; then
    echo "A newer main commit exists; its own CI run will determine publication."
    exit 0
  fi
else
  exit 1
fi

release_info="$(gh api --paginate "repos/$GITHUB_REPOSITORY/releases?per_page=100" --jq ".[] | select(.tag_name == \"$tag\") | [.id, .draft, .target_commitish] | @tsv")"
if [[ -n "$release_info" ]]; then
  IFS=$'\t' read -r release_id draft target <<< "$release_info"
  if [[ "$draft" == false ]]; then
    echo "$tag is already published."
    exit 0
  fi
  [[ "$target" == "$commit" ]] || { echo "Existing draft targets a different commit"; exit 1; }
fi
tag_refs="$(git ls-remote --tags origin "refs/tags/$tag" "refs/tags/$tag^{}")"
if [[ -n "$tag_refs" ]]; then
  tag_commit="$(printf '%s\n' "$tag_refs" | tail -1 | cut -f1)"
  [[ "$tag_commit" == "$commit" ]] || { echo "Existing tag targets a different commit"; exit 1; }
fi

bash scripts/package-release.sh output release "$version"
assets_to_publish=()
for arch in x64 x86 arm64; do
  assets_to_publish+=("WinArchiveFSPro-Setup-$arch.exe" "WinArchiveFSPro-portable-$arch.zip")
done
assets_to_publish+=("SHA256SUMS.txt")
upload_paths=()
for name in "${assets_to_publish[@]}"; do upload_paths+=("release/$name"); done

if [[ -z "$release_info" ]]; then
  gh release create "$tag" --repo "$GITHUB_REPOSITORY" --target "$commit" --draft \
    --title "WinArchiveFSPro $tag" --notes-file RELEASE_NOTES.md
fi
gh release upload "$tag" --repo "$GITHUB_REPOSITORY" --clobber \
  "${upload_paths[@]}"
release_id="$(gh release view "$tag" --repo "$GITHUB_REPOSITORY" --json databaseId --jq .databaseId)"
assets="$(gh api "repos/$GITHUB_REPOSITORY/releases/$release_id/assets")"
for name in "${assets_to_publish[@]}"; do
  size="$(stat -c %s "release/$name")"
  digest="sha256:$(sha256sum "release/$name" | cut -d' ' -f1)"
  jq -e --arg name "$name" --arg digest "$digest" --argjson size "$size" \
    'any(.[]; .name == $name and .state == "uploaded" and .size == $size and ((.digest // "") == "" or .digest == $digest))' \
    <<< "$assets" >/dev/null
done
gh release edit "$tag" --repo "$GITHUB_REPOSITORY" --draft=false --latest
gh release view "$tag" --repo "$GITHUB_REPOSITORY" --json isDraft,isPrerelease,tagName,url,assets --jq '{tagName,isDraft,isPrerelease,url,assets:[.assets[].name]}'
