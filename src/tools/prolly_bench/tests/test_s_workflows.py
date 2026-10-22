"""Regressions for the S suite's workflow logic: which pull requests run it,
what the nightly compares with, and the comment workflow's table.

The steps' scripts are taken from the workflows and run against small git
repositories standing in for the checkout and origin.

Run: cd src/tools/prolly_bench && python3 -m unittest discover -s tests
"""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from pbench import sreport
from test_ci_report_review import REPO, step_script

GIT_ENV = {
    'GIT_AUTHOR_NAME': 't', 'GIT_AUTHOR_EMAIL': 't@example.com',
    'GIT_COMMITTER_NAME': 't', 'GIT_COMMITTER_EMAIL': 't@example.com',
    'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1',
}


def outputs(path):
    return dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)


class Repos:
    def __init__(self, root):
        self.root = root
        self.env = dict(os.environ, **GIT_ENV)

    def git(self, cwd, *args):
        return subprocess.run(['git', *args], cwd=cwd, env=self.env, check=True,
                              capture_output=True, text=True).stdout.strip()

    def commit(self, repo, path, text):
        (repo / path).parent.mkdir(parents=True, exist_ok=True)
        (repo / path).write_text(text)
        self.git(repo, 'add', path)
        self.git(repo, 'commit', '-q', '-m', path + ' ' + text)
        return self.git(repo, 'rev-parse', 'HEAD')


class NightlyBaseTest(unittest.TestCase):
    """The nightly's base: the last clean night, or master's previous commit."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.repos = Repos(self.root)
        self.origin = self.root / 'origin'
        self.origin.mkdir()
        self.repos.git(self.origin, 'init', '-q', '-b', 'master')
        # (GitHub serves any commit by its SHA)
        self.repos.git(self.origin, 'config', 'uploadpack.allowAnySHA1InWant', 'true')
        self.first = self.repos.commit(self.origin, 'a', '1')
        self.previous = self.repos.commit(self.origin, 'a', '2')
        self.tonight = self.repos.commit(self.origin, 'a', '3')

    def history(self, clean):
        self.repos.git(self.origin, 'switch', '-q', '--orphan', 'perf-history')
        self.repos.commit(self.origin, 'LATEST', self.previous + '\n')
        if clean:
            self.repos.commit(self.origin, 'CLEAN', clean + '\n')
        self.repos.git(self.origin, 'switch', '-q', 'master')

    def run_step(self):
        work = self.root / 'work'
        self.repos.git(self.root, 'clone', '-q', '--depth', '1', '-b', 'master',
                       'file://' + str(self.origin), str(work))
        out = self.root / 'outputs'
        env = dict(self.repos.env, GITHUB_OUTPUT=str(out), ImageOS='ubuntu24', ImageVersion='1')
        result = subprocess.run(['bash', '-e', '-c', step_script('performance.yml', 'Tonight and the last clean night')],
                                cwd=work, env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        got = outputs(out)
        self.assertEqual(got['candidate'], self.tonight)
        return got['base'], work

    def test_first_night_compares_with_masters_previous_commit(self):
        base, work = self.run_step()
        self.assertEqual(base, self.previous)
        # (the base's build checks it out)
        self.repos.git(work, 'cat-file', '-e', base + '^{commit}')

    def test_history_without_a_clean_night_compares_with_masters_previous_commit(self):
        self.history(clean=None)
        self.assertEqual(self.run_step()[0], self.previous)

    def test_last_clean_night_is_the_base(self):
        self.history(clean=self.first)
        self.assertEqual(self.run_step()[0], self.first)

    def test_nothing_new_since_the_last_clean_night(self):
        self.history(clean=self.tonight)
        self.assertEqual(self.run_step()[0], self.tonight)

    def test_unfetchable_clean_night_compares_with_masters_previous_commit(self):
        self.history(clean='0' * 40)
        self.assertEqual(self.run_step()[0], self.previous)


class PullRequestSwitchTest(unittest.TestCase):
    """A pull request runs the S suite with S_SUITE_PRS or the s-suite label."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.repos = Repos(self.root)

    def classify(self, changed='src/x.c', **env):
        repo = self.root / 'repo'
        if not repo.exists():
            repo.mkdir()
            self.repos.git(repo, 'init', '-q', '-b', 'master')
            self.repos.commit(repo, 'README', 'base')
            self.repos.commit(repo, changed, 'change')
        out = self.root / 'outputs'
        out.unlink(missing_ok=True)
        env = {**self.repos.env, 'GITHUB_OUTPUT': str(out), 'EVENT': 'pull_request', 'ACTION': 'opened',
               'ADDED_LABEL': '', 'LABELLED': 'false', 'S_SUITE_PRS': '', **env}
        result = subprocess.run(['bash', '-e', '-c', step_script('performance.yml', 'Select builds and suite enablement')],
                                cwd=repo, env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return outputs(out)['code']

    def test_off_without_the_variable_or_the_label(self):
        self.assertEqual(self.classify(), 'false')

    def test_variable_runs_it(self):
        self.assertEqual(self.classify(S_SUITE_PRS='true'), 'true')

    def test_label_runs_it(self):
        self.assertEqual(self.classify(LABELLED='true'), 'true')
        self.assertEqual(self.classify(LABELLED='true', ACTION='synchronize'), 'true')

    def test_adding_the_label_runs_it(self):
        self.assertEqual(self.classify(LABELLED='true', ACTION='labeled', ADDED_LABEL='s-suite'), 'true')

    def test_adding_another_label_does_not_run_it_again(self):
        self.assertEqual(self.classify(LABELLED='true', ACTION='labeled', ADDED_LABEL='docs'), 'false')

    def test_label_does_not_run_it_on_a_push(self):
        self.assertEqual(self.classify(EVENT='push', LABELLED='true'), 'false')

    def test_retired_specs_path_does_not_override_suite_enablement(self):
        self.assertEqual(self.classify(changed='specs/x.md', LABELLED='true'), 'true')

    def test_label_event_is_a_trigger(self):
        text = (REPO / '.github/workflows/performance.yml').read_text()
        trigger = text.split('\non:\n')[1].split('\n  push:')[0]
        self.assertIn('labeled', trigger.split('types:')[1].split('\n')[0])


class CommentWorkflowTest(unittest.TestCase):
    """The comment workflow posts the S suite's comment."""

    def which(self, workflow):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'outputs'
            env = dict(os.environ, WORKFLOW=workflow, GITHUB_OUTPUT=str(out))
            result = subprocess.run(['bash', '-e', '-c', step_script('correctness-comment.yml', 'Which comment')],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return outputs(out)

    def test_performance_comment_is_configured(self):
        got = self.which('performance')
        self.assertEqual(got['marker'], sreport.MARKER)
        performance = (REPO / '.github/workflows/performance.yml').read_text()
        self.assertIn('\nname: performance\n', performance)
        upload = performance.split('name: ' + got['artifact'] + '\n')[1].split('\n')[0]
        self.assertEqual(upload.strip(), 'path: comment/' + got['file'])

    def test_performance_is_followed(self):
        text = (REPO / '.github/workflows/correctness-comment.yml').read_text()
        workflows = text.split('workflows: [')[1].split(']')[0]
        self.assertIn('performance', [w.strip() for w in workflows.split(',')])

    def test_correctness_comment_is_unchanged(self):
        got = self.which('correctness')
        self.assertEqual((got['artifact'], got['file'], got['marker']), ('c-suite-comment', 'comment.md', '<!-- c-suite -->'))


if __name__ == '__main__':
    unittest.main()
