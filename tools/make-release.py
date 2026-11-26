#!/usr/bin/env python3
"""Make the zips for sharing privately (run from the project folder).

  tools/make-release.py <out dir>      (zips go in <out dir>/stable or /nightly)

  DQIX-PC-<version>.zip         the project as committed (git archive of HEAD):
                                BUILD.bat, the recompiler, the runtime... no game
                                code, no ROM
  DQIX-PC-<version>-extras.zip  the optional extras (wfc/: DQIX's downloads and
                                the DQVC schedule): game data, share with care

Both unzip into a folder called DQIX-PC, so unzipping the extras zip over the
project puts them in DQIX-PC/extras/ where BUILD.bat looks.
"""
import copy, io, os, re, subprocess, sys, zipfile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
ver = re.search(r'DQIX_VERSION "([^"]+)"', open('runtime/version.h').read()).group(1)
out = os.path.join(out, 'nightly' if 'nightly' in ver else 'stable')        # release/stable, release/nightly
os.makedirs(out, exist_ok=True)

# the project, exactly as committed, with Windows line endings for the .bat files
tar = subprocess.run(['git', 'archive', '--format=zip', '--prefix=DQIX-PC/', 'HEAD'], check=True, capture_output=True).stdout
src = zipfile.ZipFile(io.BytesIO(tar))
p = os.path.join(out, f'DQIX-PC-{ver}.zip')
with zipfile.ZipFile(p, 'w', zipfile.ZIP_DEFLATED) as z:
    for info in src.infolist():
        data = src.read(info)
        if info.filename.lower().endswith(('.bat', '.txt')) and not info.filename.startswith('DQIX-PC/config/'):
            data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
        out_info = copy.copy(info)                                     # (src's own entry stays readable)
        out_info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(out_info, data)
print(p, os.path.getsize(p) // 1024, 'KB,', len(src.infolist()), 'entries')

# RecompLauncher's zips (stable versions only: the launcher installs the newest
# release that has one, so a nightly's would reach every launcher player): the
# same project with recomplauncher.json at the zip's top, one zip per system.
# The Linux one leaves out the .bat files, so the launcher, trying
# recomplauncher.json's programs in order, finds BUILD.sh and game/dqix.
if 'nightly' not in ver:
    for system in ('Windows-x64', 'Linux-x64'):
        p = os.path.join(out, f'DQIX-Enhanced-{ver}-{system}.zip')
        n = 0
        with zipfile.ZipFile(p, 'w', zipfile.ZIP_DEFLATED) as z:
            for info in src.infolist():
                name = info.filename[len('DQIX-PC/'):]
                if not name or (system.startswith('Linux') and name.lower().endswith('.bat')):
                    continue
                data = src.read(info)
                if system.startswith('Windows') and name.lower().endswith(('.bat', '.txt')) and not name.startswith('config/'):
                    data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
                zi = zipfile.ZipInfo(name, info.date_time)
                zi.compress_type = zipfile.ZIP_DEFLATED
                zi.create_system = 3                                   # Unix, so the modes below count
                mode = 0o755 if name.endswith('.sh') or name.endswith('/') else 0o644
                zi.external_attr = ((0o40000 | mode) << 16 | 0x10) if name.endswith('/') else (0o100000 | mode) << 16
                z.writestr(zi, data); n += 1
        print(p, os.path.getsize(p) // 1024, 'KB,', n, 'entries (RecompLauncher)')

# the extras (only where they are: they don't change between nightlies)
if not os.path.isdir('wfc') or not any(os.path.isfile(os.path.join('wfc', g, '_list.txt')) for g in os.listdir('wfc')):
    print('no extras here (wfc/): skipped'); sys.exit(0)
p = os.path.join(out, f'DQIX-PC-{ver}-extras.zip')
n = 0
with zipfile.ZipFile(p, 'w', zipfile.ZIP_DEFLATED) as z:
    for game in sorted(os.listdir('wfc')):
        d = os.path.join('wfc', game)
        if not os.path.isfile(os.path.join(d, '_list.txt')):
            continue
        for f in sorted(os.listdir(d)):
            z.write(os.path.join(d, f), f'DQIX-PC/extras/wfc/{game}/{f}'); n += 1
print(p, os.path.getsize(p) // 1024, 'KB,', n, 'files')
