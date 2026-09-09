"""Exercise export lifecycle on a disposable repo; never edit the MQSim sources."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

exporter = Path(__file__).with_name('export-obsidian-code.py')
with tempfile.TemporaryDirectory(prefix='mqsim-export-test-') as temp:
    base = Path(temp)
    repo, vault = base / 'repo', base / 'vault'
    repo.mkdir()
    (vault / '.obsidian').mkdir(parents=True)
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args]).decode().strip()
    git('init', '-q', '-b', 'main')
    git('config', 'user.name', 'Export Test')
    git('config', 'user.email', 'test@example.invalid')
    (repo / 'src').mkdir()
    file = repo / 'src' / 'sample.cpp'
    file.write_text('int value = 1;\n', encoding='utf-8')
    git('add', '.')
    git('commit', '-qm', 'original')
    ref = git('rev-parse', 'HEAD')
    git('remote', 'add', 'origin', str(repo))
    git('fetch', '-q', 'origin')
    root = vault / 'MQSim SSD 연구' / 'Code'
    def run():
        subprocess.run([sys.executable, str(exporter), '--repo', str(repo), '--vault', str(vault),
                        '--baseline-ref', ref, '--reason', 'fixture verification'], check=True, stdout=subprocess.DEVNULL)
    run()
    baseline = (root / 'Baseline/src/sample.cpp.md').read_bytes()
    first = {p: p.read_bytes() for p in root.rglob('*') if p.is_file()}
    run()
    assert all(p.read_bytes() == data for p, data in first.items()), 'No-op changed output'
    file.write_bytes(b'int value = 1;\r\n')
    run()
    assert len(list((root / 'History').glob('*.md'))) == 1, 'CRLF created spurious history'
    file.write_text('int value = 2;\n', encoding='utf-8')
    (repo / 'src/new.h').write_text('#pragma once\n', encoding='utf-8')
    run()
    assert (root / 'Current/src/sample.cpp.md').read_bytes() == baseline, 'Unpublished edit leaked'
    git('add', '.')
    git('commit', '-qm', 'modify and add\n\n수정 이유: fixture\n변경 설명: value 1 -> 2\n검증: test')
    # Ref mismatch must fail without replacing published notes.
    result = subprocess.run([sys.executable, str(exporter), '--repo', str(repo), '--vault', str(vault),
                             '--baseline-ref', ref], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert result.returncode != 0
    git('fetch', '-q', 'origin')
    run()
    history = '\n'.join(p.read_text('utf-8') for p in (root / 'History').glob('*.md'))
    assert '-int value = 1;' in history and '+int value = 2;' in history and '@@' in history
    assert (root / 'Current/src/new.h.md').exists()
    commit = git('rev-parse', 'HEAD')
    run()
    assert (root / 'History' / f'commit-{commit}.md').exists()
    file.rename(repo / 'src/renamed.cpp')
    (repo / 'src/new.h').unlink()
    git('add', '-A')
    git('commit', '-qm', 'rename and delete')
    git('fetch', '-q', 'origin')
    run()
    assert '삭제됨' in (root / 'Current/src/sample.cpp.md').read_text('utf-8')
    assert (root / 'Current/src/renamed.cpp.md').exists()
    assert (root / 'Baseline/src/sample.cpp.md').read_bytes() == baseline
    print('PASS: baseline, no-op, unpublished edit exclusion, remote mismatch rejection, edit/add/rename/delete, commit explanations')
