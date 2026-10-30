# Contributing
Thank you for helping. Two rules come before everything else:
- **No game data, ever.** No files from the disc, no ISOs, no recompiled code (`generated/`), no built NROs, no
  screenshots or dumps of the game's assets: not in commits, not in issues, not in pull requests.
- **No prebuilt NROs.** Everyone builds their own from their own copy of the game ([docs/HOW_TO_BUILD.md](docs/HOW_TO_BUILD.md)).

## Commit messages
Commits follow [Conventional Commits](https://www.conventionalcommits.org/):
```
<type>(<scope>): <what the change does, in the imperative>

<why, and anything a reader of the history needs, wrapped at 120 columns>
```
The scope is optional. CI checks the first line of every commit in a push or pull request
([tools/ci/check-commits.sh](tools/ci/check-commits.sh)).

| Type | For |
| --- | --- |
| `feat` | a new feature |
| `fix` | a bug fix |
| `docs` | documentation only |
| `style` | formatting, no change in behaviour |
| `refactor` | a code change that neither fixes a bug nor adds a feature |
| `perf` | a performance improvement |
| `test` | adding or correcting tests |
| `build` | the build system or external dependencies |
| `ci` | CI configuration |
| `chore` | anything else |

Scopes used so far: `switch`, `gpu`, `logging`, `docker`, `windows`, `readme`. Add `!` before the colon for
a change that breaks something users rely on (`feat(switch)!: ...`).

Examples from the history:
```
fix(switch): quit to the HOME menu without crashing
feat(gpu): apply the game's gamma ramp; keep replaced NROs in BAK/
ci: compile the Linux and Switch ports without the game, check the scripts
```

## Releases
The version lives in `CMakeLists.txt` (`project(scda VERSION x.y.z)`), and the Switch NRO carries it. For a release,
set it there, add a `## vx.y.z` section to [CHANGELOG.md](CHANGELOG.md), and push the tag `vx.y.z`: the release
workflow checks that the two match and publishes the notes with the build scripts.
