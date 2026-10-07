#!/usr/bin/env python3
"""Game profiles (games/<id>/game.json) and the paths derived from them.

A profile may start with "inherits": "<other id>": it is then deep-merged over that profile
(objects merge key by key, anything else replaces; null removes the inherited value).
A file's "dir" may be a list of roms/ subfolders (e.g. a CHD shared by two sets): the first
one containing the file is used. "optional": true files may be missing (no known dump).

usage: game.py list
       game.py <id> binary|title            (used by the Makefile)
       game.py <id> check                   (verify the user's files against the expected SHA1s)
       game.py roms-readme                  (regenerate roms/*/README.txt from the profiles)
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROMS = os.path.join(ROOT, 'roms')


def ids():
    d = os.path.join(ROOT, 'games')
    return sorted(x for x in os.listdir(d) if os.path.exists(os.path.join(d, x, 'game.json')))


def merge(base, over):
    out = dict(base)
    for k, v in over.items():
        if v is None:
            out.pop(k, None)
        elif isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = merge(out[k], v)
        else:
            out[k] = v
    return out


def load(gid):
    p = os.path.join(ROOT, 'games', gid, 'game.json')
    if not os.path.exists(p):
        raise SystemExit(f"unknown game '{gid}' (known: {', '.join(ids())})")
    g = json.load(open(p))
    if 'inherits' in g:
        g = merge(load(g.pop('inherits')), g)
    return g


def dirs(f):
    return f['dir'] if isinstance(f['dir'], list) else [f['dir']]


def file_path(g, key):
    f = g['files'][key]
    cands = [os.path.join(ROMS, d, f['name']) for d in dirs(f)]
    return next((c for c in cands if os.path.exists(c)), cands[0])


def work_dir(g):
    return os.path.join(ROOT, 'work', g['id'])


def gen_dir(g):
    return os.path.join(ROOT, 'generated', g['id'])


def sha1_of(g, key):
    """SHA1 as listed by MAME: for a CHD the internal one reported by chdman, else the file hash."""
    p = file_path(g, key)
    if p.endswith('.chd'):
        if not shutil.which('chdman'):
            return None
        out = subprocess.run(['chdman', 'info', '-i', p], capture_output=True, text=True).stdout
        m = re.search(r'^SHA1:\s+([0-9a-f]{40})', out, re.M)
        return m.group(1) if m else None
    return hashlib.sha1(open(p, 'rb').read()).hexdigest()


def check(g, keys=None):
    ok = True
    for key in keys or g['files']:
        f, p = g['files'][key], file_path(g, key)
        rel = os.path.relpath(p, ROOT)
        if not os.path.exists(p):
            if f.get('optional'):
                print(f"  (absent) {rel}: optional, no known dump")
                continue
            print(f"  MISSING  {rel}")
            ok = False
            continue
        h = sha1_of(g, key)
        if not f.get('sha1'):
            print(f"  ?        {rel} (no reference SHA1)")
        elif h is None:
            print(f"  ?        {rel} (cannot compute SHA1)")
        elif h == f['sha1']:
            print(f"  OK       {rel}")
        else:
            print(f"  MISMATCH {rel}: {h}, expected {f['sha1']} (continuing anyway)")
    return ok


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def font_config(font):
    """GAME_ENH_FONT_*: the game font used by the enhanced-mode menus (runtime/enhanced.c).
    The pages (textures of one file) are stacked vertically into one atlas; each size is a grid
    of fixed cells, one row of characters per entry, drawn at the given scale."""
    if not font:
        return ["#define GAME_ENH_FONT_FILE NULL", "#define GAME_ENH_FONT_PAGES {0}", "#define GAME_ENH_FONT_W 0",
                "#define GAME_ENH_FONT_PAGE_H 0", "#define GAME_ENH_FONT_SIZES {{0}}", "#define GAME_ENH_FONT_ROWS {{-1}}"]
    sizes, rows = [], []
    for k, name in enumerate(('large', 'medium', 'small')):
        z = font['sizes'][name]
        sizes.append(f"{{{z['cell'][0]}, {z['cell'][1]}, {z.get('scale', 1.0)}f}}")
        rows += [f"{{{k}, {y}, {c_str(chars)}}}" for y, chars in z['rows']]
    return [f"#define GAME_ENH_FONT_FILE {c_str(font['file'])}",
            "#define GAME_ENH_FONT_PAGES {" + ", ".join(f"0x{int(o, 16):x}" for o in font['pages']) + "}",
            f"#define GAME_ENH_FONT_W {font['width']}", f"#define GAME_ENH_FONT_PAGE_H {font['page_height']}",
            "#define GAME_ENH_FONT_SIZES {" + ", ".join(sizes) + "}  /* large, medium, small: cell w, h, scale */",
            "#define GAME_ENH_FONT_ROWS {" + ", ".join(rows) + ", {-1}}  /* size, y, characters */"]


VALUE_SETS = {
    'difficulty': [["EASIEST", "FACILISSIMO"], ["VERY EASY", "MOLTO FACILE"], ["EASY", "FACILE"],
                   ["MEDIUM", "MEDIO"], ["MEDIUM HARD", "MEDIO DIFFICILE"], ["HARD", "DIFFICILE"],
                   ["VERY HARD", "MOLTO DIFFICILE"], ["HARDEST", "DIFFICILISSIMO"]],
    'offon': [["OFF", "NO"], ["ON", "SI"]],
}


def name_entry_config(ne):
    """GAME_ENH_NAME_*: the ranking's name entry, typed on the keyboard in the enhanced mode
    (runtime/enhanced.c). `chars` are the characters of the game's wheel from index 0, followed
    by DEL and END; at the "name_index" hook register `index_reg` holds the index the game took
    from the steering wheel, and `index_field` (optional) is a 16-bit copy of it at an offset
    from a register; at the "name_confirm" hook `confirm_reg` holds the confirmation check."""
    if not ne:
        return ["#define GAME_ENH_NAME_CHARS NULL", "#define GAME_ENH_NAME_INDEX_REG 0",
                "#define GAME_ENH_NAME_FIELD_REG -1", "#define GAME_ENH_NAME_FIELD_OFF 0",
                "#define GAME_ENH_NAME_CONFIRM_REG 0"]
    field = ne.get('index_field') or {}
    return [f"#define GAME_ENH_NAME_CHARS {c_str(ne['chars'])}",
            f"#define GAME_ENH_NAME_INDEX_REG {ne['index_reg']}",
            f"#define GAME_ENH_NAME_FIELD_REG {field.get('reg', -1)}",
            f"#define GAME_ENH_NAME_FIELD_OFF 0x{int(field.get('offset', '0'), 16):x}u",
            f"#define GAME_ENH_NAME_CONFIRM_REG {ne['confirm_reg']}"]


def net_ghost_config(gh):
    """GAME_NET_GHOST_*: a linked node whose packets stopped (runtime/net.c). With `keep` it stays
    a frozen ghost until the attract mode (a game that stops a linked race with NETWORK ERROR when a
    node is missing); without it, it is dropped after the short concealment. In the ghost's flags
    word (at `flags`, big-endian), when the mode field (`mode` = [shift, value]) says in game and the
    substate field (`substate` = [shift, race, first setup, last setup]) says racing, `race_over`
    is ORed in after 10 s (the others do not wait for it at the goal); in the setup substates the
    word at `solo` = [offset, bits] is ORed (the game drops a solo node from the race). The mode
    `error_mode` in this node's own packet means the game stopped on NETWORK ERROR."""
    gh = gh or {}
    mode = gh.get('mode', [0, 0])
    sub = gh.get('substate', [0, -1, -1, -1])
    solo = gh.get('solo', ['0', '0'])
    return [f"#define GAME_NET_GHOST_KEEP {1 if gh.get('keep') else 0}",
            f"#define GAME_NET_GHOST_FLAGS 0x{int(gh.get('flags', '0'), 16):x}",
            f"#define GAME_NET_GHOST_MODE_SHIFT {mode[0]}",
            f"#define GAME_NET_GHOST_MODE_GAME {mode[1]}",
            f"#define GAME_NET_GHOST_SUB_SHIFT {sub[0]}",
            f"#define GAME_NET_GHOST_SUB_RACE {sub[1]}",
            f"#define GAME_NET_GHOST_SUB_SETUP_LO {sub[2]}",
            f"#define GAME_NET_GHOST_SUB_SETUP_HI {sub[3]}",
            f"#define GAME_NET_GHOST_RACE_OVER 0x{int(gh.get('race_over', '0'), 16):x}u",
            f"#define GAME_NET_GHOST_SOLO_OFF 0x{int(solo[0], 16):x}",
            f"#define GAME_NET_GHOST_SOLO_BITS 0x{int(solo[1], 16):x}u",
            f"#define GAME_NET_ERROR_MODE {gh.get('error_mode', -1)}"]


