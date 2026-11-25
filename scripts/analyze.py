#!/usr/bin/env python3
"""
analyze.py - compute the evaluation metrics of the leader-election project
from serial logs.

Input: any text logs that contain the firmware's "LE <idx> <ms> <EVENT> ..."
lines: a Cooja COOJA.testlog, or one file per mote captured on real TelosB
motes (e.g. `make TARGET=sky login MOTE=1 | tee results/mote1.log`). Prefixes
before "LE" (timestamps, "ID:3", ...) are ignored. All durations are measured
with the clock of a single mote, so the motes' clocks need not agree; events
of different motes are matched by round number and term, which are shared
through the beacons.

Usage:
  python3 scripts/analyze.py results/run1/*.log
  python3 scripts/analyze.py COOJA.testlog --firmware leader-election.sky
  python3 scripts/analyze.py COOJA.testlog --json out.json --csv-row study.csv --label k=2
"""
import argparse
import collections
import json
import math
import os
import re
import statistics
import subprocess
import sys

LINE = re.compile(r'LE (\d+) (\d+) ([A-Z_]+)\b ?(.*)$')
KV = re.compile(r'(\w+)=(\S+)')
MSG_TYPES = ['BEACON', 'DATA', 'REPORT', 'ACKMAP', 'RESULT', 'JOIN']
CTRL_TYPES = ['BEACON', 'REPORT', 'ACKMAP', 'RESULT', 'JOIN']
# IEEE 802.15.4 framing added to every application payload (bytes):
# PHY 6 (preamble 4, SFD 1, length 1) + MAC header (FCF 2, seq 1, PAN 2,
# dst 2 for broadcast / 8 for unicast, src 8) + FCS 2.
FRAME_OVERHEAD_BCAST = 6 + 2 + 1 + 2 + 2 + 8 + 2
FRAME_OVERHEAD_UCAST = 6 + 2 + 1 + 2 + 8 + 8 + 2
ACK_FRAME = 6 + 5
UNICAST_TYPES = {'DATA', 'JOIN'}


def num(v):
    try:
        return int(v, 16) if v.startswith('0x') else int(v)
    except ValueError:
        return v


def parse(paths):
    events = []
    for p in paths:
        with open(p, errors='replace') as f:
            lines = f.readlines()
        # lines before the first BOOT were printed before the reset: skip them
        start = 0
        for i, line in enumerate(lines):
            m = LINE.search(line)
            if m and m.group(3) == 'BOOT':
                start = i
                break
        for n, line in enumerate(lines[start:], start):
            m = LINE.search(line)
            if not m:
                continue
            kv = {k: num(v) for k, v in KV.findall(m.group(4))}
            kv['_node'] = int(m.group(1))
            kv['_ms'] = int(m.group(2))
            kv['_ev'] = m.group(3)
            kv['_raw'] = m.group(4)
            kv['_order'] = (p, n)
            events.append(kv)
    return events


def mean(xs):
    return statistics.mean(xs) if xs else float('nan')


def fmt(x, unit=''):
    if isinstance(x, float) and math.isnan(x):
        return 'n/a'
    if isinstance(x, float):
        return f'{x:.1f}{unit}'
    return f'{x}{unit}'


def split_vec(v):
    return [int(x) for x in str(v).split(',')]


