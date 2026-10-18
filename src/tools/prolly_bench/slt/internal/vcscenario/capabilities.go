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
	"vc.core":       "commit, branch, checkout, hard reset, log, status, hashof",
	"vc.merge":      "dolt_merge: fast-forward, --no-ff, --abort",
	"vc.conflicts":  "reading and resolving merge conflicts; dolt.allow_commit_conflicts",
	"vc.violations": "constraint violations left by merges; dolt_verify_constraints",
	"vc.cherrypick": "dolt_cherry_pick, with --abort and --continue",
	"vc.revert":     "dolt_revert",
	"vc.diff":       "dolt_diff",
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
}

// What each typed operation needs.
var opCapability = map[string]string{
	"commit":     "vc.core",
	"branch":     "vc.core",
	"checkout":   "vc.core",
	"reset":      "vc.core",
	"merge":      "vc.merge",
	"cherrypick": "vc.cherrypick",
	"revert":     "vc.revert",
}

var callName = regexp.MustCompile(`(?i)\b(dolt_[a-z0-9_]+|active_branch|dolt\.[a-z_]+)\b`)

// SQLRequires is what a SQL text's dolt_* calls need, sorted.
func SQLRequires(sql string) ([]string, error) {
	set := map[string]bool{}
	for _, m := range callName.FindAllString(sql, -1) {
		c, ok := callCapability[strings.ToLower(m)]
		if !ok {
			return nil, fmt.Errorf("no capability covers %s", m)
		}
		set[c] = true
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
	if c, ok := opCapability[st.Op]; ok {
		if len(st.Requires) != 0 {
			return nil, fmt.Errorf("%s: a typed operation's requirement is implied", st.ID)
		}
		return []string{c}, nil
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
