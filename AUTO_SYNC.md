# MQSim Git automatic synchronization

This repository is managed from `C:\CODEX\MQsim`.

The Windows scheduled task `MQSim Git Auto Sync` runs every five minutes while
the user is signed in. Each run:
The task uses `wscript.exe` as a hidden launcher, so no terminal window appears.

Credential prompts are disabled during scheduled runs, and any run that exceeds
two minutes is terminated instead of waiting invisibly.


1. stages working-tree changes;
2. skips secret-looking files and new files larger than 50 MB;
3. creates a timestamped commit when needed;
4. fetches and rebases onto `origin/main`;
5. pushes `main` to GitHub.

If a conflict occurs, the script aborts the rebase and leaves the local commits
intact for manual review. The log is stored at `.git\auto-sync.log`.

To reinstall or change the interval, run PowerShell as the signed-in user:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\install-auto-sync-task.ps1 `
  -IntervalMinutes 5
```

To run a synchronization immediately:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\mqsim-auto-sync.ps1
```

The task only synchronizes `main`. Work on another branch is intentionally left
for manual review and push.
