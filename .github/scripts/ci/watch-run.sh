#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

# Wait for the Actions runs of one commit and report what they did.
#
# The bare-metal legs of this repository run on NIC-owning hosts whose
# availability comes in windows, so the interval between pushing a change and
# learning whether it was right is measured in hours, and most of that interval
# looks identical in the Actions UI: a job that is queued because the fleet is
# busy and a job that is queued because no host advertises its label are the same
# grey dot. This polls the API instead and says which one it is, then turns the
# finished run into the three facts a fix needs -- which job, which step, and the
# error line -- rather than a 40 MB log to download by hand.
#
# It only reads. Nothing here pushes, re-runs, cancels or comments.
#
# Usage:
#   watch-run.sh                          # every run of this branch's head commit
#   watch-run.sh --pr 1682                # every run of a pull request's head
#   watch-run.sh --run 32441346791        # one run
#   watch-run.sh --workflow smoke-tests-bare-metal --job i225
#
# Exit: 0 every job passed, 1 a job failed, 2 the timeout expired with work
# still outstanding, 3 nothing to watch.

repo=${MTL_CI_REPO:-}
run_id=''
pr=''
sha=''
branch=''
workflow_filter=''
job_filter=''
timeout_min=${MTL_CI_WATCH_TIMEOUT:-120}
interval_s=${MTL_CI_WATCH_INTERVAL:-30}
log_lines=12
quiet=false

die() {
	echo "$*" >&2
	exit 3
}

usage() {
	sed -n '/^# Usage:/,/^# still outstanding/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--run) run_id=${2:?--run needs a run id} && shift 2 ;;
	--pr) pr=${2:?--pr needs a number} && shift 2 ;;
	--sha) sha=${2:?--sha needs a commit} && shift 2 ;;
	--branch) branch=${2:?--branch needs a ref} && shift 2 ;;
	--workflow) workflow_filter=${2:?--workflow needs a name} && shift 2 ;;
	--job) job_filter=${2:?--job needs a pattern} && shift 2 ;;
	--timeout) timeout_min=${2:?--timeout needs minutes} && shift 2 ;;
	--interval) interval_s=${2:?--interval needs seconds} && shift 2 ;;
	--log-lines) log_lines=${2:?--log-lines needs a count} && shift 2 ;;
	--repo) repo=${2:?--repo needs owner/repo} && shift 2 ;;
	--quiet) quiet=true && shift ;;
	-h | --help)
		usage
		exit 0
		;;
	*) die "Unknown argument: $1$(printf '\n')$(usage)" ;;
	esac
done

command -v gh >/dev/null 2>&1 || die "gh is not installed; see https://cli.github.com/"
command -v jq >/dev/null 2>&1 || die "jq is not installed: sudo apt-get install -y jq"

if [[ -z ${repo} ]]; then
	repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null) ||
		die "Cannot determine the repository; pass --repo owner/repo."
fi

api() { gh api -H 'Accept: application/vnd.github+json' "$@"; }

# A conclusion that means the job did not do its job. `cancelled` is in here on
# purpose: a leg cancelled by its own concurrency group is not a result, and the
# summary says which of the two it was.
is_failure() {
	case "$1" in
	failure | timed_out | startup_failure | action_required | cancelled) return 0 ;;
	*) return 1 ;;
	esac
}

resolve_sha() {
	if [[ -n ${sha} ]]; then
		echo "${sha}"
	elif [[ -n ${pr} ]]; then
		api "repos/${repo}/pulls/${pr}" --jq .head.sha
	else
		local ref=${branch:-$(git rev-parse --abbrev-ref HEAD)}
		# The remote's idea of the branch, not the local one: a run exists for
		# what was pushed, and reporting on an unpushed commit as if CI had seen
		# it is the failure mode this whole script exists to remove.
		git ls-remote origin "refs/heads/${ref}" | cut -f1 |
			grep . || die "No pushed ref refs/heads/${ref} on origin; push it first."
	fi
}

# Filter stdin through grep when a filter was given, pass it through when not.
# Written out rather than as `[[ -n $1 ]] && grep ... || cat` because that form
# also runs cat when grep matches nothing, and a filter that matches nothing is
# an empty result here, not an unfiltered one.
maybe_grep() {
	if [[ -n ${1} ]]; then
		grep -- "${1}" || true
	else
		cat
	fi
}

# The field separator of the lines below. Not a tab: a tab is IFS whitespace, so
# `read` collapses a run of them, and every line here has fields that are empty
# exactly when they matter -- a run with no conclusion is still running, a job
# with no runner_name was never handed to a host. Collapsing them shifts every
# later field left, which silently reported started_at as the runner name.
sep=$'\x1f'

# One line per run: id, workflow name, status, conclusion, url.
fetch_runs() {
	if [[ -n ${run_id} ]]; then
		api "repos/${repo}/actions/runs/${run_id}" \
			--jq '[.id, .name, .status, (.conclusion // ""), .html_url] | map(tostring) | join("\u001f")'
	else
		api "repos/${repo}/actions/runs?head_sha=${head_sha}&per_page=100" \
			--jq '.workflow_runs[] | [.id, .name, .status, (.conclusion // ""), .html_url] | map(tostring) | join("\u001f")'
	fi | maybe_grep "${workflow_filter}"
}