def analyze(events, firmware=None):
    by = collections.defaultdict(list)
    for e in events:
        by[e['_ev']].append(e)
    res = {}

    boot = {e['_node']: e for e in by['BOOT']}
    nodes = sorted(set(e['_node'] for e in events))
    any_boot = next(iter(boot.values()), {})
    period = any_boot.get('period_ms', 5000)
    res['config'] = {'nodes': nodes, 'K': any_boot.get('k'), 'period_ms': period,
                     'rotation_rounds': any_boot.get('rot')}

    # ------------------------------------------------------------------ rounds
    rnd = {}
    leaders_in_round = collections.defaultdict(set)
    for e in by['RND']:
        rnd.setdefault(e['r'], e)
        leaders_in_round[e['r']].add(e['_node'])
    rounds = sorted(rnd)
    res['rounds'] = {'first': rounds[0] if rounds else None,
                     'last': rounds[-1] if rounds else None,
                     'with_leader': len(rounds)}
    leader_rounds = collections.Counter(e['_node'] for e in rnd.values())
    res['leader_rounds_per_node'] = dict(sorted(leader_rounds.items()))

    # --------------------------------------------------------------- elections
    elections = []
    for e in by['ELECT']:
        elections.append({'round': e['r'], 'reason': e['reason'], 'leader': e['_node'],
                          'winner': e['winner'], 'term': e['term'],
                          'decision_ms': e['dur_ms'], 'relaxed': e['relaxed'],
                          'cand': e.get('cand', '')})
    # how members experienced each change of leader (by term)
    newldr = collections.defaultdict(list)
    for e in by['NEWLDR']:
        if e.get('gap_ms', 0) > 0:
            newldr[e['term']].append(e)
    takeovers = {e['term']: e for e in by['TAKEOVER']}
    faildet = by['FAILDET']
    failsims = by['FAILSIM']

    planned = []
    for el in elections:
        if el['winner'] == el['leader']:
            continue
        gaps = [x['gap_ms'] for x in newldr.get(el['term'], [])]
        planned.append({**el, 'member_gap_ms': gaps,
                        'handover_extra_ms': [g - period for g in gaps]})
    failovers = []
    for term, t in sorted(takeovers.items()):
        gaps = [x['gap_ms'] for x in newldr.get(term, [])]
        det = [f['since_ms'] for f in faildet if f['_node'] == t['_node'] and
               abs(f['_ms'] - t['_ms']) < period]
        # was the old leader really failed? (FAILSIM shortly before)
        # was the old leader really gone? (crash or death within the last
        # K + backups + 2 rounds; older logs without round numbers: any time)
        k = res['config']['K'] or 2
        def recent(x):
            if 'r' not in t or 'r' not in x:
                return True
            return t['r'] - (k + 5) <= x['r'] <= t['r']
        real = any(f['_node'] == t['prev'] and f['role'] == 'LEADER' and recent(f)
                   for f in failsims) or \
            any(x['_node'] == t['prev'] and x.get('role') == 'DEAD' and recent({'r': x.get('round', 0)})
                for x in by['ROLE'])
        el = next((x for x in elections if x['term'] == term and x['leader'] == t['_node']), None)
        failovers.append({'term': term, 'new_leader': t['_node'], 'prev': t['prev'],
                          'backup_rank': t['rank'], 'missed': t['missed'],
                          'detection_ms': det[0] if det else None,
                          'member_gap_ms': gaps, 'real_failure': real,
                          'decision_ms': el['decision_ms'] if el else None,
                          'round': el['round'] if el else None})
    res['elections'] = elections
    res['planned_handovers'] = planned
    res['failovers'] = failovers
    res['stepdowns'] = len(by['STEPDOWN'])
    res['orphans'] = len(by['ORPHAN'])
    res['rounds_with_two_leaders'] = sorted(r for r, l in leaders_in_round.items() if len(l) > 1)
    res['handover_fallbacks'] = sum(1 for e in by['HANDOVER'] if 'fail' in e['_raw'])

    # ---------------------------------------------------- control messages
    stats = collections.defaultdict(dict)       # node -> round -> STAT
    for e in by['STAT']:
        stats[e['_node']][e['r']] = e
    per_round = collections.defaultdict(lambda: collections.Counter())
    per_round_bytes = collections.defaultdict(lambda: collections.Counter())
    last_stat = {}
    radio_by_role = collections.defaultdict(list)   # role -> radio ms per round
    for node, d in stats.items():
        prev = None
        for r in sorted(d):
            cur = d[r]
            tx = split_vec(cur['tx'])
            txb = split_vec(cur['txB'])
            if prev is not None and r - prev['r'] == 1 and cur['role'] == prev['role']:
                radio_by_role[cur['role']].append(cur['radio_ms'] - prev['radio_ms'])
            if prev is not None:
                ptx, ptxb = split_vec(prev['tx']), split_vec(prev['txB'])
                for i, t in enumerate(MSG_TYPES):
                    per_round[r][t] += tx[i] - ptx[i]
                    per_round_bytes[r][t] += txb[i] - ptxb[i]
            prev = cur
        last_stat[node] = d[max(d)]

    def ctrl(r):
        return sum(per_round[r][t] for t in CTRL_TYPES)

    elect_rounds = [r for r in rounds if rnd[r]['type'] == 'ELECT' and r in per_round]
    data_rounds = [r for r in rounds if rnd[r]['type'] == 'DATA' and r in per_round
                   and r - 1 in rnd and rnd[r - 1]['type'] == 'DATA']
    base = mean([ctrl(r) for r in data_rounds])
    res['control_messages'] = {
        'per_data_round': base,
        'per_election_round': mean([ctrl(r) for r in elect_rounds]),
        'election_round_by_type': {t: mean([per_round[r][t] for r in elect_rounds])
                                   for t in MSG_TYPES},
        'data_round_by_type': {t: mean([per_round[r][t] for r in data_rounds])
                               for t in MSG_TYPES},
        'extra_per_election': mean([ctrl(r) for r in elect_rounds]) - base,
    }
    for fo in failovers:
        if fo['round'] is not None:
            fo['ctrl_msgs_takeover_round'] = ctrl(fo['round'])

    # ------------------------------------------------ communication overhead
    tot = collections.Counter()
    totb = collections.Counter()
    for node, st in last_stat.items():
        for i, t in enumerate(MSG_TYPES):
            tot[t] += split_vec(st['tx'])[i]
            totb[t] += split_vec(st['txB'])[i]
    payload_ctrl = sum(totb[t] for t in CTRL_TYPES)
    payload_all = sum(totb.values())
    air = {t: totb[t] + tot[t] * (FRAME_OVERHEAD_UCAST if t in UNICAST_TYPES
                                  else FRAME_OVERHEAD_BCAST)
           + (tot[t] * ACK_FRAME if t in UNICAST_TYPES else 0) for t in MSG_TYPES}
    res['overhead'] = {
        'messages': dict(tot), 'payload_bytes': dict(totb),
        'control_share_of_payload': payload_ctrl / payload_all if payload_all else float('nan'),
        'control_share_of_messages': sum(tot[t] for t in CTRL_TYPES) / sum(tot.values())
        if sum(tot.values()) else float('nan'),
        'on_air_bytes_estimate': air,
        'control_share_on_air': sum(air[t] for t in CTRL_TYPES) / sum(air.values())
        if sum(air.values()) else float('nan'),
        'mac_attempts': {n: s['mac'] for n, s in last_stat.items()},
    }

    # ------------------------------------------------------- data collection
    deliveries = collections.defaultdict(dict)   # src -> seq -> delivery round
    batch = collections.defaultdict(lambda: collections.defaultdict(list))
    for e in by['RD']:
        deliveries[e['src']].setdefault(e['seq'], e['r'])
        batch[e['r']][e['src']].append(e['seq'])
    lost, late, total = 0, 0, 0
    lost_by_src = {}
    for src, d in deliveries.items():
        seqs = sorted(d)
        if not seqs:
            continue
        expected = seqs[-1] - seqs[0] + 1
        lost_by_src[src] = expected - len(seqs)
        lost += expected - len(seqs)
        total += expected
    for r, srcs in batch.items():
        for src, seqs in srcs.items():
            late += max(0, len(set(seqs)) - 1)
    missing_rounds = []
    if rounds:
        missing_rounds = [r for r in range(rounds[0], rounds[-1] + 1) if r not in rnd]
    expected_members = [rnd[r]['slots'] for r in rounds]
    received = [rnd[r]['rx'] for r in rounds]
    res['data'] = {
        'samples_expected': total, 'samples_lost': lost,
        'delivery_ratio': (total - lost) / total if total else float('nan'),
        'samples_delivered_late': late, 'lost_by_src': lost_by_src,
        'rounds_without_leader': missing_rounds,
        'slot_success_ratio': sum(received) / sum(expected_members) if sum(expected_members) else float('nan'),
    }
    # disruption around each change of leader: rounds -2..+3
    disruption = []
    changes = [(p['round'], 'planned', p['term']) for p in planned] + \
              [(f['round'], 'failover', f['term']) for f in failovers if f['round']]
    for r0, kind, term in sorted(changes):
        win = range(max(r0 - 2, rounds[0]), r0 + 4)
        got = sum(rnd[r]['rx'] for r in win if r in rnd)
        exp = sum(rnd[r]['slots'] for r in win if r in rnd)
        lates = sum(max(0, len(set(s)) - 1) for r in win if r in batch for s in batch[r].values())
        disruption.append({'round': r0, 'kind': kind, 'term': term,
                           'rounds_without_leader': [r for r in win if r not in rnd],
                           'slot_success': f'{got}/{exp}', 'late_samples': lates})
    res['disruption'] = disruption

    # ---------------------------------------------------------------- energy
    energy = {}
    for node, st in sorted(last_stat.items()):
        e0 = boot.get(node, {}).get('e0')
        energy[node] = {'e0': e0, 'e_end': st['e'],
                        'used': (e0 - st['e']) if e0 is not None else None,
                        'radio_on_ms': st['radio_ms'], 'tx_on_ms': st['txon_ms'],
                        'uptime_ms': st['_ms'],
                        'duty_cycle_pct': 100.0 * st['radio_ms'] / st['_ms'] if st['_ms'] else None,
                        'last_role': st['role']}
        failed_after = [f for f in failsims if f['_node'] == node and f['_ms'] >= st['_ms']]
        dead = [x for x in by['ROLE'] if x['_node'] == node and x.get('role') == 'DEAD']
        if dead:
            energy[node]['last_role'] = 'DEAD'
        elif failed_after:
            energy[node]['last_role'] = 'FAILED'
    used = [v['used'] for v in energy.values() if v['used'] is not None]
    jain = (sum(used) ** 2) / (len(used) * sum(u * u for u in used)) if used and any(used) else float('nan')
    deaths = sorted((e['round'], e['_node']) for e in by['ROLE'] if e.get('role') == 'DEAD')
    res['radio_on_ms_per_round'] = {role: float(statistics.median(v)) for role, v in radio_by_role.items()}
    res['energy'] = {'per_node': energy, 'jain_fairness_used': jain,
                     'deaths': [{'node': n, 'round': r} for r, n in deaths],
                     'first_death_round': deaths[0][0] if deaths else None}

    # ---------------------------------------------------------------- memory
    mem = {'ram_static_bytes': max((b.get('ram_static', 0) for b in boot.values()), default=None),
           'protocol_state_bytes': max((b.get('state_bytes', 0) for b in boot.values()), default=None),
           'stack_peak_bytes': max((s.get('stack', -1) for d in stats.values() for s in d.values()), default=None)}
    if firmware:
        try:
            out = subprocess.run(['msp430-size', firmware], stdout=subprocess.PIPE,
                                 universal_newlines=True).stdout
            text, data, bss = map(int, out.splitlines()[1].split()[:3])
            mem.update({'flash_bytes': text + data, 'ram_data_bss_bytes': data + bss})
        except Exception as ex:  # noqa
            mem['size_error'] = str(ex)
    res['memory'] = mem
    return res


