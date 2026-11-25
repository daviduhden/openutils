# Agent Knowledge Validation Contract

Contract version: `1.0.0`
JSON schema version: `1.0`

All repositories must implement the same Agent Knowledge Validation CLI
contract, rule identifiers, output conventions and exit codes. Repository-
specific validations may extend the canonical behavior but must not silently
redefine it. Any change to the validator contract must be reflected in the
repository's agent instructions, related skills and tests.

## Run the validator

Requirements: Perl 5.14 or newer (JSON::PP core) and Git. The validator uses only core Perl modules. Run it from inside the repository worktree:

```sh
perl scripts/validate-agent-knowledge.pl --help
perl scripts/validate-agent-knowledge.pl --scope worktree
perl scripts/validate-agent-knowledge.pl \
  --scope range --base BASE_REF --head HEAD_REF
perl scripts/validate-agent-knowledge.pl \
  --scope range --base BASE_REF --head HEAD_REF --format json
prove tests/agent_knowledge_validator/test_validator.pl
```

Exit codes are consistent across repositories: `0` means no policy errors,
`1` means a policy violation, and `2` means invalid arguments, configuration,
Git state, or execution. Warnings do not change a successful exit code.

## Change discovery and evaluation

`worktree` takes the net tracked diff against `HEAD` and adds non-ignored
untracked paths. An unstaged edit can therefore cancel a staged change to
the same file in the net diff. `range` compares the merge base of the two
requested commits with the requested head. Both modes disable Git rename
detection, so a rename contributes a deletion and an addition.

The scope selects the source of changed paths and instruction diffs.
Configuration, instruction structure, and local references are evaluated
against the checkout in both modes. For a committed comparison, fetch enough
history to resolve the common ancestor and use a clean checkout of the
requested head, as the manual workflow does. Review changes to these
snapshot semantics upstream with the other adopting repositories.

The synchronization gate classifies the changed paths and measures effective
instruction edits. It trims and normalizes whitespace, skips headings and
timestamp-like lines, and cancels matching added and removed lines before
counting the remaining lines and alphanumeric characters. The structural
and reference checks discover instruction files independently of the changed
path list, so an unchanged instruction can still fail those checks.

The black-box suite copies the canonical script into temporary Git
repositories and invokes it as a subprocess. Assertions cover output,
exit status, and how the public CLI responds to fixture changes. The manual
workflow also validates its own checkout and committed range against the
actual repository instructions.

## Rules

| ID | Meaning |
| --- | --- |
| `AK001` | Project changes require a substantive update to maintained agent or skill knowledge. Changes limited to the validator, its contract tests, or its dedicated workflow are exempt to prevent recursive gate failures. |
| `AK002` | Recognized agent and skill files must be non-empty and meet their required headings and simple frontmatter field checks. Frontmatter inspection is partial; it is not full YAML validation. |
| `AK003` | Relative Markdown references in recognized instructions must resolve inside the repository. External URLs are not treated as local paths. |
| `AK004` | Cosmetic, timestamp-only, whitespace-only, or otherwise content-free instruction edits fail. Substantive updates produce a warning for human review; the validator cannot prove semantic relevance. |
| `AK005` | Invalid CLI configuration, Git revisions, worktree state, or execution are errors. |
| `AK006` | Exact duplicate instruction files and directly opposed modal directives are reported as warnings for review. This deterministic check does not infer general semantic contradictions. |
| `AKX001` | Optional configured repository-specific validation, including strict text rules, required skill references, inline path checks, and extra change thresholds. |

Every rule result uses `PASS`, `FAIL`, or `WARN`, a stable `rule_id`, a message,
relative file paths when applicable, and a remediation hint when applicable.

## Output formats

Text output emits one `[STATUS] RULE_ID - message` line per rule, followed by
an `Agent Knowledge Validation` summary with status, error count, and warning
count. Files and remediation are indented below the relevant rule.

JSON output is one UTF-8 JSON object on stdout with this stable top-level
shape:

```json
{
  "schema_version": "1.0",
  "contract_version": "1.0.0",
  "status": "PASS",
  "summary": {"errors": 0, "warnings": 1},
  "checks": [],
  "errors": [],
  "warnings": []
}
```

Each check item has `rule_id`, `status`, `message`, `files`, and
`remediation`. Empty file lists and absent remediation are represented as an
empty list and `null`. The `errors` and `warnings` arrays contain the matching
check objects. Execution details are written to stderr when JSON output is
selected; stdout remains valid JSON. Additive schema changes require a schema
version update and compatibility review.

## Optional repository configuration

`.agent-knowledge-validator.toml` is optional. The only supported root table is
`[validator]`, and `contract_version` must equal `1.0.0`. Supported settings:

- `exclude_paths`: repository-relative globs excluded from the AK001 change
  gate. Document each generated or user-state exclusion in `AGENTS.md`.
- `instruction_globs`: additional repository-relative instruction file
  patterns.
- `required_headings` and `required_phrases`: tables keyed by quoted
  repository-relative instruction paths; these add AK002 requirements.
- `strict_text`: path-glob tables containing `ascii_only`, `lf_only`,
  `final_newline`, `no_trailing_whitespace`, or `no_nul` rules.
- `skill_reference_prefixes`: backtick-delimited skill names that specialized
  agents must resolve to discovered skills.
- `inline_path_checks`: validate recognized repository paths in inline code.
- `minimum_changed_lines`: minimum effective instruction lines for AK001,
  from 1 to 1000; the shared minimum of 24 alphanumeric changed characters
  still applies.

Configuration cannot execute commands, define arbitrary code, or disable
AK002, AK003, AK004, AK005, or AK006. Unknown keys, absolute paths, parent
traversals, and incompatible contract versions fail with exit code `2`.

## Extending and replicating the contract

Keep `scripts/validate-agent-knowledge.pl` byte-identical across adopting
repositories. The standard black-box suite at
`tests/agent_knowledge_validator/test_validator.pl` must also remain
equivalent and pass with `prove`. Repository-specific config is the only
normal way to add deterministic constraints. If a distinct extension needs
code, give it a stable identifier outside `AK001`–`AK006`, document why it
cannot be expressed by the shared configuration, and add equivalent tests.

Before changing the contract, update this document, `AGENTS.md`, related
skills, tests, and each adopted repository's CI invocation. Run the test suite
and both documented CLI modes. Never auto-edit instruction files to satisfy
the gate.
