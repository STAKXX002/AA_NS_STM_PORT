"""Fast checks: boot, command parser, light/fan, no-calibration guards."""


def test_boot_banner(sim):
  assert 'READY' in sim.transcript
  assert 'CAL REQUIRED' in sim.transcript


def test_unknown_command(sim):
  sim.uart('FOO')
  sim.expect('UNKNOWN')


def test_light_and_fan_on_off(sim):
  sim.uart('ON')
  sim.expect('LIGHT & FAN ON')
  sim.uart('OFF')
  sim.expect('LIGHT OFF, FAN TIMER STARTED')


def test_go_requires_calibration(sim):
  sim.uart('GO')
  sim.expect('NO CAL')


def test_ret_requires_calibration(sim):
  sim.uart('RET')
  sim.expect('NO CAL')
