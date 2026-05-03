<p align="center">
  <h1 align="center"><code>sc</code></h1>
</p>

<p align="center">
Local restorable source history.
<br>
Small, explicit, commit-like tracking for source files without branches or remotes.
</p>

`sc` tracks files you explicitly add, stores commit messages with reverse patches,
and lets you restore older states as new commits. It is intentionally narrower than
Git: no push, no remote, no branch model, just clear local history for source files.


## INSTALLATION

Build the executable:

```sh
make sc
```

Then put `sc` on your `PATH`, or alias it from your shell profile:

```sh
alias sc="/path/to/source-control/sc"
```


## QUICK START

```sh
sc init
sc add src include
sc ignore build scratch.tmp
sc status
sc commit -m "initial source snapshot"

# Edit tracked files.
sc diff
sc commit -m "update parser"

# Review and restore history.
sc log
sc diff --latest
sc browse
sc restore 2
sc revert --no-warning src/main.c
```

Files added with `sc add` are tracked. Matching but untracked files are shown
by `sc status` and `sc ls --untracked`, and `sc commit`/`sc amend` capture them
automatically when nothing is already staged.
Ignored paths are skipped by future scans and explicit adds, but ignoring a path
does not untrack a file that is already tracked.

Long scans and diffs print throttled progress to stderr in interactive terminals.


## COMMANDS

### `sc init`

Create `.source-control/` in the current directory.

```sh
sc init
```

All other commands discover `.source-control/` in the current directory or a parent
directory up to `$HOME`.

### `sc add [paths...]`

Track and stage files. Explicit files are added directly; directories are scanned
recursively for text-like files.

```sh
sc add src include README.md
sc add
```

If no paths are given, `sc add` scans the current directory. Files inside `.git/`,
`.source-control/`, `build/`, `dist/`, and `.app` bundles are ignored.

### `sc ignore [--literal|--glob|--regex] <paths...>`

Add local ignore rules for files or directory trees.

```sh
sc ignore build
sc ignore scratch.tmp generated/cache
sc ignore '.DerivedData*'
sc ignore --regex '{.}[.]DerivedData.*'
```

Plain paths are exact path or subtree rules. Unflagged inputs containing `*` or
`?` are stored as glob rules; quote them so the shell does not expand them first.
`*` and `?` match within one path segment. Use `--literal` to store a path that
contains those characters literally, or `--regex` for the custom regex syntax
used by `sc trace --regex`.

Ignore rules are written to `.source-control/config`. They affect future scans:
ignored files do not appear as untracked and are skipped by `sc add`. Already
tracked files remain tracked until explicitly removed with `sc rm`.

### `sc rm <paths...>`

Remove tracked files and stage the deletions.

```sh
sc rm old/file.c
```

### `sc mv <old> <new>`

Move a tracked file and stage a tiny rename record.

```sh
sc mv old/file.c new/file.c
```

### `sc status`

Show the latest commit, staged changes, unstaged tracked changes, and up to 20
matching untracked files in separate sections. Small text changes include
best-effort `+added` and `-deleted` line counts like `(+6/-3)`; binary and
link-only changes skip counts. Interactive terminals color modified files green,
staged additions blue, untracked files orange, and deletions red.

```sh
sc status
```

If more than 20 untracked files match, status prints `... X more` and points to
`sc ls --untracked`.

If there are no staged, unstaged, or untracked changes, status prints
`nothing to commit`.

### `sc ls [--tracked] [--untracked] [--ignored]`

List tracked and untracked files.

```sh
sc ls
sc ls --tracked
sc ls --untracked
sc ls --ignored
```

By default, `sc ls` shows up to 10 tracked files and 10 untracked files. Use
`--tracked`, `--untracked`, or `--ignored` to print the full list for scripts.
Ignored output includes local ignore rules and built-in skipped directories such
as `.git`, `.source-control`, `build`, `dist`, and `.app` bundles, with ignored
directories listed once instead of listing their contents.

### `sc diff`

Show readable terminal diffs for local, staged, latest, or historical changes.

```sh
sc diff
sc diff src/main.c
sc diff --full
sc diff --staged
sc diff --latest
sc diff 2
sc diff 2 --against 1
sc diff --against 1 2
```

Default `sc diff` shows unstaged tracked changes. If there are staged changes and
no unstaged changes, it shows staged changes. Diffs include three unchanged context
lines around changes and omit output above 100 diff lines unless `--full` is used.
Historical diffs compare commit endpoints; for example,
`sc diff 2 --against 1` shows changes from commit `1` to commit `2`.

### `sc commit -m "message"`

Commit staged changes with a message.

```sh
sc commit -m "parse config"
```

