#!/usr/bin/env bash
# esp-base 固件正式包的 log-sdk 源码准备（package Job 的 pre_build；本地开发同样可用）：
# 按本仓登记的 gitlink 取得 dependencies/log-sdk，构建时只用其中的 ESP-IDF 组件 log_esp（darren-space#114 T22）。
set -euo pipefail

fail() {
  printf '[ERROR] %s\n' "$*" >&2
  exit 1
}

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
product_root="$(cd -- "$script_dir/.." && pwd)"

for name in log-sdk; do
  relative="dependencies/$name"
  # 以索引里的 gitlink 为准：正式构建的检出与提交一致，本地开发 git add 后即可使用。
  read -r mode sdk_commit stage listed <<<"$(git -C "$product_root" ls-files --stage -- "$relative")"
  [[ "$mode" == 160000 && "$stage" == 0 && "$listed" == "$relative" && "$sdk_commit" =~ ^[0-9a-f]{40}$ ]] || \
    fail "本产品没有登记 $relative 的 gitlink。"
  url="$(git -C "$product_root" config -f .gitmodules --get "submodule.$name.url")"
  [[ "$url" == "git@github.com:darren-you/$name.git" ]] || fail ".gitmodules 的 $name 地址不符：$url"

  sdk_root="$product_root/$relative"
  if [[ -e "$sdk_root/.git" ]]; then
    [[ "$(git -C "$sdk_root" rev-parse HEAD)" == "$sdk_commit" ]] || fail "$relative 不在 gitlink 登记的提交上。"
  else
    [[ ! -d "$sdk_root" || -z "$(ls -A -- "$sdk_root")" ]] || fail "$relative 不为空，又不是 Git 检出。"
    mkdir -p -- "$sdk_root"
    git init --quiet --template= "$sdk_root"
    git -C "$sdk_root" remote add origin "$url"
    for attempt in 1 2 3; do
      git -C "$sdk_root" fetch --quiet --depth 1 origin "$sdk_commit" && break
      [[ "$attempt" -lt 3 ]] || fail "无法获取 $name 的登记提交。"
      sleep $((attempt * 10))
    done
    git -c advice.detachedHead=false -C "$sdk_root" checkout --quiet --detach "$sdk_commit"
  fi
  [[ -z "$(git -C "$sdk_root" status --porcelain=v1 --untracked-files=no)" ]] || fail "$relative 存在未保存改动。"
  printf '[OK] %s @ %s\n' "$name" "$sdk_commit"
done
