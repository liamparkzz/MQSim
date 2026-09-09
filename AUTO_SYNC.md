# MQSim Git automatic synchronization

On the laptop, each run calls `scripts/export-obsidian-code.ps1` only after
successful Git synchronization. It verifies origin/main against the remote and
exports that committed tree, excluding unpublished local edits. Commit bodies
provide the reason, behavior explanation, and validation for Obsidian history.
For an immediate snapshot with a reason, run that script with `-Reason`.
The separate desktop worker is not yet connected to this exporter.

Desktop checkout: `D:\D_Drive_Codex\MQsimDT`.
Laptop checkout: `C:\CODEX\MQsim`.

On the desktop, `D:\D_Drive_Codex\.sync\install-sync-tasks.ps1` installs
`MQSim Git Auto Sync` and `Obsidian Git Auto Sync`. They run silently every
five minutes while signed in, and at sign-in. Obsidian does not need to be open.
The desktop worker commits local changes, fetches, merges without rewriting
history, and pushes `main`. A conflict aborts the merge and preserves local
commits. Each repository records results in `.git\auto-sync.log`.
Obsidian Git's automatic timers are disabled on this desktop to avoid overlap.
Previously copied experiment data remain local through `.git\info\exclude`.

The following bundled-script instructions describe the laptop setup.

The Windows scheduled task `MQSim Git Auto Sync` runs every five minutes while
the user is signed in. Each run:
The task uses `wscript.exe` as a hidden launcher, so no terminal window appears.

Credential prompts are disabled during scheduled runs, and any run that exceeds
four minutes is terminated instead of waiting invisibly. HTTP transfers that
make no progress for 30 seconds fail quickly and are recorded in the sync log.


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
