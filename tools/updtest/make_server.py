# Test server files for the updater (windows.sh, linux.sh): a release JSON like GitHub's API
# gives, pointing at packages served from this folder over http://127.0.0.1:<port>.
#   python make_server.py <srv dir> <port> <version> <mode> <full package> <update package>
# mode: slim (both packages listed), full (only the full one), badsum (wrong SHA-256),
#       broken (an update package whose SDK cannot build: Setup must put the old files back)
import hashlib, json, os, shutil, sys, zipfile, tarfile, io

srv, port, ver, mode, full, slim = sys.argv[1:7]
os.makedirs(srv, exist_ok=True)

def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()

def with_marker(src, dst, drop=None):
    """the package plus RondoRecomp/update_marker.txt (to see the files were replaced);
    drop: a member left out"""
    marker = ('installed by the updater test, ' + mode + '\n').encode()
    if src.endswith('.zip'):
        with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as zo:
            for it in zi.infolist():
                if drop and it.filename.endswith(drop):
                    continue
                zo.writestr(it, zi.read(it.filename))
            zo.writestr('RondoRecomp/update_marker.txt', marker)
    else:
        with tarfile.open(src, 'r:gz') as ti, tarfile.open(dst, 'w:gz', format=tarfile.GNU_FORMAT) as to:
            for m in ti:
                if drop and m.name.endswith(drop):
                    continue
                to.addfile(m, ti.extractfile(m) if m.isfile() else None)
            m = tarfile.TarInfo('RondoRecomp/update_marker.txt')
            m.size = len(marker)
            m.mode = 0o644
            to.addfile(m, io.BytesIO(marker))

assets = []
def add(path):
    name = os.path.basename(path)
    assets.append({
        'name': name, 'size': os.path.getsize(path), 'digest': 'sha256:' + sha(path),
        'browser_download_url': 'http://127.0.0.1:%s/%s' % (port, name),
    })

slim_out = os.path.join(srv, os.path.basename(slim))
full_out = os.path.join(srv, os.path.basename(full))
if mode in ('slim', 'badsum', 'broken'):
    with_marker(slim, slim_out, drop='sdk/src/frontend/main.c' if mode == 'broken' else None)
    add(slim_out)
if mode in ('slim', 'full', 'badsum'):
    with_marker(full, full_out)
    add(full_out)
if mode == 'badsum':
    for a in assets:
        a['digest'] = 'sha256:' + '0' * 64

body = ('Test release.\n\n## Downloads\n\n- **Windows:** the zip.\n\n## New in %s\n\n'
        '- **Updates** from the start screen: the game says when a new version is out. Then more.\n'
        '- A [linked](https://example.com) point with `code`.\n  - a nested point (not shown)\n'
        '- Third point.\n- Fourth point.\n- Fifth point (not shown).\n' % ver)
release = {
    'tag_name': 'v' + ver, 'name': 'RondoRecomp ' + ver, 'draft': False, 'prerelease': False,
    'html_url': 'https://github.com/gminustwo/RondoRecomp/releases/tag/v' + ver,
    'assets': assets, 'body': body,
}
with open(os.path.join(srv, 'latest.json'), 'w', encoding='utf-8') as f:
    json.dump(release, f, indent=1)
print('served:', [a['name'] for a in assets])
