#!/usr/bin/env python3
"""A/B: does collapsing the 51-ray depth fan to 7 sectors change Jev's decisions?

Replays the observations already logged in a run.jsonl and, for each, asks Jev TWICE
with two states that differ ONLY in `view`:
  A = the current 7-sector summary (min/max per sector) + openings + ahead_min_dist
  B = the full 51-ray fan (every ~1.8 deg) + the same openings + ahead_min_dist
Everything else (player, aim, way_out, door, progress, commander...) is identical, so
any difference in the chosen macro is due to the view resolution alone.

    python compare_view.py run.jsonl --limit 30            # key from TYPESAFE_API_KEY
    python compare_view.py run.jsonl --limit 30 --jev-key ...

Prints a per-step table, an agreement rate, the divergences with context, and the
average input tokens per call for A vs B (the token cost of sending the full fan).
Needs the Jev key (same Cloudflare-safe client as the agent). Read-only; makes 2
Jev calls per sampled step (~$0.00003 each).
"""
import argparse
import copy
import json
import os
import sys

import doom_jev_agent as agent

RAYS_NOTE = (
    " OVERRIDE FOR THIS REQUEST: `view` has NO `sectors`. Instead `view.rays` is the "
    "full depth fan: 51 rays across -45..+45 degrees (about every 1.8 deg), each "
    "{b: bearing, d: distance to the nearest wall along that ray, 2048 = nothing}. "
    "Read the fan directly to judge where walls and gaps are; a run of long rays is a "
    "passage, a short run is a wall or pillar."
)


def state_rays(state_a, walls):
    """State B: same as A but the 7-sector `view.sectors` replaced by the raw fan."""
    b = copy.deepcopy(state_a)
    v = b["view"]
    v.pop("sectors", None)
    v["rays"] = [{"b": w["bearing"], "d": w["dist"]} for w in walls]
    b["legend"] = state_a["legend"] + RAYS_NOTE
    return b


def interesting(rec):
    s = rec.get("state") or {}
    return bool(s.get("door_ahead") or s.get("enemies_in_view") or s.get("hazards_in_view")
               or rec["obs"].get("blocked") or (s.get("view", {}).get("ahead_min_dist", 9999) < 150))


def choice_of(answers):
    a = answers["action"]
    c = a.get("choice")
    return c, round(float(a.get("confidence", 0) or 0), 2)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="a run.jsonl produced by doom_jev_agent.py --log")
    ap.add_argument("--limit", type=int, default=30, help="max situations to compare (2 Jev calls each)")
    ap.add_argument("--jev-url", default=os.environ.get("TYPESAFE_API_URL", agent.JEV_DEFAULT_URL))
    ap.add_argument("--jev-model", default=os.environ.get("TYPESAFE_MODEL", agent.JEV_DEFAULT_MODEL))
    ap.add_argument("--jev-key", default=os.environ.get("TYPESAFE_API_KEY"))
    ap.add_argument("--jev-timeout", type=float, default=8.0)
    ap.add_argument("--out", help="write the per-step results as JSONL here")
    args = ap.parse_args()
    if not args.jev_key:
        sys.exit("no key: set TYPESAFE_API_KEY or pass --jev-key")

    recs = [json.loads(l) for l in open(args.log, encoding="utf-8")]
    recs = [x for x in recs if x.get("kind") == "step" and x.get("obs", {}).get("valid")
            and isinstance(x.get("state"), dict) and x["obs"].get("walls")]
    # prefer varied situations, then fill with the rest, keeping run order
    picked = [x for x in recs if interesting(x)][:args.limit]
    if len(picked) < args.limit:
        seen = {id(x) for x in picked}
        for x in recs:
            if id(x) not in seen:
                picked.append(x)
            if len(picked) >= args.limit:
                break
    picked.sort(key=lambda x: x["step"])

    jev = agent.JevClient(args.jev_url, args.jev_key, args.jev_model, timeout=args.jev_timeout)
    questions = agent.build_questions(agent.DEFAULT_INTENT)

    same = 0
    rows = []
    toks_a = toks_b = 0
    print(f"{'step':>4} {'situation':<22} {'7-sectors':<16} {'51-rays':<16} {'same?'}")
    for x in picked:
        sa = x["state"]
        sb = state_rays(sa, x["obs"]["walls"])
        try:
            ans_a, ua = jev.decide(sa, questions)
            ans_b, ub = jev.decide(sb, questions)
        except agent.JevError as e:
            sys.exit(f"Jev error: {e}")
        ca, cfa = choice_of(ans_a)
        cb, cfb = choice_of(ans_b)
        toks_a += int((ua or {}).get("input_tokens", 0) or 0)
        toks_b += int((ub or {}).get("input_tokens", 0) or 0)
        s = x["state"]
        sit = []
        if s.get("door_ahead"):
            d = s["door_ahead"]
            sit.append(f"door {d['dist']}@{d['bearing']:+d}" + ("!" if d.get("wont_open") else ""))
        if s.get("enemies_in_view"):
            e = min(s["enemies_in_view"], key=lambda e: abs(e["bearing"]))
            sit.append(f"{e['name']} {e['dist']}@{e['bearing']:+d}")
        if s.get("hazards_in_view"):
            sit.append("barrel")
        if x["obs"].get("blocked"):
            sit.append("blocked")
        sit.append(f"ahead={s['view']['ahead_min_dist']}")
        agree = ca == cb
        same += agree
        rows.append({"step": x["step"], "sit": " ".join(sit), "sectors": ca, "conf_s": cfa,
                     "rays": cb, "conf_r": cfb, "agree": agree})
        print(f"{x['step']:>4} {' '.join(sit)[:22]:<22} {f'{ca}({cfa})':<16} {f'{cb}({cfb})':<16} {'=' if agree else 'DIFF'}")

    n = len(rows)
    print(f"\nagreement: {same}/{n} = {100*same/max(1,n):.0f}%   (macro identical with both views)")
    print(f"avg input tokens/call:  7-sectors {toks_a//max(1,n)}   51-rays {toks_b//max(1,n)}   "
          f"(+{(toks_b-toks_a)//max(1,n)} per call with the full fan)")
    diffs = [r for r in rows if not r["agree"]]
    if diffs:
        print(f"\ndivergences ({len(diffs)}):")
        for r in diffs:
            print(f"  s{r['step']:>4}  [{r['sit']}]  7-sectors -> {r['sectors']}({r['conf_s']})   "
                  f"51-rays -> {r['rays']}({r['conf_r']})")
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")
        print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
