#!/usr/bin/env bash
# Record real AMap (高德) responses as host-test fixtures for the place package
# (tests/host/fixtures/amap/).  Uses public landmarks only (天安门 -> 故宫),
# never the device's own position.  The key is read from ../.amap_key, sent
# only to AMap, and every saved file is checked not to contain it.
#
#     tools/record_amap_fixtures.sh
set -euo pipefail
cd "$(dirname "$0")/.."
KEY_FILE=../.amap_key
OUT=tests/host/fixtures/amap
[ -s "$KEY_FILE" ] || { echo "missing $KEY_FILE" >&2; exit 1; }
KEY=$(tr -d '[:space:]' < "$KEY_FILE")
BASE=https://restapi.amap.com
ORIGIN=116.397455,39.909187   # 天安门 (GCJ-02)
FAR=116.481028,39.989643      # 望京, AMap's own documentation example point
mkdir -p "$OUT"

# get <file> <path-with-query>: one call, pretty-printed, status must be "1".
get() {
    local body
    body=$(curl -sS -m 15 --retry 3 --retry-all-errors --retry-delay 1 "$BASE/$2&key=$KEY")
    python3 -c 'import json,sys
d=json.loads(sys.argv[1])
assert d.get("status")=="1", d.get("info")
json.dump(d,open(sys.argv[2],"w"),ensure_ascii=False,indent=1)' "$body" "$OUT/$1"
    echo "recorded $1"
}

# v3: search, geocoding, weather, IP location (what ops_place.cpp calls)
get around.json "v3/place/around?radius=1000&offset=6&location=$ORIGIN&keywords=%E9%A4%90%E5%8E%85"  # 餐厅
get text.json "v3/place/text?offset=1&city=110000&citylimit=true&keywords=%E6%95%85%E5%AE%AB%E5%8D%9A%E7%89%A9%E9%99%A2"  # 故宫博物院
DEST=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["pois"][0]["location"])' "$OUT/text.json")
get regeo.json "v3/geocode/regeo?location=$ORIGIN"
get weather.json "v3/weather/weatherInfo?city=110101"
get ip.json "v3/ip?ip=202.106.0.20"   # a public Beijing resolver address
# v5 route planning, same parameters as ops_place.cpp
get walking.json "v5/direction/walking?origin=$ORIGIN&destination=$DEST&show_fields=cost"
get driving.json "v5/direction/driving?origin=$ORIGIN&destination=$DEST&strategy=0&show_fields=cost"
get bicycling.json "v5/direction/bicycling?origin=$ORIGIN&destination=$DEST"
get transit.json "v5/direction/transit/integrated?origin=$ORIGIN&destination=$FAR&city1=010&city2=010&AlternativeRoute=1&show_fields=cost"

python3 "$(dirname "$0")/prune_amap_fixture.py" "$OUT"/*.json >/dev/null  # keep only what the parser reads
if grep -rqF "$KEY" "$OUT"; then
    echo "key found in fixtures, removing them" >&2
    rm -f "$OUT"/*.json
    exit 1
fi
echo "ok: fixtures in $OUT (key not present)"
