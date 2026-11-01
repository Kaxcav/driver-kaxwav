"""Mini cliente do painel. Uso: python api.py get /api/state [campos...] | python api.py post /api/dsp/ch1 '{"gate":false}'"""
import sys, json, urllib.request
U = "http://127.0.0.1:8930"
if sys.argv[1] == "get":
    d = json.load(urllib.request.urlopen(U + sys.argv[2]))
    keys = sys.argv[3:]
    print(json.dumps({k: d.get(k) for k in keys} if keys else d, ensure_ascii=False))
else:
    req = urllib.request.Request(U + sys.argv[2], data=sys.argv[3].encode(), headers={"Content-Type": "application/json"}, method="POST")
    print(urllib.request.urlopen(req).read().decode())