def report(res):
    c = res['config']
    p = print
    p(f"== Configuration: nodes {c['nodes']}, K={c['K']}, period={c['period_ms']} ms, "
      f"rotation every {c['rotation_rounds']} rounds")
    r = res['rounds']
    p(f"   rounds {r['first']}..{r['last']}, {r['with_leader']} with an active leader; "
      f"leader rounds per node: {res['leader_rounds_per_node']}")

    p("\n== Elections (decision = ELECT beacon -> RESULT, measured at the leader)")
    for el in res['elections']:
        p(f"   r={el['round']:<4} {el['reason']:<10} leader {el['leader']} -> {el['winner']}  "
          f"decision {el['decision_ms']} ms  relaxed={el['relaxed']}")
    ph = res['planned_handovers']
    if ph:
        gaps = [g for x in ph for g in x['member_gap_ms']]
        extra = [g for x in ph for g in x['handover_extra_ms']]
        p(f"   planned handovers: {len(ph)}; decision {fmt(mean([x['decision_ms'] for x in ph]), ' ms')}; "
          f"members' beacon gap {fmt(mean(gaps), ' ms')} (= period + {fmt(mean(extra), ' ms')})")

    p("\n== Failovers (detection measured at the backup, gap = last beacon of the old "
      "leader -> first beacon of the new one, at each member)")
    for f in res['failovers']:
        p(f"   term {f['term']}: {f['prev']} -> {f['new_leader']} (backup rank {f['backup_rank']}), "
          f"{'real failure' if f['real_failure'] else 'FALSE POSITIVE'}, detection "
          f"{f['detection_ms']} ms, member gap {fmt(mean(f['member_gap_ms']), ' ms')}, "
          f"new-leader election {f['decision_ms']} ms")
    p(f"   step-downs (two leaders resolved): {res['stepdowns']}, handover fallbacks: "
      f"{res['handover_fallbacks']}, orphan leaders: {res['orphans']}")
    tl = res['rounds_with_two_leaders']
    p(f"   rounds with two leaders at once: {len(tl)}" + (f" {tl[:12]}{' ...' if len(tl) > 12 else ''}" if tl else ''))

    cm = res['control_messages']
    p("\n== Control messages (whole group, per round)")
    p(f"   normal round: {fmt(cm['per_data_round'])}  {_types(cm['data_round_by_type'])}")
    p(f"   election round: {fmt(cm['per_election_round'])}  {_types(cm['election_round_by_type'])}")
    p(f"   extra messages per election: {fmt(cm['extra_per_election'])}")

    o = res['overhead']
    p("\n== Communication overhead (all messages sent during the run)")
    p(f"   messages: {o['messages']}")
    p(f"   payload bytes: {o['payload_bytes']}")
    p(f"   control share: {fmt(100 * o['control_share_of_messages'], ' %')} of messages, "
      f"{fmt(100 * o['control_share_of_payload'], ' %')} of payload bytes, "
      f"{fmt(100 * o['control_share_on_air'], ' %')} of estimated on-air bytes")

    d = res['data']
    p("\n== Sensor-data collection")
    p(f"   samples: {d['samples_expected']} expected, {d['samples_lost']} lost "
      f"(delivery {fmt(100 * d['delivery_ratio'], ' %')}), {d['samples_delivered_late']} "
      f"delivered late from the backlog; slot success {fmt(100 * d['slot_success_ratio'], ' %')}")
    p(f"   rounds without a leader: {d['rounds_without_leader']}")
    for x in res['disruption']:
        p(f"   around r={x['round']} ({x['kind']}): rounds without leader "
          f"{x['rounds_without_leader']}, slots {x['slot_success']}, late samples {x['late_samples']}")

    e = res['energy']
    p("\n== Energy (simulated units) and radio duty cycle (Energest)")
    for n, v in e['per_node'].items():
        p(f"   node {n}: {v['e0']} -> {v['e_end']} (used {v['used']}), radio on "
          f"{v['radio_on_ms']} ms = {fmt(v['duty_cycle_pct'], ' %')}, last role {v['last_role']}")
    rr = res['radio_on_ms_per_round']
    p("   radio on per round (Energest, median): " + ", ".join(
        f"{k} {fmt(v, ' ms')} = {fmt(100 * v / c['period_ms'], ' %')}" for k, v in sorted(rr.items())))
    p(f"   Jain fairness of energy used: {fmt(e['jain_fairness_used'], '')}; first death at round "
      f"{e['first_death_round']}; deaths: {[(d['node'], d['round']) for d in e['deaths']]}")

    m = res['memory']
    p("\n== Memory")
    p(f"   {m}")


