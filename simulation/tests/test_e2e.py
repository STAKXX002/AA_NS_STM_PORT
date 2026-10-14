"""One long end-to-end walk-through (the old test_simulation.py). Run before
releases: pytest -m slow tests/test_e2e.py"""
import pytest

from harness import T_GO, T_HATCH, T_RET


@pytest.mark.slow
def test_full_sequence(sim):
  sim.calibrate()

  sim.uart('GO')
  sim.expect('HOLD', timeout=T_GO)

  sim.uart('RET')
  sim.expect('RETURNED', timeout=T_RET)

  sim.uart('OPEN')
  sim.expect('OPENED', timeout=T_HATCH)

  sim.uart('CLOSE')
  sim.expect('CLOSED', timeout=T_HATCH)

  sim.uart('ON')
  sim.expect('LIGHT & FAN ON')
  sim.uart('OFF')
  sim.expect('LIGHT OFF, FAN TIMER STARTED')
