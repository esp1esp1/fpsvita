#!/usr/bin/env python3
"""Pull the helper's play samples off the console and append them to a TSV.

  python3 -I tools/ingest_playlog.py --ip VITA_IP          # fetch, append, clear the log
  python3 -I tools/ingest_playlog.py --ip VITA_IP --keep   # leave the log in place
  python3 -I tools/ingest_playlog.py --ip VITA_IP --out mygames.tsv

Needs `log=1` in ur0:data/fpsvita/config.txt (reboot after adding it) and an
FTP server on the console's port 1337, such as VitaShell's (press Select in
VitaShell). Play normally, then run this.

The helper writes one `helper: <TITLEID> hooks=.. mask=..` line per launch, one
`helper: env vg=.. ib=.. fb=.. fps=.. arm=.. gpu=.. bus=..` line with the VitaGrafix
section and current clocks, and one `helper: rate ...` line per 3000 presents (frames 900,
3000, 6000, ...). Each rate line becomes a row tagged with the launch it belongs to, so a
title played under several settings yields one row per sample with the limit, vsync,
anti-lag, limiter and lowlat state in force. The src_*/vcb_*/cb_check columns are filled
only by development builds and stay empty otherwise.
"""
import argparse, csv, io, os, re, sys, time
from ftplib import FTP

ap = argparse.ArgumentParser()
ap.add_argument('--ip', required=True, help="the console's address, as VitaShell's FTP screen shows it")
ap.add_argument('--keep', action='store_true')
ap.add_argument('--out', default='play.tsv')
args = ap.parse_args()

COLS = ['ingested', 'title', 'launch', 'mask', 'pergame', 'init', 'gl', 'frames', 'flips', 'cur', 'inject',
        'ae_period', 'work', 'work_max', 'ctrl_after_pct', 'jit', 'slack', 'applied', 'missed',
        'present_lag', 'softcap', 'softcap_applied', 'll',
        # 2026-10-17 latency instrument (all us): controller sample age at the read
        # (min/ema/max), read phase and stamp phase after the capped wait returned,
        # fresh-poll hits/timeouts/spin, delivered sample age, input age at the next
        # flip, input age and submit-to-flip of the frame itself, exact flip ties,
        # unmatched flips, sleep backoffs for a later flip, doubled periods, CPU near
        # misses, IMMEDIATE calls/errors/first code
        'ts_min', 'ts_ema', 'ts_max', 'ts_n', 'rd_ph', 'ts_ph', 'fresh_hit', 'fresh_miss',
        'fresh_spin', 'out', 'scan', 'fscan', 'flag', 'tied', 'unmatched', 'hurt', 'miss',
        'near', 'low1', 'imm_calls', 'imm_err', 'imm_code', 'immq_wait', 'immq_calls', 'rr',
        'hp0', 'hp1', 'hp2', 'hp3', 'hpa0', 'hpa1', 'hpa2', 'hpa3', 'lt', 'btn_diff', 'btn_total',
        # Source probe (development builds), from the `helper: probe` line that follows each
        # rate line: calls in the rate window to sceDisplayGetVcount, sceRtcGetCurrentTick,
        # sceKernelGetSystemTimeWide, sceKernelGetProcessTimeLow, sceDisplayGetVcountInternal,
        # sceDisplayGetRefreshRate, sceKernelGetProcessTime, sceKernelGetProcessTimeWide,
        # sceKernelGetSystemTime, _sceRtcGetCurrentTick; then vblank-callback register and
        # unregister calls (cumulative for the process) and sceKernelCheckCallback calls in
        # the window. In-engine vs menus is what these are for.
        'src_vc', 'src_rtc', 'src_sys', 'src_tl', 'src_vcint', 'src_rr', 'src_pt', 'src_ptw',
        'src_st', 'src_rtc2', 'vcb_reg', 'vcb_unreg', 'cb_check',
        # `helper: env` at game start: the VitaGrafix section for the title
        # and the clocks another plugin has set.
        'vg_enabled', 'vg_ib', 'vg_fb', 'vg_fps', 'arm_mhz', 'gpu_mhz', 'bus_mhz',
        # Play-session samples are gate-less: the helper log has no screenshot
        # evidence, so phase stays `unknown` and gate `gate-less`.
        'phase', 'gate']