def wheel_select_config(ws):
    """GAME_ENH_WHEEL_SELECTS: screens that take a choice from zones of the wheel position (the
    course select, GTI Club 2's transmission select), stepped with Left/Right in the enhanced mode
    (runtime/enhanced.c). Each key is the name of the hook that runs every frame of the screen;
    `positions` are wheel positions (-1 full left .. 1 full right) inside each choice's zone, from
    left to right, and the frontend holds the wheel at the chosen one."""
    rows = []
    for hook, w in (ws or {}).items():
        pos = w['positions']
        assert 2 <= len(pos) <= 8, hook
        rows.append(f"{{{c_str(hook)}, {len(pos)}, {{" + ", ".join(f"{float(v)!r}" for v in pos) + "}}")
    return "#define GAME_ENH_WHEEL_SELECTS {" + "".join(r + ", " for r in rows) + "{NULL, 0, {0}}}"


def text_fixes_config(fixes):
    """GAME_ENH_TEXT_FIXES: typos in the game's text textures, fixed in VRAM in the enhanced mode
    (runtime/enhanced.c). A fix is a band of rows `y` [first, end) of the A8 texture at `addr`
    (`stride` bytes per row), recognised by the CRC-32 of the original band; the band is then
    rebuilt left to right from `spans` of its own columns: [x0, x1] copies those columns, and
    {"rot180": [x0, x1, y0, y1]} copies that rectangle turned by 180 degrees (an n becomes a u).
    The line goes into columns `fit` [first, end), the part of the texture the game draws from,
    squeezed horizontally if it is wider."""
    rows = []
    for f in fixes or []:
        spans = []
        for s in f['spans']:
            if isinstance(s, dict):
                x0, x1, y0, y1 = s['rot180']
                spans.append(f"{{{x0}, {x1}, {y0}, {y1}}}")
            else:
                spans.append(f"{{{s[0]}, {s[1]}, -1, -1}}")
        assert len(spans) <= 24
        fit = f.get('fit', [0, f['stride']])
        rows.append(f"{{0x{int(f['addr'], 16):x}u, {f['stride']}, {f['y'][0]}, {f['y'][1]}, 0x{int(f['crc'], 16):08x}u, "
                    f"{fit[0]}, {fit[1]}, {len(spans)}, {{" + ", ".join(spans) + "}}")
    return "#define GAME_ENH_TEXT_FIXES {" + "".join(r + ", " for r in rows) + "{0}}"