# One line per job of one run: id, name, status, conclusion, runner, started.
fetch_jobs() {
	api "repos/${repo}/actions/runs/${1}/jobs?per_page=100" --paginate \
		--jq '.jobs[] | [.id, .name, .status, (.conclusion // ""), (.runner_name // ""), (.started_at // "")] | map(tostring) | join("\u001f")' |
		maybe_grep "${job_filter}"
}

minutes_since() {
	# minutes_since <iso8601> -- how long a job has been in its current state.
	local started=$1 epoch
	[[ -n ${started} ]] || {
		echo 0
		return
	}
	epoch=$(date -u -d "${started}" +%s 2>/dev/null) || {
		echo 0
		return
	}
	echo $(((now_epoch - epoch) / 60))
}

say() { [[ ${quiet} == true ]] || printf '%s\n' "$*"; }

# The failing step and the error lines of one finished job, from the job log.
# Bounded on purpose: the point is the three lines that name the cause.
job_diagnosis() {
	local job_id=$1 step log region last=5
	step=$(api "repos/${repo}/actions/jobs/${job_id}" \
		--jq '[.steps[]? | select(.conclusion == "failure") | .name] | first // ""')
	[[ -n ${step} ]] && printf '         failed step: %s\n' "${step}"
	log=$(api "repos/${repo}/actions/jobs/${job_id}/logs" 2>/dev/null) || return 0
	# Drop the timestamp each log line carries and the colour the tasks emit, and
	# stop at the line the runner prints when the step failed. Stopping there is
	# what makes the excerpt readable: everything after it is the post-job
	# cleanup -- twenty `git config --unset` lines that match "unset" and would
	# otherwise be the whole excerpt.
	#
	# Then drop what the runner says rather than what the job said: the `Run ...`
	# preamble with its two dozen lines of exported environment, the group
	# markers, the echo Task prints before every command (`task: [ci:x] bash ...`,
	# which matches any pattern the command it runs would), the checkout's fetch
	# output and its detached-HEAD warning. Each of those appears in every job,
	# including the ones that pass, so none of it can be the reason one failed.
	region=$(printf '%s\n' "${log}" |
		sed -e 's/^[0-9-]\{10\}T[0-9:.]\{8,\}Z //' -e 's/\x1b\[[0-9;]*m//g' \
			-e '/^##\[error\]Process completed with exit code/q' \
			-e '/^##\[group\]Run /,/^##\[endgroup\]$/d' |
		grep -viE '^[[:space:]]*$|^(task: \[|\[command\]|##\[(group|endgroup)\]|[[:space:]]*\* \[|Warning: you are leaving)|^##\[error\](exit status|Process completed)') || true
	# The last few lines of that are always shown, and earlier ones only when
	# they name something. A step fails at its end, so its closing lines are the
	# diagnosis -- and they are the part no keyword list can be trusted with. The
	# scripts in this repository fail by naming the command that fixes the host,
	# in whatever words fit ("Permission denied (publickey)", "does not work"),
	# and twice now a keyword grep has thrown exactly those lines away and printed
	# `task: Failed` on its own. Keywords now only reach further back, into a long
	# test log, for the earlier line that says what went wrong first.
	#
	# `[error]` unbracketed by ## on purpose: it catches the runner's own
	# ##[error] and a tool that prints [ERROR] itself, which super-linter does and
	# which is the only line naming what it found.
	{
		printf '%s\n' "${region}" | head -n "-${last}" |
			grep -iE '\[error\]|error:|^task: Failed|cannot|missing|unset|no such|not (found|available|installed|connected)' || true
		printf '%s\n' "${region}" | tail -n "${last}"
	} | tail -n "${log_lines}" | sed 's/^/         /'
	return 0
}

head_sha=''
if [[ -n ${run_id} ]]; then
	head_sha=$(api "repos/${repo}/actions/runs/${run_id}" --jq .head_sha) ||
		die "No run ${run_id} in ${repo}."
else
	head_sha=$(resolve_sha)
fi
[[ -n ${head_sha} ]] || die "Could not resolve a commit to watch."

now_epoch=$(date -u +%s)
deadline=$((now_epoch + timeout_min * 60))
started_epoch=${now_epoch}

say "Watching ${repo} @ ${head_sha:0:8}${run_id:+ (run ${run_id})}, timeout ${timeout_min}m."

declare -A run_state=() job_state=() job_runner=() run_name=() run_url=()
declare -A run_status=() run_conclusion=() job_name_of=() job_conclusion=() job_run=()
timed_out=false

