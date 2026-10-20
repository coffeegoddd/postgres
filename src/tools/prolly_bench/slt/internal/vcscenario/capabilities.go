package vcscenario

import (
	"fmt"
	"regexp"
	"sort"
	"strings"
)

// Capabilities a scenario can require. An engine claims the ones it
// supports; a scenario needing one it lacks is reported unsupported, never
// run. Each version-control capability names the dolt_* calls it covers, so
// a step's SQL shows what it needs.
var Capabilities = map[string]string{
	"sql":           "plain SQL",
	"vc.core":       "commit, branch, checkout, hard reset, log, status, hashof; connecting to a branch",
	"vc.merge":      "dolt_merge: fast-forward, --no-ff, --abort",
	"vc.conflicts":  "reading and resolving merge conflicts; dolt.allow_commit_conflicts",
	"vc.violations": "constraint violations left by merges; dolt_verify_constraints",
	"vc.cherrypick": "dolt_cherry_pick, with --abort and --continue",
	"vc.revert":     "dolt_revert",
	"vc.diff":       "dolt_diff, dolt_diff_summary, dolt_schema_diff",
	"vc.mergebase":  "dolt_merge_base",
	"vc.gc":         "dolt_gc",
	"vc.reflog":     "dolt_reflog",
}

// What each dolt_* name (function, view or setting) a step's SQL mentions
// needs. A name missing here is an error: a port can't call what no
// capability claims.
var callCapability = map[string]string{
	"dolt_commit":                 "vc.core",
	"dolt_branch":                 "vc.core",
	"dolt_checkout":               "vc.core",
	"dolt_reset":                  "vc.core",
	"dolt_add":                    "vc.core",
	"dolt_hashof":                 "vc.core",
	"dolt_log":                    "vc.core",
	"dolt_branches":               "vc.core",
	"dolt_status":                 "vc.core",
	"active_branch":               "vc.core",
	"dolt.author":                 "vc.core",
	"dolt_merge":                  "vc.merge",
	"dolt_conflicts":              "vc.conflicts",
	"dolt_conflicts_resolve":      "vc.conflicts",
	"dolt.allow_commit_conflicts": "vc.conflicts",
	"dolt_constraint_violations":  "vc.violations",
	"dolt_verify_constraints":     "vc.violations",
	"dolt_cherry_pick":            "vc.cherrypick",
	"dolt_revert":                 "vc.revert",
	"dolt_diff":                   "vc.diff",
	"dolt_diff_summary":           "vc.diff",
	"dolt_schema_diff":            "vc.diff",
	"dolt_merge_base":             "vc.mergebase",
	"dolt_gc":                     "vc.gc",
	"dolt_reflog":                 "vc.reflog",
}

// A typed operation: the capability it needs, the dolt_* function it calls
// (connect calls none: it opens the session's connection again), and
// whether it may save the commit it makes.
type opSpec struct {
	capability string
	function   string
	saves      bool
}

var ops = map[string]opSpec{
	"commit":     {"vc.core", "dolt_commit", true},
	"branch":     {"vc.core", "dolt_branch", false},
	"checkout":   {"vc.core", "dolt_checkout", false},
	"connect":    {"vc.core", "", false},
	"reset":      {"vc.core", "dolt_reset", false},
	"merge":      {"vc.merge", "dolt_merge", true},
	"cherrypick": {"vc.cherrypick", "dolt_cherry_pick", true},
	"revert":     {"vc.revert", "dolt_revert", true},
}

var callName = regexp.MustCompile(`(?i)(?:\b([a-z]+)\s+)?\b(dolt_[a-z0-9_]+|active_branch|dolt\.[a-z_]+)\b(\s*\()?`)

// Words before a name and a parenthesis that make it a table's, not a call
// (CREATE TABLE x (...), INSERT INTO x (...), REFERENCES x (...)).
var tableWords = map[string]bool{"table": true, "into": true, "references": true, "exists": true, "on": true}

// Text SQLRequires doesn't look in: string literals (standard, E” and
// dollar-quoted), quoted identifiers and comments.
var quoted = regexp.MustCompile(`(?s)'(?:[^']|'')*'|"(?:[^"]|"")*"|--[^\n]*|/\*.*?\*/|\$([a-zA-Z_]*)\$.*?\$([a-zA-Z_]*)\$`)

// SQLRequires is what a SQL text's dolt_* calls, views and settings need,
// sorted. A name a capability covers counts; another dolt_* name called
// as a function is an error (no capability claims it); one that isn't
// called is a table or column of the scenario's own, and doesn't count.
func SQLRequires(sql string) ([]string, error) {
	set := map[string]bool{}
	for _, m := range callName.FindAllStringSubmatch(quoted.ReplaceAllString(sql, " "), -1) {
		c, ok := callCapability[strings.ToLower(m[2])]
		if !ok && m[3] != "" && !tableWords[strings.ToLower(m[1])] {
			return nil, fmt.Errorf("no capability covers %s", m[2])
		}
		if ok {
			set[c] = true
		}
	}
	out := make([]string, 0, len(set))
	for c := range set {
		out = append(out, c)
	}
	sort.Strings(out)
	return out, nil
}

// stepRequires is what a step needs: its typed operation's capability, or
// what its SQL calls. A SQL step must list exactly that as its requires, so
// a reviewer sees it and a stale tag can't hide a call.
func stepRequires(st Step) ([]string, error) {
	if op, ok := ops[st.Op]; ok {
		if len(st.Requires) != 0 {
			return nil, fmt.Errorf("%s: a typed operation's requirement is implied", st.ID)
		}
		return []string{op.capability}, nil
	}
	need, err := SQLRequires(st.SQL)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", st.ID, err)
	}
	got := append([]string{}, st.Requires...)
	sort.Strings(got)
	if strings.Join(got, ",") != strings.Join(need, ",") {
		return nil, fmt.Errorf("%s: requires %v, but its SQL needs %v", st.ID, st.Requires, need)
	}
	return need, nil
}
