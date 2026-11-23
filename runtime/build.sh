#!/bin/bash
# 从源码重新编译 libwxar_tip.dylib，并更新 patches.json 里登记的 SHA-256。
# 仓库里带了编译好的版本；不放心预编译文件、或改了 wxar_tip.cpp，就运行这个。
set -euo pipefail
cd "$(dirname "$0")/.."

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
echo "· 单元测试 …"
clang++ -std=c++17 -DWXAR_NO_INIT runtime/test_tip.cpp -o "$T/test"
"$T/test"

echo "· 编译 …"
clang++ -arch arm64 -std=c++17 -O2 -Wall -Wextra -dynamiclib -fvisibility=hidden \
  -mmacosx-version-min=13.0 -compatibility_version 1.0 -current_version 1.0 \
  -install_name @loader_path/libwxar_tip.dylib runtime/wxar_tip.cpp -o runtime/libwxar_tip.dylib

/usr/bin/python3 - <<'P'
import hashlib, json, pathlib
db = pathlib.Path("patches.json")
raw = db.read_text(encoding="utf-8")
d = json.loads(raw)
sha = hashlib.sha256(pathlib.Path("runtime/libwxar_tip.dylib").read_bytes()).hexdigest()
for b in d["builds"].values():
    for t in b["targets"].values():
        for f in t.get("files", []):
            if f["src"] == "runtime/libwxar_tip.dylib":
                f["sha256"] = sha
db.write_text(json.dumps(d, ensure_ascii=False, indent=2) + ("\n" if raw.endswith("\n") else ""), encoding="utf-8")
print("✓ 已编译，SHA-256", sha)
P
