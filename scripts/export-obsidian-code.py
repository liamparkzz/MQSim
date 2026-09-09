"""Read-only MQSim source export; generated notes and durable snapshots live in the vault."""
import argparse
import base64
import datetime as dt
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--vault', type=Path, required=True)
    p.add_argument('--git', default='git')
    p.add_argument('--reason', default='자동 기록: 변경 이유는 해당 Git 커밋 또는 작업 노트 참조')
    p.add_argument('--baseline-ref', default='51f0f2d3fed92d88ef4a0fa61a38024b07bf9d16')
    a = p.parse_args()
    repo = a.repo.resolve()
    if not (a.vault / '.obsidian').is_dir():
        raise RuntimeError('Expected an existing Obsidian vault')
    root = a.vault / 'MQSim SSD 연구' / 'Code'
    root.mkdir(parents=True, exist_ok=True)
    lock = root / '.export.lock'
    # OS releases the advisory lock even when a scheduled process is terminated.
    handle = lock.open('a+b')
    handle.seek(0)
    if os.name == 'nt':
        import msvcrt
        if lock.stat().st_size == 0:
            handle.write(b'0')
            handle.flush()
        handle.seek(0)
        msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def git(*args):
        return subprocess.check_output([a.git, '-c', f'safe.directory={repo.as_posix()}',
                                        '-C', str(repo), *args])

    def write(path, content):
        path.parent.mkdir(parents=True, exist_ok=True)
        data = content.encode('utf-8')
        if not path.exists() or path.read_bytes() != data:
            temp = path.with_name(path.name + '.tmp')
            temp.write_bytes(data)
            temp.replace(path)

    def text(data):
        return data.decode('utf-8-sig').replace('\r\n', '\n')

    def fence(content, language=''):
        marker = '`' * max(3, max((len(s) for s in __import__('re').findall(r'`+', content)), default=0) + 1)
        return f'{marker}{language}\n{content}' + ('' if content.endswith('\n') else '\n') + marker + '\n'

    def note(path, data):
        language = {'.cpp': 'cpp', '.h': 'cpp', '.hpp': 'cpp', '.xml': 'xml', '.html': 'html'}.get(Path(path).suffix, '')
        return f'# {path}\n\n원본 상대 경로: `{path}` · SHA-256: `{hashlib.sha256(data).hexdigest()}`\n\n' + fence(text(data), language)

    def diff(before, after):
        sections = []
        for path in sorted(before.keys() | after.keys()):
            old, new = before.get(path, b''), after.get(path, b'')
            # Windows checkout line endings do not constitute a source edit.
            if text(old) == text(new) and (path in before) == (path in after):
                continue
            patch = ''.join(difflib.unified_diff(text(old).splitlines(True), text(new).splitlines(True),
                            fromfile='a/' + path if path in before else '/dev/null',
                            tofile='b/' + path if path in after else '/dev/null'))
            sections.append(f'## {path}\n\n' + (fence(patch, 'diff') if patch else '바이트 또는 빈 파일 존재 여부 변경.\n'))
        return '\n'.join(sections)

    state_path = root / 'snapshot.json'
    state = json.loads(state_path.read_text('utf-8')) if state_path.exists() else None
    baseline_ref = state['baseline_ref'] if state else git('rev-parse', a.baseline_ref).decode().strip()
    scope = ['src', 'Makefile', 'MQSim.sln', 'MQSim.vcxproj', 'MQSim.vcxproj.filters', 'ssdconfig.xml', 'workload.xml',
             'LICENSE', 'README.md']
    baseline_paths = git('ls-tree', '-r', '--name-only', '-z', baseline_ref, '--', *scope).decode().strip('\0').split('\0')
    baseline = {path: git('show', f'{baseline_ref}:{path}') for path in baseline_paths if path}
    current_paths = git('ls-files', '--cached', '--others', '--exclude-standard', '-z', '--', *scope).decode().strip('\0').split('\0')
    current = {path: (repo / path).read_bytes() for path in sorted(set(current_paths)) if path and (repo / path).is_file()}
    head = git('rev-parse', 'HEAD').decode().strip()
    branch = git('branch', '--show-current').decode().strip()
    stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    previous = {k: base64.b64decode(v) for k, v in state['files'].items()} if state else baseline
    delta = diff(previous, current)
    if state is None or delta:
        title = stamp + (' 최초 기준' if state is None else ' 코드 변경')
        write(root / 'History' / (title + '.md'), f'# {title}\n\n- 기록 시각(UTC): {stamp}\n- 기기: {os.environ.get("COMPUTERNAME", "unknown")}\n- 브랜치: `{branch}`\n- HEAD: `{head}` (미커밋 변경 포함)\n- 사유: {a.reason}\n\n' + (delta or '공식 원본 기준과 전체 내용 일치.\n'))
    for path, data in baseline.items():
        target = root / 'Baseline' / (path + '.md')
        if not target.exists():
            write(target, note(path, data))
        elif target.read_text('utf-8') != note(path, data):
            raise RuntimeError(f'Baseline note was edited; preserve and review: {target}')
    for path, data in current.items():
        write(root / 'Current' / (path + '.md'), note(path, data))
    for path in previous.keys() - current.keys():
        write(root / 'Current' / (path + '.md'), f'# {path}\n\n현재 코드에서 삭제됨. [[MQSim 코드 변경 이력]]에서 이전 내용과 삭제 diff 확인.\n')
    # Export every reachable source commit after initialization, including edits between periodic snapshots.
    start = state['start_head'] if state else head
    commits = git('rev-list', '--reverse', f'{start}..{head}', '--', *scope).decode().splitlines()
    for commit in commits:
        target = root / 'History' / ('commit-' + commit + '.md')
        if not target.exists():
            meta = git('show', '-s', '--format=%cI%n%B', commit).decode('utf-8')
            patch = git('show', '--format=', '--first-parent', commit, '--', *scope).decode('utf-8')
            write(target, f'# Git 변경 {commit[:12]}\n\n{meta}\n[GitHub 커밋](https://github.com/liamparkzz/MQSim/commit/{commit})\n\n' + fence(patch, 'diff'))
    write(root / '원본 대비 누적 변경.md', '# 원본 대비 누적 변경\n\n기준: `' + baseline_ref + '`\n\n' + (diff(baseline, current) or '원본과 동일합니다.\n'))
    links = []
    for path in sorted(current):
        links.append(f'| `{path}` | [[MQSim SSD 연구/Code/Current/{path}.md\u007c현재 코드]] | [[MQSim SSD 연구/Code/Baseline/{path}.md\u007c원본]] |' if path in baseline else f'| `{path}` | [[MQSim SSD 연구/Code/Current/{path}.md\u007c현재 코드]] | 신규 파일 |')
    # Escape wikilink display separators inside Markdown tables.
    links = [line.replace('\u007c현재', '\\|현재').replace('\u007c원본', '\\|원본') for line in links]
    write(root / 'MQSim 전체 코드.md', '# MQSim 전체 코드\n\n공식 MQSim 원본을 기준으로 보존합니다. PR79 수정본이 아닙니다.\n\n'
          f'- 공식 원본 커밋: `{baseline_ref}`\n- 현재 소스 파일: {sum(p.startswith("src/") for p in current)}개 · 설정/빌드/문서 포함 {len(current)}개\n'
          '- Baseline: 변경하지 않는 원본 전문. Current: 로컬 작업 코드 전문. 노트는 자동 생성되므로 코드는 저장소에서 수정합니다.\n'
          '- [[MQSim 코드 변경 이력]] · [[원본 대비 누적 변경]] · [[코드 기록 사용법]]\n'
          '- 전체 src(내장 라이브러리 포함), 루트 빌드 파일·XML·README·LICENSE 포함. 실험 trace·논문·실행 산출물은 제외.\n\n'
          '| 파일 | 현재 | 최초 원본 |\n|---|---|---|\n' + '\n'.join(links) + '\n')
    histories = sorted((root / 'History').glob('*.md'), key=lambda f: f.name, reverse=True)
    write(root / 'MQSim 코드 변경 이력.md', '# MQSim 코드 변경 이력\n\n[[MQSim 전체 코드]] · [[원본 대비 누적 변경]]\n\n'
          '`-` 삭제 전 / `+` 추가 후. `@@ -이전줄,개수 +새줄,개수 @@`는 변경 줄 위치입니다.\n'
          '시각별 기록은 직전 기록과의 차이, commit 기록은 Git 커밋별 차이입니다. 같은 변경이 양쪽에 나타날 수 있습니다.\n\n'
          + '\n'.join(f'- [[MQSim SSD 연구/Code/History/{f.stem}]]' for f in histories) + '\n')
    write(state_path, json.dumps({'baseline_ref': baseline_ref, 'start_head': start,
                                 'files': {k: base64.b64encode(v).decode() for k, v in current.items()}}, ensure_ascii=False, indent=2) + '\n')
    print(f'Export OK: {len(current)} files; changed={previous != current}; {root}')
    handle.close()


if __name__ == '__main__':
    main()
