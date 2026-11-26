#!/usr/bin/env python3
"""Keep main's submodule pins on their branches' tips, with their patches.

Every submodule in .gitmodules names the branch it follows (`branch = source/wine`). main must
pin each one to that branch's tip, so a release never builds from a commit the branch has moved
past. For the trees exported as patch series (PATCH_DIRS), every commit a pin move brings in must
also have its `git format-patch` file in patches/. A submodule's `upstream` in .gitmodules (a
commit of the tree it is built on, such as the WineForge or Wine release merged into it) marks
that tree's commits: a pin move that merges a newer upstream needs patches only for the commits
the upstream doesn't have.

  pins.py check [--base REV] [--worktree] [--offline-ok]
      Fails if a pin (in the index) isn't its branch's tip, or if a commit between the pins in
      REV (default: HEAD) and the index has no patch. --worktree also requires each checked-out
      submodule to be at its pin, without changes to tracked files (for release builds).
  pins.py update [--base REV]
      Moves each stale pin in the index to its branch's tip and writes the missing patches
      (fetched from GitHub, numbered after the last one), staging both. The submodule checkouts
      are left as they are: `git submodule update src/<name>` moves one.

GH_TOKEN or GITHUB_TOKEN (or `gh auth token`) raises GitHub's API rate limit; public
repositories work without one.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

# submodule path -> its patch series. A commit's patch is the one from its sha, or one with its
# subject and the same changed lines (exported before a rebase merge or an amend gave it a new sha).
PATCH_DIRS = {
    'src/wine': 'patches/wine',
    'src/wine-x86_64': 'patches/wine-x86_64',
    'src/FEX': 'patches/fex',
}
# a pin move bringing in more commits than this is a rebase onto a new upstream release: its
# series is re-exported by hand, and the check then only asks that the patch directory changed
MAX_EXPORT = 60

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class NetworkError(Exception):
    pass


def git(*args, check=True):
    r = subprocess.run(['git', '-C', ROOT, *args], capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit(f'git {" ".join(args)}: {r.stderr.strip()}')
    return r.stdout


def submodules():
    out = git('config', '-f', '.gitmodules', '--get-regexp', r'^submodule\..*\.path$', check=False)
    subs = []
    for line in out.splitlines():
        key, path = line.split(' ', 1)
        name = key[len('submodule.'):-len('.path')]
        url = git('config', '-f', '.gitmodules', f'submodule.{name}.url').strip()
        branch = git('config', '-f', '.gitmodules', f'submodule.{name}.branch', check=False).strip()
        upstream = git('config', '-f', '.gitmodules', f'submodule.{name}.upstream', check=False).strip()
        subs.append({'path': path, 'url': url, 'branch': branch, 'upstream': upstream or None})
    return subs


def index_pin(path):
    out = git('ls-files', '-s', '--', path).split()
    return out[1] if out and out[0] == '160000' else None


def tree_pin(rev, path):
    out = git('ls-tree', rev, '--', path, check=False).split()
    return out[2] if len(out) > 2 and out[1] == 'commit' else None


_heads = {}


def branch_tip(url, branch):
    if url not in _heads:
        r = subprocess.run(['git', 'ls-remote', '--heads', url], capture_output=True, text=True,
                           timeout=120)
        if r.returncode:
            raise NetworkError(f'git ls-remote {url}: {r.stderr.strip()}')
        _heads[url] = {ref[len('refs/heads/'):]: sha for sha, ref in
                       (l.split('\t') for l in r.stdout.splitlines())}
    return _heads[url].get(branch)


def github_repo(url):
    m = re.match(r'https://github\.com/([^/]+/[^/]+?)(?:\.git)?/?$', url)
    return m.group(1) if m else None


_token = None


def token():
    global _token
    if _token is None:
        _token = os.environ.get('GH_TOKEN') or os.environ.get('GITHUB_TOKEN') or ''
        if not _token:
            try:
                _token = subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True,
                                        timeout=10).stdout.strip()
            except (OSError, subprocess.TimeoutExpired):
                pass
    return _token


def api(path, accept='application/vnd.github+json'):
    req = urllib.request.Request('https://api.github.com/' + path,
                                 headers={'Accept': accept, 'User-Agent': 'metalsharp-pins'})
    if token():
        req.add_header('Authorization', 'Bearer ' + token())
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            body = resp.read().decode('utf-8', 'surrogateescape')
    except (urllib.error.URLError, OSError) as e:
        raise NetworkError(f'GitHub API {path}: {e}')
    return body if accept.endswith('patch') else json.loads(body)


def compare_commits(repo, base, head):
    """Commits head has and base hasn't (a rewritten branch's new side too), oldest first, or None
    if there are more than the 250 GitHub lists."""
    cmp = api(f'repos/{repo}/compare/{base}...{head}?per_page=250')
    if cmp['status'] == 'behind' or cmp['ahead_by'] > 250:
        return None
    return cmp['commits']


def commit_range(repo, base, head, upstream=None):
    """Non-merge commits head has and base hasn't, without the upstream's, oldest first, or None
    if there are more than MAX_EXPORT of them."""
    commits = compare_commits(repo, base, head)
    if commits is not None and upstream:
        own = compare_commits(repo, upstream, head)
        if own is None:
            return None
        own = {c['sha'] for c in own}
        commits = [c for c in commits if c['sha'] in own]
    if commits is None:
        return None
    commits = [c for c in commits if len(c['parents']) == 1]
    return None if len(commits) > MAX_EXPORT else commits


def subject_of(message):
    return ' '.join(message.split('\n\n', 1)[0].split())


def fingerprint(patch):
    """The files and the added and removed lines of a patch, without line numbers or context."""
    diff = patch[patch.find('\ndiff --git '):].split('\n-- \n', 1)[0]
    return tuple(l for l in diff.split('\n') if l.startswith(('diff --git ', '+', '-'))
                 and not l.startswith(('+++ ', '--- ')))


def existing_patches(pdir):
    """(shas, {subject: [fingerprints]}, last number) of a patch series."""
    shas, subjects, last = set(), {}, 0
    d = os.path.join(ROOT, pdir)
    for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        m = re.match(r'(\d{4})-.*\.patch$', name)
        if not m:
            continue
        last = max(last, int(m.group(1)))
        with open(os.path.join(d, name), encoding='utf-8', errors='surrogateescape') as f:
            text = f.read()
        head = text[:8192]
        m2 = re.match(r'From ([0-9a-f]{40}) ', head)
        if m2:
            shas.add(m2.group(1))
        m3 = re.search(r'^Subject: (?:\[PATCH[^\]]*\] )?(.*(?:\n[ \t].*)*)', head, re.M)
        if m3:
            subjects.setdefault(' '.join(m3.group(1).split()), []).append(fingerprint(text))
    return shas, subjects, last


def missing_patches(path, repo, base, head, upstream=None):
    """(missing commits, error) for the pin move base -> head of a patched submodule."""
    pdir = PATCH_DIRS[path]
    commits = commit_range(repo, base, head, upstream)
    if commits is None:
        return None, (f'{base[:10]}..{head[:10]} is not a plain run of at most {MAX_EXPORT} commits '
                      f'(a rebase or a new upstream release): re-export {pdir} by hand')
    shas, subjects, _ = existing_patches(pdir)
    missing = []
    for c in commits:
        if c['sha'] in shas:
            continue
        same = subjects.get(subject_of(c['commit']['message']))
        if same and fingerprint(commit_patch(repo, c['sha'])) in same:
            continue
        missing.append(c)
    return missing, None


_patches = {}


def commit_patch(repo, sha):
    if sha not in _patches:
        _patches[sha] = api(f'repos/{repo}/commits/{sha}', accept='application/vnd.github.patch')
    return _patches[sha]


def slug(subject):
    s = re.sub(r'[^A-Za-z0-9._]+', '-', subject)
    s = re.sub(r'\.\.+', '.', s).strip('-.')
    return s[:52].rstrip('-.')


def write_patch(path, repo, commit):
    pdir = PATCH_DIRS[path]
    _, _, last = existing_patches(pdir)
    n = last + 1
    text = commit_patch(repo, commit['sha'])
    text = re.sub(r'^Subject: \[PATCH[^\]]*\]', f'Subject: [PATCH {n}/{n}]', text, count=1, flags=re.M)
    name = f'{n:04d}-{slug(subject_of(commit["commit"]["message"]))}.patch'
    os.makedirs(os.path.join(ROOT, pdir), exist_ok=True)
    with open(os.path.join(ROOT, pdir, name), 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(text)
    git('add', '--', os.path.join(pdir, name))
    return os.path.join(pdir, name)


def base_rev(rev):
    return rev if git('rev-parse', '--verify', '--quiet', rev + '^{commit}', check=False).strip() else None


def patch_dir_changed(base, pdir):
    return bool(git('diff', '--cached', '--name-only', base, '--', pdir).strip()) if base else False


def do_check(args):
    base = base_rev(args.base)
    problems = []
    for s in submodules():
        path, pin = s['path'], index_pin(s['path'])
        if not pin or not s['branch']:
            continue
        tip = branch_tip(s['url'], s['branch'])
        if tip is None:
            problems.append(f'{path}: branch {s["branch"]} does not exist in {s["url"]}')
        elif pin != tip:
            problems.append(f'{path}: pinned to {pin[:10]}, but {s["branch"]} is at {tip[:10]}')
        else:
            print(f'{path}: {pin[:10]} = {s["branch"]}')
        old = tree_pin(base, path) if base else None
        repo = github_repo(s['url'])
        if path in PATCH_DIRS and old and old != pin and repo:
            missing, err = missing_patches(path, repo, old, pin, s['upstream'])
            if err and not patch_dir_changed(base, PATCH_DIRS[path]):
                problems.append(f'{path}: {err}')
            for c in missing or []:
                problems.append(f'{path}: {c["sha"][:10]} "{subject_of(c["commit"]["message"])}" '
                                f'has no patch in {PATCH_DIRS[path]}')
        if args.worktree and os.path.exists(os.path.join(ROOT, path, '.git')):
            sub = ['git', '-C', os.path.join(ROOT, path)]
            head = subprocess.run(sub + ['rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
            dirty = subprocess.run(sub + ['status', '--porcelain', '--untracked-files=no'],
                                   capture_output=True, text=True).stdout.strip()
            if head != pin:
                problems.append(f'{path}: checked out at {head[:10]}, not its pin {pin[:10]} '
                                f'(git submodule update {path})')
            if dirty:
                problems.append(f'{path}: has changes to tracked files')
    for p in problems:
        print('stale: ' + p, file=sys.stderr)
    if problems:
        print('Run `python3 scripts/pins.py update` (the pre-commit hook does it for you).', file=sys.stderr)
    return 1 if problems else 0


def do_update(args):
    base = base_rev(args.base)
    failed = False
    for s in submodules():
        path, pin = s['path'], index_pin(s['path'])
        if not pin or not s['branch']:
            continue
        tip = branch_tip(s['url'], s['branch'])
        if tip is None:
            print(f'{path}: branch {s["branch"]} does not exist in {s["url"]}', file=sys.stderr)
            failed = True
            continue
        old = (tree_pin(base, path) if base else None) or pin
        repo = github_repo(s['url'])
        if path in PATCH_DIRS and old != tip and repo:
            missing, err = missing_patches(path, repo, old, tip, s['upstream'])
            if err:
                if not patch_dir_changed(base, PATCH_DIRS[path]):
                    print(f'{path}: {err}; pin left at {pin[:10]}', file=sys.stderr)
                    failed = True
                    continue
                missing = []
            for c in missing:
                print(f'{path}: wrote {write_patch(path, repo, c)}')
        if pin != tip:
            git('update-index', '--cacheinfo', f'160000,{tip},{path}')
            print(f'{path}: pin {pin[:10]} -> {tip[:10]} ({s["branch"]})')
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    for name in ('check', 'update'):
        c = sub.add_parser(name)
        c.add_argument('--base', default='HEAD', help='commit whose pins the index is compared with')
        c.add_argument('--offline-ok', action='store_true',
                       help='pass, with a warning, when GitHub cannot be reached')
        if name == 'check':
            c.add_argument('--worktree', action='store_true',
                           help='also require the submodule checkouts to be at their pins, unchanged')
    args = p.parse_args()
    try:
        return do_check(args) if args.cmd == 'check' else do_update(args)
    except (NetworkError, subprocess.TimeoutExpired) as e:
        print(f'pins: {e}', file=sys.stderr)
        if args.offline_ok:
            print('pins: not checked (offline); CI checks them again', file=sys.stderr)
            return 0
        return 2


if __name__ == '__main__':
    sys.exit(main())
