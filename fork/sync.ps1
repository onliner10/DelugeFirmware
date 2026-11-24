# Brings this fork up to date with upstream and rebuilds `next` from it:
#   1. fork's main  = upstream main (fast-forward only, never any own commits)
#   2. next        += upstream main, then every branch in fork/branches.txt
#   3. pushes main and next to origin
# Run it from the `next` worktree. It stops at the first conflict: resolve it, commit, and run it again.
# git rerere remembers each resolution, so the same conflict is resolved by itself next time.
param([switch]$NoPush)
# Not 'Stop': Windows PowerShell treats git's progress on stderr as an error. Failures are caught by exit code below.
$ErrorActionPreference = 'Continue'

function Invoke-Git {
	git @args
	if ($LASTEXITCODE -ne 0) { throw "git $args failed" }
}

if ((git branch --show-current) -ne 'next') { throw "Run this from the next worktree (on branch next)." }
if (git status --porcelain --untracked-files=no) { throw "next has uncommitted changes. Commit or drop them first." }

Invoke-Git fetch upstream
Invoke-Git fetch origin --prune

# 1. main mirrors upstream
Invoke-Git branch -f main upstream/main
if (-not $NoPush) { Invoke-Git push origin upstream/main:refs/heads/main }

# 2. next = upstream + own branches
$branches = Get-Content "$PSScriptRoot\branches.txt" | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object { $_.Trim() }
foreach ($ref in @('upstream/main') + ($branches | ForEach-Object { "origin/$_" })) {
	Write-Host "== merging $ref"
	git merge --no-edit $ref
	if ($LASTEXITCODE -ne 0) {
		$left = git diff --name-only --diff-filter=U
		if ($left) {
			Write-Host "Conflict merging $ref in:" -ForegroundColor Yellow
			$left | ForEach-Object { Write-Host "  $_" }
			throw "Resolve the files above, git add them, git commit, then run sync.ps1 again."
		}
		# rerere resolved every file: finish the merge
		Invoke-Git commit --no-edit
	}
}

# 3. publish
if (-not $NoPush) { Invoke-Git push origin next }
Write-Host "next is at $(git rev-parse --short HEAD). Build it with: .\dbt.cmd build release" -ForegroundColor Green
