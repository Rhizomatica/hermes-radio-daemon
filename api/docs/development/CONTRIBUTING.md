# Contributing to hermes-documentation

How to write commits and pull requests in this repository.

This repo is **documentation only**: manuals, guides, presentations, hardware
files, and the `tech_docs/api-redesign/` design specs. There is no runtime
code. The same discipline applies to the specs (`api-rest.yaml`,
`api-ws.yaml`) as to prose: a commit that adds an endpoint is a `feat`; a
commit that rewrites a paragraph is a `docs`.

## Commits

Use [Conventional Commits](https://www.conventionalcommits.org/):

```
type(scope): summary
```

- `summary` is imperative, lowercase, no trailing period, at most 72
  characters.
- A body is optional; use it for the *why* when the summary doesn't say it.

### Types

| Type | Use |
| --- | --- |
| `docs` | Prose and documentation changes (the default for this repo). |
| `feat` | A new piece of the **specs** — a new endpoint, schema, or document. |
| `fix` | Correcting an error in a document or spec. |
| `chore` | Tooling, CI, housekeeping. |
| `refactor` | Restructuring without changing meaning. |

`feat` and `fix` still apply here because the specs are the "product": an
`api-rest.yaml` endpoint is a feature.

### Scopes

Use the top-level area you changed:

| Scope | Area |
| --- | --- |
| `api` | `api-rest.yaml` / `api-ws.yaml` (the REST + WebSocket specs) |
| `api-redesign` | the redesign docs as a whole: `COMPARISON.md`, `poc/`, `broadcast.md`, `reticulum.md`, `hermes-backend/` |
| `manual` | `manual/` |
| `guide` | `guide/` |
| `presentation` | `presentation/` |
| `tech_docs` | other files under `tech_docs/` |
| `tools` | `tools/` |
| `gerbers` | `gerbers/` |

Omit the scope only for repo-wide files (`README.md`, `CONTRIBUTING.md`,
`.gitignore`, `LICENSE`).

### Examples

Good:

- `docs(api): add GPS trip breadcrumb endpoints to API redesign`
- `feat(api): add device registration and message reactions endpoints`
- `docs(api-redesign): update comparison and poc docs`
- `docs(manual): clarify the antenna troubleshooting steps`

Avoid:

- `api-redesign: broadcast messages to NNCP areas` — missing the type;
  write `docs(api-redesign): …`.
- `added manual`, `finish page` — no type or scope, vague summary.
- `cline checkpoint session=…`, `index on <branch>: …` — tooling and stash
  artifacts; never commit these.

## Branches

- Branch off `main` with a short, descriptive name. Existing examples:
  `api-redesign`, `guide`, `sbitx_bias`. For new work prefer
  `<type>/<scope>-<short-desc>` (for example `feat/api-gps-trip`,
  `docs/manual-troubleshooting`).
- Keep one topic per branch and keep `main` deployable.

## Pull requests

### Title

Use the same form as a commit subject — `type(scope): summary` — because it
becomes the merge commit message.

### Description

Fill it in; an empty description is not acceptable. Use this structure:

```markdown
## What
<what changed, and which files/areas>

## Decisions (if any)
<which COMPARISON.md register rows (D-xx / R-xx) this touches, or "none">

## Notes
<contracts only, nothing implemented; anything deliberately deferred>
```

### Rules

- One concern per PR. Reconciliation work and new spec surface are separate
  PRs.
- Reference the decision register (`tech_docs/api-redesign/COMPARISON.md`)
  when a change reflects or resolves a D-xx / R-xx row.
- Ask for review and address feedback before merging.

### Merge

- **Squash-merge** to `main`; the PR title is the squashed commit message.
- Delete the branch after merge.

> The history currently contains merge commits (`Merge pull request #N`);
> going forward prefer squash so each merged change lands as one
> `type(scope): summary`.

## Checklist

- [ ] Commit subjects are `type(scope): summary`.
- [ ] PR title matches the squashed commit message.
- [ ] PR description filled (What / Decisions / Notes).
- [ ] No `cline checkpoint` or stash (`index on …`) commits.
- [ ] One concern per PR.