If anything is staged, `commit` saves only the staged set and leaves other
changes unstaged. If nothing is staged, it automatically stages modified,
deleted, or type-changed tracked files plus matching untracked files.

### `sc amend [-m "message"]`

Fold staged changes into `HEAD`. If `-m` is supplied, replace the latest commit
message.

```sh
sc amend
sc amend -m "parse config"
```

Like `commit`, `amend` auto-stages tracked edits and matching untracked files
when nothing is already staged.

### `sc log [path]`

Show commits newest first. With a path, show only commits that touched that file.

```sh
sc log
sc log src/main.c
```

Multiline commit messages are displayed indented below the commit header.

### `sc browse [COMMIT] [FILE]`

Browse stored history size by commit and file.

```sh
sc browse
sc browse HEAD
sc browse HEAD source_control.c
```

With an interactive terminal, bare `sc browse` opens a simple keyboard browser:
Up/Down selects, Enter or Right opens, Left or Backspace goes back, and `q` quits.
Inside a file, edits are selectable; opening an edit shows a focused red/green
diff preview around that edit, and Up/Down scrolls the preview.
Edit rows show stored `size`, line `additions`, and line `deletions` for each
edit.
Without a terminal, bare `sc browse` prints a commit list. `sc browse COMMIT`
prints the files in that commit, largest stored record first. `sc browse COMMIT
FILE` prints storage details for that file's record in the commit. Sizes are
stored `.source-control/commits` bytes, not logical diff size.

### `sc clean`

Interactively rank and remove low-relevance committed history records.

```sh
sc clean
```

`sc clean` requires an interactive terminal and a clean stage/worktree. It ranks
records as `gone` for paths no longer in the current tracked source, `undone`
for exact done-then-undone changes, then `old` for older records that have a
newer change for the same current file. Press Enter to inspect a candidate and
`c` to clean it. Cleaning requires typing `clean` before history is rewritten.

### `sc trace <path>`

Show commits newest first with the hunks that touched a file, line range, or
regex-matched text.

```sh
sc trace src/main.c
sc trace src/main.c --lines 20:25
sc trace src/main.c --regex "parse.*config"
sc trace src/main.c 2..5
sc trace src/main.c --after 2 --before HEAD
```

`--lines A[:B]` traces the selected current lines backward through the file's
history. `--regex` uses the bundled tlux regex syntax and matches changed hunk
text. `START..END` is inclusive; `--after` is exclusive and `--before` is
inclusive.

### `sc squash COMMIT|HEAD|START..END|ALL`

Combine commits in the current linear history.

```sh
sc squash HEAD
sc squash 2..4
sc squash ALL
```

`sc squash HEAD` folds the latest commit into the previous commit and keeps the
previous ID. `START..END` squashes an inclusive range. `ALL` leaves one commit,
`0`, containing the combined history.

### `sc restore COMMIT`

Restore a commit's state by adding a new commit.

```sh
sc restore 4
sc restore HEAD
```

`restore` accepts commits in the current linear history. If the target already
matches `HEAD`, it prints `nothing to restore`.

### `sc revert [--no-warning] <path>`

Discard staged or unstaged changes to one file without changing commit history.

```sh
sc revert src/main.c
sc revert --no-warning src/main.c
```

By default, `revert` asks before discarding changes. For a newly added file with
no committed version, `revert` deletes the file and removes it from tracking.

### `sc destroy`

Delete only the `.source-control/` directory in the current directory.

```sh
sc destroy
```

This command does not search parents or children. It errors unless the current
directory itself contains `.source-control/`, prints what will be deleted, and
requires typing `destroy` before removing local history, staged data, index,
config, and commits.

### `sc import`

Convert the current Git branch's first-parent history into a new
`.source-control/` history.

```sh
sc import
```

Run this from inside a Git repository. The command refuses to run if the Git
worktree is dirty or `.source-control/` already exists at the Git root. Before
writing history, it prints the Git root, destination, branch, and commit count,
then requires typing `import`.

Git history is imported as a linear first-parent chain because `sc` has no
branch or merge model. The final imported state matches Git `HEAD`.


## CONFIGURATION

Global config is read from:

```text
~/.source-control.config
```

Local overrides live in:

```text
.source-control/config
```

The default scan matcher tracks text-like files and skips a small set of generated
output trees. The matcher can be configured with the bundled tlux regex syntax.


## STORAGE

Repository data lives under `.source-control/`:

```text
.source-control/
  config
  head
  index
  stage
  files/
  staged/
  commits/
```

`files/` stores the last committed source for tracked paths. `commits/` stores
reverse-oriented patches so older states can be restored from the current state.
