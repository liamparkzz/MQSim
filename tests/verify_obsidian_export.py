"""Verify an actual published commit was exported with explanation and exact code."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--vault', type=Path, required=True)
p.add_argument('--commit', required=True)
p.add_argument('--report', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
root = a.vault / 'MQSim SSD 연구' / 'Code'
def git(*args):
    return subprocess.check_output(['git', '-c', f'safe.directory={repo.as_posix()}', '-C', str(repo), *args])
state = json.loads((root / 'snapshot.json').read_text('utf-8'))
assert state['published_head'] == a.commit
history = root / 'History' / f'commit-{a.commit}.md'
content = history.read_text('utf-8')
when = git('show', '-s', '--format=%cI', a.commit).decode().strip()
message = git('show', '-s', '--format=%B', a.commit).decode().strip()
assert when in content and message in content
assert all(field in content for field in ['수정 이유:', '변경 설명:', '검증:', '관련 이슈:'])
assert '아직 작성되지 않음' not in content
assert f'https://github.com/liamparkzz/MQSim/commit/{a.commit}' in content
changed = git('diff-tree', '--no-commit-id', '--name-only', '-r', a.commit, '--', 'src').decode().splitlines()
assert all(f'diff --git a/{path} b/{path}' in content for path in changed)
checked = 0
for path, encoded in state['files'].items():
    data = git('show', f'{a.commit}:{path}')
    assert base64.b64decode(encoded) == data, path
    note = (root / 'Current' / (path + '.md')).read_text('utf-8')
    assert hashlib.sha256(data).hexdigest() in note
    code = re.search(r'\n(`{3,})[^\n]*\n(.*)\n\1\n\Z', note, re.S)
    expected = data.decode('utf-8-sig').replace('\r\n', '\n')
    if not expected.endswith('\n'):
        expected += '\n'
    assert code and code.group(2) + '\n' == expected, path
    checked += 1
baseline_count = 0
for note in (root / 'Baseline').rglob('*.md'):
    path = note.relative_to(root / 'Baseline').as_posix()[:-3]
    data = git('show', f'{state["baseline_ref"]}:{path}')
    assert hashlib.sha256(data).hexdigest() in note.read_text('utf-8'), path
    baseline_count += 1
report = {'commit': a.commit, 'commit_time': when, 'history_note': str(history), 'reason_and_explanation_present': True,
          'diffs_for_changed_source_files': len(changed), 'current_files_match_published_git_blobs': checked,
          'baseline_files_match_original_git_hashes': baseline_count}
a.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(report, ensure_ascii=True))
