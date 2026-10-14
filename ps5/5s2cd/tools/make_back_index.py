#!/usr/bin/env python3
# 5S2CD: makes the case backs' index the app ships (ps5/frontend/assets/backs.txt): one line per PS2 serial,
# "<serial> <LaunchBox image file>", the image at https://images.launchbox-app.com/<file>. Every serial in PCSX2's
# GameIndex.yaml is matched to a LaunchBox Games Database "Sony Playstation 2" game by name (English name first,
# then the database's alternate names, then the main title when only one game has it), and that game's
# "Box - Back" is picked for the serial's own region first. back_overrides.txt (next to this script) holds the
# matches checked by hand that the names don't reach.
#
#   wget https://gamesdb.launchbox-app.com/Metadata.zip
#   ps5/5s2cd/tools/make_back_index.py Metadata.zip bin/resources/GameIndex.yaml ps5/frontend/assets/backs.txt
#
# SPDX-License-Identifier: GPL-3.0-or-later
import difflib, os, re, sys, zipfile, collections
import xml.etree.ElementTree as ET
if len(sys.argv) != 4:
    sys.exit('usage: make_back_index.py <Metadata.zip> <GameIndex.yaml> <backs.txt>')
ZIP, GI, OUT = sys.argv[1:4]
ROMAN = {'ii': '2', 'iii': '3', 'iv': '4', 'v': '5', 'vi': '6', 'vii': '7', 'viii': '8', 'ix': '9', 'x': '10'}
PAL = ['Europe', 'United Kingdom', 'Germany', 'France', 'Spain', 'Italy', 'Australia', 'Netherlands', 'Sweden',
       'Scandinavia', 'Norway', 'Denmark', 'Finland', 'Portugal', 'Russia', 'Greece', 'Poland', 'World']
JP = ['Japan', 'Asia', 'Korea', 'China', 'Hong Kong', 'Taiwan']
US = ['North America', 'United States', 'Canada', 'World']
ALIAS = {'SLPM-62703': 'Sega Rally Championship'}  # (checked by hand)

def norm(t):
    t = t.lower().replace('&', ' and ')
    t = re.sub(r'\(.*?\)|\[.*?\]', ' ', t)
    t = re.sub(r'[^a-z0-9]+', ' ', t)
    return ''.join(ROMAN.get(x, x) for x in t.split() if x not in ('the', 'a', 'an'))

def regions(serial, region):
    if region.startswith('PAL'):
        return PAL, US + JP
    if region in ('NTSC-J', 'NTSC-K', 'NTSC-C'):
        return JP, US + PAL
    return US, PAL + JP

# GameIndex.yaml: serial -> (name, name-en, region); a plain line scan (the file is regular)
gi, cur = {}, None
for line in open(GI, encoding='utf-8'):
    m = re.match(r'^([A-Z]{4}-\d{5}):\s*$', line)
    if m:
        cur = m.group(1); gi[cur] = {'name': '', 'name-en': '', 'region': ''}
        continue
    m = re.match(r'^  (name|name-en|region): "(.*)"\s*$', line)
    if m and cur:
        gi[cur][m.group(1)] = m.group(2)

games, names, imgs = {}, collections.defaultdict(set), collections.defaultdict(list)
mains = collections.defaultdict(set)  # LaunchBox titles without their subtitle
with zipfile.ZipFile(ZIP).open('Metadata.xml') as f:
    for ev, el in ET.iterparse(f):
        if el.tag == 'Game':
            if el.findtext('Platform') == 'Sony Playstation 2':
                gid = el.findtext('DatabaseID'); games[gid] = el.findtext('Name') or ''
                names[norm(games[gid])].add(gid)
                mains[norm(re.split(r' - |: ', games[gid])[0])].add(gid)
            el.clear()
        elif el.tag == 'GameAlternateName':
            names[norm(el.findtext('AlternateName') or '')].add(el.findtext('DatabaseID'))
            el.clear()
        elif el.tag == 'GameImage':
            if el.findtext('Type') == 'Box - Back':
                imgs[el.findtext('DatabaseID')].append((el.findtext('Region') or '', el.findtext('FileName')))
            el.clear()

lines, stats, bymain, used = [], collections.Counter(), [], []
# the matches checked by hand, used where the names don't meet
reviewed = {}
_ov = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'back_overrides.txt')
if os.path.exists(_ov):
    for _l in open(_ov, encoding='utf-8'):
        if _l.strip() and not _l.startswith('#'):
            _k, _v = _l.split()
            reviewed[_k] = _v
for s in sorted(gi):
    g = gi[s]
    cands = set()
    for t in (ALIAS.get(s, ''), g['name-en'], g['name'], re.split(r' - |: ', g['name-en'] or g['name'])[0]):
        n = norm(t)
        if len(n) >= 3:
            cands = {x for x in names.get(n, ()) if x in games}
        if cands:
            break
    if not cands:
        # "WRC 4" for "WRC 4: FIA World Rally Championship": the main title, when only one PS2 game has it
        for t in (g['name-en'], g['name']):
            t = re.sub(r'\[.*?\]', '', t).strip()
            parts = re.split(r' - |: ', t, maxsplit=1)
            n = norm(parts[0])
            if len(n) >= 4 and len(mains.get(n, ())) == 1:
                gid = min(mains[n])
                if len(parts) > 1:  # both have a subtitle: they must be alike ("Ten he Kaere" ~ "Sora e Kaere")
                    lb_parts = re.split(r' - |: ', games[gid], maxsplit=1)
                    a, b = norm(parts[1]), norm(lb_parts[1]) if len(lb_parts) > 1 else ''
                    if not b or difflib.SequenceMatcher(None, a, b).ratio() < 0.6:
                        continue
                    if a.startswith(b) and len(a) - len(b) >= 3:  # "... Gaiden": a game of its own
                        continue
                cands = {gid}; stats['by main title'] += 1
                bymain.append('%s | %s => %s' % (s, t, games[next(iter(cands))]))
                break
    if not cands and s in reviewed:
        lines.append('%s %s' % (s, reviewed[s])); stats['reviewed'] += 1; used.append(s); continue
    if not cands:
        stats['no game'] += 1; continue
    own, other = regions(s, g['region'])
    best = None
    for x in sorted(cands, key=lambda d: int(d) if d.isdigit() else 0):  # the same pick every run
        for reg, fn in imgs.get(x, []):
            r = own.index(reg) if reg in own else (100 + other.index(reg) if reg in other else (150 if not reg else 200))
            if best is None or r < best[0]:
                best = (r, fn)
    if not best and s in reviewed:
        lines.append('%s %s' % (s, reviewed[s])); stats['reviewed'] += 1; used.append(s); continue
    if not best:
        stats['no back'] += 1; continue
    stats['own region' if best[0] < 100 else 'other region'] += 1
    lines.append('%s %s' % (s, best[1]))
hdr = ('# 5S2CD back covers: <serial> <file>, the image at https://images.launchbox-app.com/<file>\n'
       '# (LaunchBox Games Database, matched by make_back_index.py; a file in covers/back/ always wins)\n')
open(OUT, 'w', encoding='utf-8', newline='\n').write(hdr + '\n'.join(lines) + '\n')
print('serials', len(gi), dict(stats), 'lines', len(lines))