ENV_COLS = ['vg_enabled', 'vg_ib', 'vg_fb', 'vg_fps', 'arm_mhz', 'gpu_mhz', 'bus_mhz']
PROBE_COLS = 13
ENV_RE = re.compile(r'helper: env vg=(-?\d+) ib=(\S+) fb=(\S+) fps=(-?\d+)'
                    r' arm=(\d+) gpu=(\d+) bus=(\d+)')
PROBE_RE = re.compile(r'helper: probe src=(\d+)/(\d+)/(\d+)(?:/(\d+))?'
                      r'(?: vcint=(\d+) rr=(\d+) pt=(\d+) ptw=(\d+) st=(\d+) rtc2=(\d+))?'
                      r'(?: vcb=(\d+)/(\d+)/(\d+))?')
RATE_RE = (r'helper: rate frames=(\d+) flips/s=(\d+) cur=(-?\d+) inject=(\d+) since_vblank=\d+'
           r' ae_period=(\d+) work=(\d+) work_max=(\d+) ctrl_after_pct=(\d+) jit=(\d+) slack=(\d+)'
           r' applied=(\d+) missed=(\d+) present_lag=(\d+) softcap=(-?\d+) softcap_applied=(\d+)'
           r'(?: ll=(\d+))?'
           r'(?: ts=(-?\d+)/(-?\d+)/(-?\d+) ts_n=(\d+) rd_ph=(-?\d+) ts_ph=(-?\d+)'
           r' fresh=(\d+)/(\d+)/(\d+) out=(-?\d+) scan=(-?\d+)'
           r'(?: fscan=(-?\d+) flag=(\d+)(?: tied=(\d+))? unmatched=(\d+)(?: hurt=(\d+))?)?'
           r' miss=(\d+) near=(\d+)(?: low1=(\d+))? imm=(\d+)/(\d+)(?:/(0x[0-9a-f]+))?(?: immq=(\d+)/(\d+))?(?: rr=(-?\d+))?(?: hp=(\d+)/(\d+)/(\d+)/(\d+) hpa=(\d+)/(\d+)/(\d+)/(\d+))?(?: lt=(\d+))?(?: btn=(\d+)/(\d+))?)?')

f = FTP(); f.connect(args.ip, 1337, timeout=20); f.login('anonymous', '')
chunks = []
for remote in ('ur0:/data/fpsvita/helper.log.1', 'ur0:/data/fpsvita/helper.log'):
    b = io.BytesIO()
    try:
        f.retrbinary('RETR ' + remote, b.write)
        chunks.append(b.getvalue())
    except Exception as e:
        if remote.endswith('.log'):
            sys.exit(f'no helper.log on the console ({e})')
log = b''.join(chunks).decode(errors='replace')

