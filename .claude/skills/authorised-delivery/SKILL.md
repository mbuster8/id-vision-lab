---
name: authorised-delivery
description: Take an ID Vision Lab item from a written spec to a green pull request without the owner — deep Opus review of the spec and fixes, a plan with a deep Opus review and fixes, inline test-first execution, a final Opus review of the branch and fixes, local CI and commits, then push, a pull request assigned to the owner, and watching CI and fixing it until every job passes, ending with test instructions for what could not be checked here. Use this whenever the owner invokes it, or asks in their own words for the whole run on their behalf: "go and be self-sufficient", "I'm going off the Mac", "do it all without me", "review it with Opus, fix it, implement, commit, push and watch CI until green", "same authorised plan as last time".
---

# Authorised delivery

The owner hands over an item and leaves. They come back to a branch that is implemented, tested, reviewed,
committed, pushed, open as a pull request assigned to them, with every CI job green, and with instructions for the
few things only they can check. What they must not come back to: a question that waited all night, a red pipeline,
or a claim nobody verified.

## What invoking this authorises, and what it does not

Invoking the skill, or the owner asking for this run in the request, is their approval **for this item, on its
feature branch** of what `CLAUDE.md` and `CLAUDE.local.md` otherwise ask about:

- commits on the feature branch, in the repository's commit style, ending with the attribution lines the harness
  gives;
- pushing that branch, opening one pull request against `master` assigned to the owner
  (`gh pr create --assignee "@me"`), and pushing fixes to it until CI is green;
- three read-only reviewers on **Opus**: a deep review of the spec, a deep review of the plan, a final review of
  the branch. Each appends its findings to a file in the job's tmp folder as it finds them, so a stopped reviewer
  still leaves its work;
- installing standard development tooling the item needs (a language's test runner or formatter, the libraries the
  code itself imports), into a project-local, git-ignored environment such as a venv. Say what was installed.

Still not authorised, so stop and ask, or leave it undone and report it: merging, pushing to `master`, force-pushing,
deleting branches, closing the pull request, rewriting history that was pushed, paid services, installs beyond
development tooling, anything outside this repository, and anything irreversible or security-sensitive. A
question the owner must answer goes in the final report with a recommendation, not into a dialog nobody is there
to answer.

## Before starting

- A spec exists for the item (from `superpowers:brainstorming`, in `docs/superpowers/specs/`), or the owner's
  request is complete enough to write one. If it is not, this is not the skill yet: ask the owner's open decisions
  first, while they are still there.
- Note the decisions the owner already made. Everything after that is your call: take the reviewer's or your own
  recommendation, write it down as **(my call)** in the spec, the plan or the ledger, and carry on.
- Long commands run in the background with `caffeinate -s -i` (the Mac sleeps otherwise). Docker Desktop must be
  running for `scripts/check.sh`'s Ubuntu job; if it is not, say so in the report and run the rest.

## The run

1. **Spec review and fixes.** If no deep review of the spec has been done, run one on Opus: findings graded
   CRITICAL, HIGH, IMPORTANT, MINOR, with the cheap ones tagged as quick wins, checked against the real code.
   Fix every critical, high and important finding, and the quick wins among the rest, in the spec itself. Check a
   reviewer's factual claims you depend on (a library's behaviour, an API) before building on them.

2. **Plan, review, fixes.** Write the plan with `superpowers:writing-plans`: exact files, signatures, tests and
   commands, a Review Focus section, and no commit steps beyond those this skill allows. Run a deep Opus review of
   the plan: recomputing every expected value in its tests, checking interfaces against the real headers,
   threading and portability. Fix the critical, high and important findings, and the quick wins, in the plan.

3. **Inline execution.** `superpowers:executing-plans` with `superpowers:test-driven-development`: each test seen
   failing for the right reason before the code that makes it pass; a ledger line per task; every deviation
   recorded as a ruling. Commit as tasks complete, so the history tells the story.

4. **Final review and fixes.** One fresh Opus reviewer over the whole branch, with the plan's Review Focus. Re-grade
   its findings by their effect on someone using the software. Fix every critical, high and important one, and the
   quick wins, each with a test that fails first. A finding not fixed is a ruling, with its cost if wrong; minors
   left are listed in the report.

5. **Local CI and commit.** `scripts/check.sh` in full (the `local-ci` skill) on the committed tree; fix and rerun
   until `check.sh: all steps passed.` Then the pull request description with the `pr-ready` skill's template.

6. **Push, pull request, CI until green.** Push the branch, open the pull request assigned to the owner, and watch
   its checks (`gh pr checks <n> --watch`). On a failure:
   - read the failing job's log (`gh run view --job <id> --log-failed`) and find the first real error;
   - reproduce it locally where possible (`scripts/check.sh --linux` for the Ubuntu job);
   - fix it with a test that fails first when the failure is a bug, not just the pipeline;
   - commit, push, and watch again.
   Keep going until every job passes. If the same job fails three times for reasons you cannot reproduce or fix,
   stop pushing and report what was tried. A job that passes but logs warnings the project forbids (MSVC `/W4`,
   for example) is not green: read the Windows log for `warning C`.

7. **Test instructions.** Only after CI is green: what the owner should check by hand, the things the automated
   tests and this machine cannot show (macOS drawing, appearance changes, real cameras and videos, other machines,
   rates in the running app). Number the steps and give each its expected result. Run every command you hand over,
   or at least its argument parsing (`--help`, a dry run against an unused port), before giving it: a command
   that turns out wrong costs the owner the morning this skill was meant to save.

## Rules that hold throughout

- Measurements come from this session's runs, with what was measured and how; never an estimate presented as
  one. No test counts or timings in Markdown files; the pull request and the report may carry them.
- Tests never use port 7139, the owner's running viewer. A real viewer for a check runs on another port with its
  own home (`CFFIXED_USER_HOME` in the job's tmp folder) and is stopped afterwards.
- Narrate briefly as you go; the owner reads the transcript later. Long command output goes to a file in the job's
  tmp folder, and only the lines that matter come back.

## The report

When CI is green, end with:

- the pull request's link and branch, the commits, and that every CI job passed (Windows warnings checked);
- what was built, in a few lines, and the measurements with their numbers;
- the reviews: how many findings, what was fixed, and the minors left;
- every ruling made on the owner's behalf, each with its cost if wrong;
- anything installed;
- the test instructions;
- decisions only the owner can make, each with options and a recommendation;
- a `result:` line.
