"""One long end-to-end walk-through (the old test_simulation.py). Run before
releases: pytest -m slow tests/test_e2e.py"""
import pytest


@pytest.mark.slow
def test_full_sequence(sim):
  sim.calibrate()

  sim.uart('GO')
  sim.expect('HOLD', timeout=60.0)

  sim.uart('RET')
  sim.expect('RETURNED', timeout=90.0)

  sim.uart('OPEN')
  sim.expect('OPENED', timeout=60.0)

  sim.uart('CLOSE')
  sim.expect('CLOSED', timeout=60.0)

  sim.uart('ON')
  sim.expect('LIGHT & FAN ON')
  sim.uart('OFF')
  sim.expect('LIGHT OFF, FAN TIMER STARTED')