rows = []; title = mask = pergame = init = ''; gl = ''; launch = 0; unmatched_rate = 0; orphan_rate = 0
env_vals = [''] * len(ENV_COLS)
stamp = time.strftime('%Y-%m-%d %H:%M')
# A probe line belongs to the rate line directly before it (same 600-present
# window). It is attached only then: a rate line the regex did not match must
# not hand its probe counts to the previous row.
probe_target = None
for l in log.splitlines():
    m = re.match(r'helper: (\S+) hooks=\d+ mask=0x(\w+) loaders=\d+ pergame=(-?\d+)(?: read=\d+)?(?: init=(\S+))?', l)
    if m:
        title, mask, pergame = m.group(1), '0x' + m.group(2), m.group(3); launch += 1
        # init: the GXM init import hooks per entry point (i landed, - not);
        # gl: 1 once the title's init went through vitaGL's sceGxmVshInitialize.
        init = m.group(4) or ''; gl = ''
        env_vals = [''] * len(ENV_COLS)
        probe_target = None; continue
    if l.startswith('helper: gxm vsh init'):
        gl = '1'; continue
    if l.startswith('helper: gxm init'):
        gl = '0'; continue
    m = ENV_RE.match(l)
    if m:
        env_vals = list(m.groups())
        continue
    if l.startswith('helper: rate'):
        m = re.match(RATE_RE, l)
        if m and title:
            rows.append([stamp, title, launch, mask, pergame, init, gl] + [g if g is not None else '' for g in m.groups()]
                        + [''] * PROBE_COLS + env_vals + ['unknown', 'gate-less'])
            probe_target = rows[-1]
        else:
            # No start line before it (the log was cleared mid-session) or a
            # format the regex does not know; either way the row is skipped.
            if m: orphan_rate += 1
            else: unmatched_rate += 1
            probe_target = None
        continue
    m = PROBE_RE.match(l)
    if m and probe_target is not None:
        probe_target[-2 - len(ENV_COLS) - PROBE_COLS:-2 - len(ENV_COLS)] = [
            g if g is not None else '' for g in m.groups()]
        probe_target = None

# An older play.tsv with fewer columns is re-headered in place, old rows padded.
if os.path.dirname(args.out):
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
old_rows = []
if os.path.exists(args.out):
    with open(args.out, newline='') as fh:
        old_rows = list(csv.reader(fh, delimiter='\t'))
if old_rows and old_rows[0] != COLS:
    old_header = old_rows[0]
    data = old_rows[1:]
    # Map old rows by column name so a new column can be inserted anywhere.
    # Fall back to append-and-pad when the old header is unknown or ambiguous.
    if (len(old_header) == len(set(old_header))
            and all(c in COLS for c in old_header)):
        pos = [COLS.index(c) for c in old_header]
        fixed = []
        for r in data:
            r = r + [''] * (len(old_header) - len(r))
            out = [''] * len(COLS)
            for j, v in enumerate(r[:len(old_header)]):
                out[pos[j]] = v
            fixed.append(out)
        data = fixed
    else:
        data = [r + [''] * max(0, len(COLS) - len(r)) for r in data]
        data = [r[:len(COLS)] for r in data]
    with open(args.out, 'w', newline='') as fh:
        w = csv.writer(fh, delimiter='\t', lineterminator='\n')
        w.writerow(COLS); w.writerows(data)
with open(args.out, 'a', newline='') as fh:
    w = csv.writer(fh, delimiter='\t', lineterminator='\n')
    if not old_rows: w.writerow(COLS)
    w.writerows(rows)
print(f'{len(rows)} samples from {launch} launches appended to {args.out}'
      + (f' ({unmatched_rate} rate lines did not match RATE_RE)' if unmatched_rate else '')
      + (f' ({orphan_rate} rate lines had no start line and were skipped)' if orphan_rate else ''))
for r in rows:
    d = dict(zip(COLS, r))
    print(f"  {d['title']} frames={d['frames']} flips={d['flips']} cur={d['cur']} jit={d['jit']} work={d['work']}"
          f" out={d['out']} fscan={d['fscan']} flag={d['flag']} fresh={d['fresh_hit']}/{d['fresh_miss']} applied={d['applied']} hurt={d['hurt']} ll={d['ll']}"
          + (f" src={d['src_vc']}/{d['src_rtc']}/{d['src_sys']}/{d['src_tl']}"
             f" pt={d['src_pt']}/{d['src_ptw']} vcb={d['vcb_reg']}/{d['vcb_unreg']}/{d['cb_check']}" if d['src_vc'] else ''))

if not args.keep:
    for remote in ('ur0:/data/fpsvita/helper.log.1', 'ur0:/data/fpsvita/helper.log'):
        try: f.delete(remote)
        except Exception as e:
            if not str(e).startswith('226'): print('could not clear ' + remote + ':', e)
f.quit()
