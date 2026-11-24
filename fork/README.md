# This fork's branches

| Branch | What it is | Commits of your own? |
| --- | --- | --- |
| `main` | Exact copy of upstream `main` | Never |
| `perf/...`, `feat/...`, `fix/...` | One change each, started from upstream `main`; each can be a PR upstream | Yes |
| `next` | Your firmware: upstream `main` plus every branch in `fork/branches.txt` | Only merges, and files under `fork/` |

## Update from upstream

From the `next` worktree (`E:\git\DelugeFirmware-next`):

```powershell
.\fork\sync.ps1
```

It fast-forwards the fork's `main`, merges upstream and each listed branch into `next`, and pushes. Each push of
`next` builds the firmware on GitHub Actions and publishes it as the **next-build** pre-release
(`.github/workflows/fork-next.yml`). Or build it locally with `.\dbt.cmd build release`.

On a conflict it stops: fix the files, `git add`, `git commit`, run it again. `git rerere` remembers the fix.

## Start a new change (to send upstream)

From the main checkout, not from `next`:

```powershell
git fetch upstream
git switch -c perf/my-change upstream/main
# ...commit...
git push -u origin perf/my-change
```

Then add `perf/my-change` to `fork/branches.txt` and run `sync.ps1`. Open the PR from that branch to
`SynthstromAudible/DelugeFirmware:main`, never from `next`.

When upstream merges it, remove its line from `fork/branches.txt`.

## Keep a branch current for its PR

```powershell
git switch perf/my-change
git merge upstream/main   # or rebase, if the PR has no reviews yet
git push
```