def game_options_config(opts):
    """GAME_ENH_GAME_OPTIONS: the TEST MODE settings the enhanced-mode OPTIONS pages edit in the
    NVRAM. Each field is `bits` wide at `shift` in the byte (size 1) or big-endian word (size 2)
    at `addr`; values run from min to max, with English|Italian labels (none: shown as numbers)."""
    rows = []
    for o in opts or []:
        vals = o.get('values')
        if isinstance(vals, str):
            vals = VALUE_SETS[vals]
        text = c_str("\n".join(f"{en}|{it}" for en, it in vals)) if vals else 'NULL'
        page = {'game': 0, 'sound': 1}[o['page']]
        rows.append(f"{{{page}, {c_str(o['label'][0])}, {c_str(o['label'][1])}, 0x{int(o['addr'], 16):x}, {o.get('size', 1)}, "
                    f"{o['shift']}, {o['bits']}, {o['min']}, {o['max']}, {1 if o.get('language') else 0}, {text}}}")
    return "#define GAME_ENH_GAME_OPTIONS {" + ", ".join(rows + ["{-1}"]) + "}"


def write_config_header(g, out):
    """generated/<id>/game_config.h: the per-game constants the runtime is built with."""
    rel = lambda p: os.path.relpath(p, ROOT)
    inp = g['inputs']
    cal = g.get('calibration') or {}
    if 'script' not in cal:
        cal = {}
    nvo = g.get('nvram_options') or {}
    lines = [
        f"/* generated from games/{g['id']}/game.json */",
        "#pragma once",
        f"#define GAME_ID {c_str(g['id'])}",
        f"#define GAME_TITLE {c_str(g['title'] + ' (' + g['version'] + ')')}",
        f"#define GAME_DEFAULT_WORK {c_str(rel(work_dir(g)))}",
        f"#define GAME_DEFAULT_NVRAM {c_str(rel(file_path(g, 'nvram')))}",
        f"#define GAME_DEFAULT_DS2430 {c_str(rel(file_path(g, 'ds2430')))}",
        f"#define GAME_DEFAULT_BIOS {c_str(rel(file_path(g, 'bios')))}",
        f"#define GAME_NVRAM_SAVE {c_str(g['binary'] + '_nvram.bin')}",
        f"#define GAME_SETTINGS {c_str(g['binary'] + '_settings.ini')}",
        "#define GAME_INPUT_DEFAULTS {" + ", ".join(f"0x{v:02x}" for v in inp['defaults']) + "}",
        "#define GAME_ANALOG_REST {" + ", ".join(str(v) for v in inp['analog_rest']) + "}",
        f"#define GAME_HAS_HANDBRAKE {1 if inp.get('handbrake') else 0}",
        f"#define GAME_CALIBRATION_SCRIPT {c_str(cal['script']) if cal else 'NULL'}",
        f"#define GAME_CALIBRATION_SECONDS {cal.get('seconds', 0)}",
        # NVRAM bytes written after the calibration (settings TEST MODE cannot change), {-1} ends
        "#define GAME_CALIBRATION_NVRAM {" + "".join(f"{{0x{int(a, 16):x}, 0x{int(v, 16):02x}}}, "
                                                     for a, v in (cal.get('nvram_set') or {}).items()) + "{-1, 0}}",
        # TEST MODE option block of the NVRAM: words from start to the checksum word sum to 0xffff
        f"#define GAME_NVRAM_OPT_START {int(nvo.get('start', '0'), 16)}",
        f"#define GAME_NVRAM_OPT_CSUM {int(nvo.get('checksum', '0'), 16)}",
        # TEST MODE NETWORK ID (bits 6-7, ID - 1) in the NVRAM: set at boot to 1 or --net-id (0: none)
        f"#define GAME_NETWORK_ID_ADDR 0x{int((g.get('network') or {}).get('id_addr', '0'), 16):x}",
        *net_ghost_config((g.get('network') or {}).get('ghost')),
        # the game's own live NETWORK ID change (TEST MODE applies it at once): set_id(id - 1), and
        # the RAM copy of the NVRAM option block (RAM = options_ram + NVRAM offset); 0: restart instead
        f"#define GAME_NET_HOT_SET_ID 0x{int(((g.get('network') or {}).get('hot') or {}).get('set_id', '0'), 16):x}u",
        f"#define GAME_NET_HOT_OPTIONS_RAM 0x{int(((g.get('network') or {}).get('hot') or {}).get('options_ram', '0'), 16):x}u",
    ]
    # enhanced ("conversion") mode, runtime/enhanced.c: optional, absent for unverified versions
    enh = g.get('enhanced') or {}
    setup = enh.get('setup') or {}
    blanks = enh.get('blank_strings') or []
    lines += [
        f"#define GAME_HAS_ENHANCED {1 if enh else 0}",
        f"#define GAME_ENH_NVRAM_SAVE {c_str(g['binary'] + '_enhanced_nvram.bin')}",
        f"#define GAME_ENH_SETTINGS {c_str(g['binary'] + '_enhanced_settings.ini')}",
        f"#define GAME_ENH_SETUP_SCRIPT {c_str(setup['script']) if setup.get('script') else 'NULL'}",
        f"#define GAME_ENH_SETUP_SECONDS {setup.get('seconds', 0)}",
        # named hooks (the recompiler inserts rt_hook() there): GAME_ENH_HOOK_<NAME> = address for
        # a name used once, and GAME_ENH_HOOKS lists them all (a name may mark several addresses)
        *[f"#define GAME_ENH_HOOK_{name.upper()} 0x{int(a, 16):08x}u"
          for mod in (enh.get('hooks') or {}).values() for a, name in mod.items()
          if sum(n == name for m in (enh.get('hooks') or {}).values() for n in m.values()) == 1],
        "#define GAME_ENH_HOOKS {" + "".join(f"{{0x{int(a, 16):08x}u, {c_str(name)}}}, "
                                             for mod in (enh.get('hooks') or {}).values()
                                             for a, name in mod.items()) + "{0, NULL}}",
        *font_config(enh.get('font')),
        # widescreen: where the gl library keeps its projection slots and viewport (0: not supported)
        *[f"#define GAME_ENH_WIDE_{k.upper()} 0x{int((enh.get('widescreen') or {}).get(k, '0'), 16):x}u"
          for k in ('proj_matrix', 'proj_frustum', 'proj_slot', 'viewport')],
        game_options_config(enh.get('game_options')),
        *name_entry_config(enh.get('name_entry')),
        wheel_select_config(enh.get('wheel_select')),
        text_fixes_config(enh.get('text_fixes')),
        # TEST MODE main menu index of GAME MODE: the pause menu's "main menu" returns to the attract through it
        f"#define GAME_ENH_TEST_GAME_MODE {enh.get('test_menu_game_mode', -1)}",
        "#define GAME_ENH_BLANK_STRINGS {" + "".join(f"{{0x{int(b['addr'], 16):08x}u, {c_str(b['text'])}}}, " for b in blanks)
        + "{0, NULL}}",
    ]
    open(os.path.join(out, 'game_config.h'), 'w').write("\n".join(lines) + "\n")


