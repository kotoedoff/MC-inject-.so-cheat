#!/usr/bin/env bash
# Launch Minecraft 1.21.4 FABRIC (KnotClient) with HOST java + LD_PRELOAD.
#
# voltage auto-detects Fabric at runtime: JVMTI sees Lnet/minecraft/class_310;
# instead of Lflk; and the ESP/TriggerBot/menu switch to intermediary mappings.
#
# ---- HOW TO RUN ----
# 1. ./scripts/build.sh
# 2. ./scripts/launch_mc_fabric.sh Steve
# 3. tail -f /tmp/voltage.log  ->  "detected profile: FABRIC (intermediary)"
#
# Mods (sodium, fabric-api) from gameDir/mods are loaded automatically by Knot.
# Same offline-token stub as launch_mc.sh: singleplayer / offline servers only.
set -euo pipefail

NICK="${1:-VoltageTest}"
GAME_DIR="/home/kotoed/.var/app/ch.tlaun.TL/data/game"
VER_JSON="$GAME_DIR/versions/Fabric 1.21.4/Fabric 1.21.4.json"
VER_JAR="$GAME_DIR/versions/Fabric 1.21.4/Fabric 1.21.4.jar"
VANILLA_JSON="$GAME_DIR/versions/1.21.4/1.21.4.json"
SO="$(pwd)/build/libvoltage.so"

[[ -f "$VER_JSON" ]] || { echo "no fabric json: $VER_JSON" >&2; exit 1; }
[[ -f "$VER_JAR" ]] || { echo "no fabric jar: $VER_JAR" >&2; exit 1; }
[[ -f "$SO" ]] || { echo "no $SO, run ./scripts/build.sh first" >&2; exit 1; }

JAVA="$(command -v java)"
echo "[*] java: $JAVA ($("$JAVA" -version 2>&1 | head -n1))"

# Classpath: fabric jar + every library jar from BOTH jsons (fabric json has
# no inheritsFrom, vanilla libraries like jopt-simple must come from there).
# Dedupe by artifact (group:name): Knot dies on duplicate classes
# (e.g. asm 9.9 from Fabric vs asm 9.6 from vanilla). Fabric json wins.
CP="$VER_JAR"
while read -r lib; do CP="$CP:$lib"; done < <(
python3 - "$VER_JSON" "$VANILLA_JSON" "$GAME_DIR/libraries" <<'EOF'
import json, sys, os
base = sys.argv[3]
seen_path = set()
seen_artifact = set()

def maven_path(name):
    p = name.split(":")
    g, a, v = p[0].replace(".", "/"), p[1], p[2]
    clf = "-" + p[3] if len(p) > 3 else ""
    return f"{g}/{a}/{v}/{a}-{v}{clf}.jar"

def collect(json_path):
    if not os.path.isfile(json_path): return
    js = json.load(open(json_path))
    for l in js.get("libraries", []):
        nm = l.get("name", "")
        if ":natives" in nm or "natives" in nm: continue
        parts = nm.split(":")
        art = (parts[0], parts[1]) if len(parts) >= 2 else (nm, "")
        if art in seen_artifact: continue  # fabric json first, vanilla dup dropped
        a = l.get("artifact") or (l.get("downloads") or {}).get("artifact") or {}
        p = a.get("path") or maven_path(nm)
        if not p.endswith(".jar"): continue
        full = os.path.join(base, p)
        if os.path.isfile(full) and full not in seen_path:
            seen_path.add(full); seen_artifact.add(art); print(full)
        elif not os.path.isfile(full):
            print(f"missing: {full}", file=sys.stderr)

collect(sys.argv[1])  # fabric json first (its intermediary/loader win)
collect(sys.argv[2])  # vanilla fallback
EOF
)
NATIVES="$GAME_DIR/versions/Fabric 1.21.4/natives"
# Vanilla natives as fallback: the launcher sometimes skips extracting some
# LWJGL natives (e.g. liblwjgl_tinyfd.so) into the Fabric natives dir.
# Same LWJGL 3.3.3 on both sides, so the files are interchangeable.
VANILLA_NATIVES="$GAME_DIR/versions/1.21.4/natives"
ASSETS="$GAME_DIR/assets"
ASSET_INDEX="$(python3 -c "import json;print(json.load(open('$VANILLA_JSON'))['assetIndex']['id'])")"

echo "[*] gameDir : $GAME_DIR"
echo "[*] main    : net.fabricmc.loader.impl.launch.knot.KnotClient"
echo "[*] natives : $NATIVES"
echo "[*] assets  : $ASSETS ($ASSET_INDEX)"
echo "[*] nick    : $NICK"
echo "[*] log     : tail -f /tmp/voltage.log"

export VOLTAGE_LOG=/tmp/voltage.log
if [[ "${VOLTAGE_NOINJECT:-0}" == "1" ]]; then
  echo "[*] VOLTAGE_NOINJECT=1 -> running WITHOUT injection"
  unset LD_PRELOAD
else
  export LD_PRELOAD="$SO"
fi

exec "$JAVA" \
  -Xmx2G \
  -Djava.library.path="$NATIVES:$VANILLA_NATIVES" \
  -cp "$CP" \
  net.fabricmc.loader.impl.launch.knot.KnotClient \
  --username "$NICK" \
  --version "Fabric 1.21.4" \
  --gameDir "$GAME_DIR" \
  --assetsDir "$ASSETS" \
  --assetIndex "$ASSET_INDEX" \
  --accessToken "0" \
  --userType "legacy"
