"""Compact pytest reporter: one line per test, short failure blocks.

Loaded from conftest.py. pytest's own terminal output is switched off in
pytest.ini (`-p no:terminal`); to get the stock output back run
    pytest -o addopts= -v
"""
import os
import sys
import time

import pytest

USE_COLOR = sys.stdout.isatty()
CODES = {'PASS': '30;42', 'FAIL': '37;41', 'ERROR': '37;41', 'SKIP': '30;43'}


def paint(text, code):
  return f'\x1b[{code}m{text}\x1b[0m' if USE_COLOR else text


def dim(text):
  return paint(text, '2')


def split_name(nodeid):
  """'tests/test_faults.py::test_x' -> ('faults', 'x')"""
  path, _, name = nodeid.partition('::')
  group = os.path.basename(path).removeprefix('test_').removesuffix('.py')
  return group, name.removeprefix('test_')


class CompactReporter:

  def __init__(self, config):
    self.config = config
    self.counts = {'PASS': 0, 'FAIL': 0, 'ERROR': 0, 'SKIP': 0}
    self.times = {}
    self.failures = []     # (nodeid, short_text)
    self.deselected = 0
    self.start = 0.0

  def emit(self, text=''):
    sys.stdout.write(text + '\n')
    sys.stdout.flush()

  # ----------------------------------------------------------- collection
  def pytest_deselected(self, items):
    self.deselected += len(items)

  def pytest_sessionstart(self, session):
    self.start = time.time()

  def pytest_collection_finish(self, session):
    extra = f', {self.deselected} deselected' if self.deselected else ''
    self.emit(dim(f'{len(session.items)} tests{extra}'))
    self.emit()

  def pytest_collectreport(self, report):
    if report.failed:
      self.emit(f'{paint(" ERROR ", CODES["ERROR"])} collecting '
                f'{report.nodeid}\n{report.longreprtext}')

  # --------------------------------------------------------------- results
  def pytest_runtest_logreport(self, report):
    self.times[report.nodeid] = (self.times.get(report.nodeid, 0.0)
                                 + (report.duration if report.when != 'teardown'
                                    else 0.0))
    status = None
    if report.when == 'setup' and report.failed:
      status = 'ERROR'
    elif report.when == 'setup' and report.skipped:
      status = 'SKIP'
    elif report.when == 'call':
      status = 'PASS' if report.passed else ('SKIP' if report.skipped else 'FAIL')
    elif report.when == 'teardown' and report.failed:
      status = 'ERROR'
    if status is None:
      return

    self.counts[status] += 1
    group, name = split_name(report.nodeid)
    secs = self.times[report.nodeid]
    self.emit(f'{paint(f" {status:<5} ", CODES[status])} {group:<7} '
              f'{name:<38} {dim(f"{secs:5.1f}s")}')
    if status in ('FAIL', 'ERROR'):
      self.failures.append((report.nodeid,
                            getattr(report, 'short_fail', None)
                            or report.longreprtext.strip().splitlines()[-1]))

  # --------------------------------------------------------------- summary
  def pytest_sessionfinish(self, session, exitstatus):
    if self.failures:
      self.emit()
      self.emit(paint('FAILURES', '1;31'))
      for nodeid, text in self.failures:
        group, name = split_name(nodeid)
        self.emit(f'\n {paint(f"{group}::{name}", "1")}')
        for line in text.splitlines():
          self.emit(f'   {line}')
    c = self.counts
    parts = [f'{c["PASS"]} passed']
    for key, label in (('FAIL', 'failed'), ('ERROR', 'errors'), ('SKIP', 'skipped')):
      if c[key]:
        parts.append(f'{c[key]} {label}')
    bad = c['FAIL'] or c['ERROR']
    line = f'{", ".join(parts)} in {time.time() - self.start:.1f}s'
    self.emit()
    self.emit(paint(line, '31' if bad else '32'))


def _short(excinfo):
  """Failure text: message + the test line that failed."""
  msg = str(excinfo.value).strip() or excinfo.typename
  where = ''
  for entry in reversed(excinfo.traceback):
    if os.sep + 'tests' + os.sep in str(entry.path):
      stmt = str(entry.statement).strip().splitlines()[0]
      where = f'at {os.path.basename(str(entry.path))}:{entry.lineno + 1}  {stmt}'
      break
  return f'{where}\n{msg}' if where else msg


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
  outcome = yield
  report = outcome.get_result()
  if call.excinfo is not None and report.failed:
    report.short_fail = _short(call.excinfo)


def pytest_configure(config):
  if not config.pluginmanager.hasplugin('terminalreporter'):
    config.pluginmanager.register(CompactReporter(config), 'compact-reporter')
