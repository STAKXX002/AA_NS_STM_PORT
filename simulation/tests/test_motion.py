"""Calibration and gantry motion. Tests marked slow wait for simulated travel."""
import time

import pytest


@pytest.mark.slow
def test_calibration_sequence(sim):
  sim.calibrate()
  assert 'SKEW:' in sim.transcript


@pytest.mark.slow
def test_go_reaches_hold(calibrated):
  calibrated.uart('GO')
  calibrated.expect('HOLD', timeout=60.0)


@pytest.mark.slow
def test_return_after_go(calibrated):
  calibrated.uart('GO')
  calibrated.expect('HOLD', timeout=60.0)
  calibrated.uart('RET')
  calibrated.expect('RETURNED', timeout=90.0)


@pytest.mark.slow
def test_ret_before_go_is_invalid(calibrated):
  calibrated.uart('RET')
  calibrated.expect('INVALID')


def test_second_cal_is_busy(sim):
  sim.uart('CAL')            # limits released -> starts moving
  time.sleep(0.3)
  sim.uart('CAL')
  sim.expect('BUSY')


def test_stop_halts_and_decalibrates(sim):
  sim.uart('CAL')
  time.sleep(0.3)
  sim.uart('STOP')
  sim.expect('HATCH STOPPED')
  sim.uart('GO')
  sim.expect('NO CAL')       # emergency stop clears calibration


def test_stop_does_not_latch_fault(sim):
  sim.uart('STOP')
  sim.expect('HATCH STOPPED')
  sim.uart('ON')
  sim.expect('LIGHT & FAN ON')   # would print FAULT - RST REQUIRED if latched
