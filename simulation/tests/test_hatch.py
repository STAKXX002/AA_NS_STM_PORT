"""Hatch actuator behaviour."""
import time

import pytest


def test_second_open_is_busy(sim):
  sim.uart('OPEN')
  sim.expect('OPENING')
  sim.uart('OPEN')
  sim.expect('BUSY')
  sim.uart('STOP')
  sim.expect('HATCH STOPPED')


def test_close_while_opening_is_busy(sim):
  sim.uart('OPEN')
  sim.expect('OPENING')
  sim.uart('CLOSE')
  sim.expect('BUSY')


def test_hatch_blocked_during_calibration(sim):
  sim.uart('CAL')
  time.sleep(0.3)
  sim.uart('OPEN')
  sim.expect('BUSY')


@pytest.mark.slow
def test_open_then_close(sim):
  sim.uart('OPEN')
  sim.expect('OPENED', timeout=60.0)
  sim.uart('CLOSE')
  sim.expect('CLOSED', timeout=60.0)