while :; do
	now_epoch=$(date -u +%s)
	pending=0
	queued_report=''
	runs=$(fetch_runs) || die "Cannot read runs of ${head_sha:0:8} in ${repo}."
	[[ -n ${runs} ]] || die "No runs for ${head_sha:0:8}${workflow_filter:+ matching ${workflow_filter}}."

	while IFS=${sep} read -r id name status conclusion url; do
		[[ -n ${id} ]] || continue
		run_name[${id}]=${name}
		run_url[${id}]=${url}
		run_status[${id}]=${status}
		run_conclusion[${id}]=${conclusion}
		if [[ ${run_state[${id}]:-} != "${status}:${conclusion}" ]]; then
			run_state[${id}]="${status}:${conclusion}"
			say "$(date -u +%H:%M:%S)  ${name}: run ${status}${conclusion:+ (${conclusion})}"
		fi
		[[ ${status} == completed ]] || pending=1

		while IFS=${sep} read -r job_id job_name job_status job_conc runner started; do
			[[ -n ${job_id} ]] || continue
			job_name_of[${job_id}]=${job_name}
			job_conclusion[${job_id}]=${job_conc}
			job_run[${job_id}]=${id}
			job_runner[${job_id}]=${runner}
			if [[ ${job_state[${job_id}]:-} != "${job_status}:${job_conc}" ]]; then
				job_state[${job_id}]="${job_status}:${job_conc}"
				say "$(date -u +%H:%M:%S)    ${job_name} -> ${job_status}${job_conc:+ (${job_conc})}${runner:+ on ${runner}}"
			fi
			# A queued job with no runner name has not been handed to a host.
			# That is the fleet-availability case, and the only way to tell it
			# from a job that is running slowly.
			if [[ ${job_status} == queued ]]; then
				queued_report+="      ${job_name}: queued $(minutes_since "${started}")m, no runner yet"$'\n'
			fi
		done < <(fetch_jobs "${id}")
	done <<<"${runs}"

	[[ ${pending} -eq 1 ]] || break
	if [[ -n ${queued_report} ]]; then
		say "    still waiting after $(((now_epoch - started_epoch) / 60))m:"
		say "${queued_report%$'\n'}"
	fi
	if [[ $(date -u +%s) -ge ${deadline} ]]; then
		timed_out=true
		break
	fi
	sleep "${interval_s}"
done

# ---------------------------------------------------------------------------
# Summary. This is the whole output when --quiet, so it carries the verdict.
failed=0
total=0
skipped=0
echo "=== watch-run summary ==="
# What was asked for, beside the commit it resolved to: the branch if one was
# named, else the pull request, else the pushed tip. Spelled out rather than
# nested in one parameter expansion, which printed the number twice.
asked_for=HEAD
[[ -z ${pr} ]] || asked_for="PR ${pr}"
[[ -z ${branch} ]] || asked_for=${branch}
printf '%-9s %s (%s)\n' commit "${head_sha:0:8}" "${asked_for}"
for id in $(printf '%s\n' "${!run_name[@]}" | sort); do
	printf '%-9s %s: %s%s\n' run "${run_name[${id}]}" \
		"${run_status[${id}]}" \
		"${run_conclusion[${id}]:+ (${run_conclusion[${id}]})}"
	printf '%-9s %s\n' '' "${run_url[${id}]}"
	for job_id in $(printf '%s\n' "${!job_run[@]}" | sort); do
		[[ ${job_run[${job_id}]} == "${id}" ]] || continue
		conc=${job_conclusion[${job_id}]}
		if [[ -z ${conc} ]]; then
			mark=....
		elif is_failure "${conc}"; then
			mark=FAIL
		elif [[ ${conc} == skipped ]]; then
			# A gated leg that never ran. It is not a failure -- the path filters
			# skip most of this repository's jobs on most commits -- but reporting
			# it as `ok` is how a green summary comes to mean "the leg you were
			# waiting for did not happen", which is the misreading this whole
			# script exists to remove. Counted apart from the verdict.
			mark=skip
			skipped=$((skipped + 1))
		else
			mark=ok
		fi
		[[ ${mark} == skip ]] || total=$((total + 1))
		printf '  %-6s %s%s\n' "${mark}" "${job_name_of[${job_id}]}" \
			"${job_runner[${job_id}]:+  [${job_runner[${job_id}]}]}"
		if is_failure "${conc}"; then
			failed=$((failed + 1))
			if [[ ${conc} == cancelled ]]; then
				echo "         cancelled${job_runner[${job_id}]:+ on ${job_runner[${job_id}]}}: no result, not a failure of the change"
			else
				job_diagnosis "${job_id}"
			fi
		fi
	done
done

if [[ ${timed_out} == true ]]; then
	echo "result: TIMEOUT after ${timeout_min}m with work outstanding"
	exit 2
fi
skipped_note=''
[[ ${skipped} -eq 0 ]] || skipped_note=", ${skipped} skipped"
if [[ ${failed} -gt 0 ]]; then
	echo "result: FAIL (${failed} of ${total} jobs${skipped_note})"
	exit 1
fi
echo "result: PASS (${total} jobs${skipped_note})"
