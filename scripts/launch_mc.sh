#!/usr/bin/env bash
# Launch Minecraft 1.21.4 (vanilla files from the flatpak launcher dir)
# with HOST java + LD_PRELOAD=voltage.so, bypassing the flatpak sandbox.
#
# ---- HOW TO RUN (понятная инструкция) ----
# 1. Собери библиотеку:   ./scripts/build.sh
# 2. Быстрая проверка:     ./scripts/run_test.sh        (должен увидеть "loaded into /bin/sleep")
# 3. Запуск майнкрафта:    ./scripts/launch_mc.sh <nickname>
#    Пример:               ./scripts/launch_mc.sh Steve
#
# Что происходит:
# - скрипт берет gameDir /home/kotoed/.var/app/ch.tlaun.TL/data/game (твои миры, сервера, скины — всё то же)
# - берет versions/1.21.4/1.21.4.jar + все libraries из json (LWJGL 3.3.3 и др.)
# - запускает ХОСТОВУЮ /usr/bin/java (21), а не flatpak-jre
# - подсовывает LD_PRELOAD=build/libvoltage.so  -> наш код стартует раньше игры
# - лог инжекта: tail -f /tmp/voltage.log  (вторым терминалом)
# - игра запускается в OFFLINE/demo без Microsoft-авторизации (токен-заглушка);
#   для лицензионных серверов заходи через свой обычный лаунчер + gdb-инжект (этап 2).
#
# Тестируй СНАЧАЛА в одиночном мире. Паблики с читом = бан, это на твоей совести.
set -euo pipefail

NICK="${1:-VoltageTest}"
GAME_DIR="/home/kotoed/.var/app/ch.tlaun.TL/data/game"
VER="1.21.4"
VER_DIR="$GAME_DIR/versions/$VER"
JSON="$VER_DIR/$VER.json"
JAR="$VER_DIR/$VER.jar"
SO="$(pwd)/build/libvoltage.so"

[[ -f "$JAR" ]] || { echo "no jar: $JAR" >&2; exit 1; }
[[ -f "$SO" ]] || { echo "no $SO, run ./scripts/build.sh first" >&2; exit 1; }

JAVA="$(command -v java)"
echo "[*] java: $JAVA ($("$JAVA" -version 2>&1 | head -n1))"

# Classpath: client jar + every library jar (non-native) from the version json.
CP="$JAR"
while read -r lib; do CP="$CP:$lib"; done < <(python3 - "$JSON" "$GAME_DIR/libraries" <<'EOF'
import json, sys, os
js = json.load(open(sys.argv[1]))
base = sys.argv[2]
def maven_path(name):
    # group:artifact:version[:classifier] -> group/group.../artifact/version/artifact-version[-classifier].jar
    p = name.split(":")
    g, a, v = p[0].replace(".", "/"), p[1], p[2]
    clf = "-" + p[3] if len(p) > 3 else ""
    return f"{g}/{a}/{v}/{a}-{v}{clf}.jar"
for l in js.get("libraries", []):
    nm = l.get("name", "")
    if ":natives" in nm or "natives" in nm: continue  # natives are -Djava.library.path, not classpath
    a = l.get("artifact") or (l.get("downloads") or {}).get("artifact") or {}
    p = a.get("path") or maven_path(nm)  # legacy entries (e.g. jopt-simple) have no path
    if not p.endswith(".jar"): continue
    full = os.path.join(base, p)
    if os.path.isfile(full): print(full)
    else: print(f"missing: {full}", file=sys.stderr)
EOF
)
NATIVES="$VER_DIR/natives"
ASSETS="$GAME_DIR/assets"
ASSET_INDEX="$(python3 -c "import json;print(json.load(open('$JSON'))['assetIndex']['id'])")"

echo "[*] gameDir : $GAME_DIR"
echo "[*] natives : $NATIVES"
echo "[*] assets  : $ASSETS ($ASSET_INDEX)"
echo "[*] nick    : $NICK"
echo "[*] log     : tail -f /tmp/voltage.log"
echo "[*] hint    : открой ВТОРОЙ терминал и выполни: tail -f /tmp/voltage.log"

export VOLTAGE_LOG=/tmp/voltage.log
if [[ "${VOLTAGE_NOINJECT:-0}" == "1" ]]; then
  echo "[*] VOLTAGE_NOINJECT=1 -> запуск БЕЗ инжекта (базовая проверка окружения)"
  unset LD_PRELOAD
else
  export LD_PRELOAD="$SO"
fi

exec "$JAVA" \
  -Xmx2G \
  -Djava.library.path="$NATIVES" \
  -cp "$CP" \
  net.minecraft.client.main.Main \
  --username "$NICK" \
  --version "$VER" \
  --gameDir "$GAME_DIR" \
  --assetsDir "$ASSETS" \
  --assetIndex "$ASSET_INDEX" \
  --accessToken "0" \
  --userType "legacy"
