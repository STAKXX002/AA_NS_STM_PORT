"""Fault policy: while faulted only RST, OFF and STOP are accepted."""


def test_limit_active_latches_fault(sim):
  sim.press(z1=True)
  sim.uart('CAL')
  sim.expect('FAULT: LIMIT ACTIVE')


def test_on_rejected_while_faulted(faulted):
  faulted.uart('ON')
  faulted.expect('FAULT - RST REQUIRED')


def test_off_allowed_while_faulted(faulted):
  faulted.uart('OFF')
  faulted.expect('LIGHT OFF')


def test_stop_allowed_while_faulted(faulted):
  faulted.uart('STOP')
  faulted.expect('HATCH STOPPED')


def test_motion_and_hatch_blocked_while_faulted(faulted):
  faulted.uart('CAL')
  faulted.expect('BUSY')
  faulted.uart('OPEN')
  faulted.expect('BUSY')
  faulted.uart('GO')
  faulted.expect('NO CAL')


def test_rst_clears_fault(faulted):
  faulted.uart('RST')
  faulted.expect('NO CAL')
  faulted.uart('ON')
  faulted.expect('LIGHT & FAN ON')
