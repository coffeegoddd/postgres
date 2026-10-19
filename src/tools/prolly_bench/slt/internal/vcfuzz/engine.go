package vcfuzz

// Outcome classes: what an op did, as every engine reports it.
const (
	ClassOK          = "ok"
	ClassConflict    = "conflict"     // the merge, pick or revert conflicted
	ClassUpToDate    = "up_to_date"   // nothing to merge
	ClassFastForward = "fast_forward" // the branch moved to theirs
	ClassUnchanged   = "unchanged"    // a commit, pick or revert with no change
)

// Outcome is what an op did.  Conflicts counts the conflicted rows a
// merge in a transaction block found; other conflicts aren't counted.
type Outcome struct {
	Class     string `json:"class"`
	Conflicts int    `json:"conflicts,omitempty"`
}

// Engine runs a history's ops in sessions and reports what each session
// sees.  An error is the harness's, never an outcome: an op an engine
// refuses unexpectedly is an Outcome of its own class.
type Engine interface {
	Apply(op Op) (Outcome, error)
	// Visible is the rows session s sees.
	Visible(s int) (State, error)
	// Branch is session s's branch.
	Branch(s int) (string, error)
	// Dirty is whether session s's branch has uncommitted changes.
	Dirty(s int) (bool, error)
	Close()
}

// Identities is implemented by engines whose commits have ids and
// canonical root hashes, for checking history independence.
type Identities interface {
	// Heads is each branch's commit id.
	Heads() (map[string]string, error)
	// RootHash is the canonical hash of a commit's contents.
	RootHash(commit string) (string, error)
}

// Opener opens an engine at a history's setup: Tables holding the setup
// rows on main, committed, and the history's sessions on main.
type Opener func(h History) (Engine, error)