def write_roms_readmes():
    """One README.txt per roms/ subfolder, listing the files expected there."""
    folders = {}
    for gid in ids():
        g = load(gid)
        for key, f in g['files'].items():
            for d in dirs(f):
                folders.setdefault(d, {}).setdefault(f['name'], (f, set()))[1].add(gid)
    for d, files in sorted(folders.items()):
        users = sorted({gid for _, (_, gs) in files.items() for gid in gs})
        own = [x for x in users if load(x)['mame_set'] == d]
        if d == 'kviper':
            head = ["Konami Viper BIOS set (MAME \"kviper\"), shared by every game."]
        else:
            users = own or users
            g = load(users[0])
            names = [g['title']] + g.get('aka', [])
            head = [f"{names[0]} ({g['version']}), MAME set \"{g['mame_set']}\"."]
            if len(names) > 1:
                head.append("Also known as: " + ", ".join(names[1:]) + ".")
            for n in g.get('notes', []):
                head.append("Note: " + n)
        lines = head + ["", "Put these files in this folder:", ""]
        w = max(len(n) for n in files)
        for name, (f, gs) in sorted(files.items(), key=lambda kv: (not kv[0].endswith('.chd'), kv[0])):
            if f.get('sha1'):
                desc = "SHA1 " + f['sha1'] + (" (internal CHD SHA1, see 'chdman info')" if name.endswith('.chd') else "")
            else:
                desc = "no known dump: optional, the game starts with an empty NVRAM"
            lines.append(f"  {name.ljust(w)}  {desc}")
            others = [x for x in dirs(f) if x != d]
            if others:
                lines.append(f"  {''.ljust(w)}  (shared with roms/{', roms/'.join(others)}/: one copy in either folder is enough)")
        lines.append("")
        if d != 'kviper':
            lines.append("Also needed: roms/kviper/ (941b01.u25, ds2430.u3).")
        lines.append("Check with: make check GAME=<id>" if d == 'kviper' else
                     "Check with: " + "; ".join(f"make check GAME={x}" for x in users))
        os.makedirs(os.path.join(ROMS, d), exist_ok=True)
        open(os.path.join(ROMS, d, 'README.txt'), 'w').write("\n".join(lines) + "\n")
        gk = os.path.join(ROMS, d, '.gitkeep')
        if os.path.exists(gk):
            os.remove(gk)


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a or a[0] == 'list':
        for i in ids():
            g = load(i)
            print(f"{i:12s} {g['title']} ({g['version']}) - {g['status']}")
    elif a[0] == 'roms-readme':
        write_roms_readmes()
    elif a[1] in ('binary', 'title'):
        print(load(a[0])[a[1]])
    elif a[1] == 'check':
        sys.exit(0 if check(load(a[0])) else 1)
    else:
        raise SystemExit(__doc__)