def _types(d):
    return ' '.join(f"{t}={fmt(v)}" for t, v in d.items() if v and not math.isnan(v))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--firmware', help='.sky file for flash/RAM sizes (needs msp430-size)')
    ap.add_argument('--json', help='write all metrics to this JSON file')
    ap.add_argument('--csv-row', help='append a one-line summary to this CSV (for parameter studies)')
    ap.add_argument('--label', default='', help='label of this run in the CSV')
    a = ap.parse_args()
    events = parse(a.logs)
    if not events:
        sys.exit('no "LE ..." lines found')
    cluster_of = {e['_node']: e.get('cluster', 0) for e in events if e['_ev'] == 'BOOT'}
    clusters = sorted(set(cluster_of.values())) or [0]
    if len(clusters) > 1:
        out = {}
        for c in clusters:
            print(f"\n################ cluster {c} ################")
            ev_c = [e for e in events if cluster_of.get(e['_node'], 0) == c]
            out[c] = analyze(ev_c, a.firmware)
            report(out[c])
        res = out[clusters[0]]
        if a.json:
            with open(a.json, 'w') as f:
                json.dump(out, f, indent=1, default=str)
            a.json = None
    else:
        res = analyze(events, a.firmware)
        report(res)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(res, f, indent=1, default=str)
    if a.csv_row:
        fo = res['failovers']
        real = [f for f in fo if f['real_failure']]
        row = {
            'label': a.label, 'K': res['config']['K'],
            'failovers': len(fo), 'false_failovers': len(fo) - len(real),
            'detection_ms': mean([f['detection_ms'] for f in real if f['detection_ms']]),
            'recovery_gap_ms': mean([g for f in real for g in f['member_gap_ms']]),
            'planned_handovers': len(res['planned_handovers']),
            'delivery_ratio': res['data']['delivery_ratio'],
            'samples_lost': res['data']['samples_lost'],
            'late_samples': res['data']['samples_delivered_late'],
            'ctrl_per_round': res['control_messages']['per_data_round'],
            'ctrl_extra_per_election': res['control_messages']['extra_per_election'],
            'control_share_on_air': res['overhead']['control_share_on_air'],
            'stepdowns': res['stepdowns'],
            'two_leader_rounds': len(res['rounds_with_two_leaders']),
        }
        new = not os.path.exists(a.csv_row)
        with open(a.csv_row, 'a') as f:
            if new:
                f.write(','.join(row) + '\n')
            f.write(','.join(f'{v:.4g}' if isinstance(v, float) else str(v)
                             for v in row.values()) + '\n')


if __name__ == '__main__':
    main()
