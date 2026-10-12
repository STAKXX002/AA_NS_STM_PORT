"""pytest fixtures for the Renode simulation tests."""
import os
import subprocess

import pytest

from harness import ROOT, SIM_ELF, Sim

pytest_plugins = ['reporter']   # compact one-line-per-test output


def pytest_addoption(parser):
  parser.addoption('--no-build', action='store_true',
                   help='skip rebuilding the Simulation preset before testing')


@pytest.fixture(scope='session', autouse=True)
def simulation_build(request):
  """The sim loads build/Simulation/*.elf (RENODE_BUILD=ON). Rebuild it first
  so the tests never run against a stale binary."""
  if request.config.getoption('--no-build'):
    return
  build_dir = os.path.join(ROOT, 'build', 'Simulation')
  steps = []
  if not os.path.isdir(build_dir):
    steps.append(['cmake', '--preset', 'Simulation'])
  steps.append(['cmake', '--build', build_dir])
  capman = request.config.pluginmanager.getplugin('capturemanager')
  with capman.global_and_fixture_disabled():
    print('building Simulation preset ...', flush=True)
    for cmd in steps:
      r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
      if r.returncode != 0:
        pytest.exit(f'{" ".join(cmd)} failed:\n'
                    f'{r.stdout[-2000:]}{r.stderr[-2000:]}', returncode=2)
  if not os.path.exists(SIM_ELF):
    pytest.exit(f'{SIM_ELF} missing after build', returncode=2)


@pytest.fixture
def sim(request):
  """A freshly booted simulator, shut down after the test."""
  s = Sim(request.node.name)
  try:
    s.start()
    yield s
  finally:
    s.cleanup()


@pytest.fixture
def calibrated(sim):
  sim.calibrate()
  return sim


@pytest.fixture
def faulted(sim):
  sim.fault()
  return sim
