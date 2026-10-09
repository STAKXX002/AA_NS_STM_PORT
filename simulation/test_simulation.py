#!/usr/bin/env python3
"""Renode simulation test with compact output.

Renode's own console output goes to simulation/renode.log, so the terminal
only shows one line per test step. On failure the last UART lines and the
tail of the Renode log are printed to help debugging.
"""
import contextlib
import os
import re
import signal
import socket
import sys
import time
import pexpect

HOST = '127.0.0.1'
UART_PORT = 1234

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RENODE_LOG = os.path.join(ROOT, 'simulation', 'renode.log')
PROMPT = r'\(stm32f446\)'

ANSI_RE = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Za-z0-9]')
CTRL_RE = re.compile(r'[^\x20-\x7e\n]')  # keep printable ASCII + newline

USE_COLOR = sys.stdout.isatty()


def color(text, code):
  return f'\x1b[{code}m{text}\x1b[0m' if USE_COLOR else text


PASS = color(' PASS ', '30;42')
FAIL = color(' FAIL ', '37;41')
DIM = lambda t: color(t, '2')


def clean(text):
  """Strips ANSI codes, stray control bytes and carriage returns."""
  text = ANSI_RE.sub('', text).replace('\r', '')
  return CTRL_RE.sub('', text)


class StepFailed(Exception):
  pass


class RenodeSimulationTest:

  def __init__(self):
    self.renode = None
    self.uart_sock = None
    self.rx = ''            # UART text received during the current step
    self.sent = ''          # last UART command sent
    self.history = []       # every cleaned UART line, for failure dumps
    self.results = []       # (name, ok, seconds)
    self.name_width = 8

  # ---------------------------------------------------------------- setup
  def start_renode(self):
    os.makedirs(os.path.dirname(RENODE_LOG), exist_ok=True)
    self.log_file = open(RENODE_LOG, 'w')
    self.renode = pexpect.spawn(
        'renode --disable-gui --console --plain '
        'simulation/scripts/run_stm32.resc',
        cwd=ROOT,
        encoding='utf-8',
        timeout=15,
    )
    self.renode.logfile_read = self.log_file  # keep Renode noise off screen
    self.renode.expect(PROMPT)

    version = re.search(r'version (\d+\.\d+\.\d+)', self.renode.before or '')
    time.sleep(1)

    self.uart_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    self.uart_sock.connect((HOST, UART_PORT))
    self.uart_sock.settimeout(0.2)
    ver = f'Renode {version.group(1)}' if version else 'Renode'
    print(f'{ver} booted - UART {HOST}:{UART_PORT}  '
          f'{DIM("(log: " + os.path.relpath(RENODE_LOG, ROOT) + ")")}\n')

  # --------------------------------------------------------------- actions
  def renode_cmd(self, cmd_str):
    self.renode.sendline(cmd_str)
    self.renode.expect(PROMPT)

  def uart(self, cmd_str):
    self.rx = ''
    self.sent = cmd_str
    self.uart_sock.sendall(f'{cmd_str}\n'.encode('utf-8'))

  def expect(self, substring, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
      try:
        data = self.uart_sock.recv(1024)
      except socket.timeout:
        continue
      if not data:
        break
      text = clean(data.decode('utf-8', errors='ignore'))
      self.rx += text
      self.history.extend(l for l in text.split('\n') if l.strip())
      if substring in self.rx:
        return
    raise StepFailed(f"timeout waiting for '{substring}'")

  # ----------------------------------------------------------------- steps
  def lines(self):
    """Distinct UART lines from this step, minus the echoed command."""
    out = []
    for line in self.rx.split('\n'):
      line = line.strip().strip('"')  # drop stray quote seen before READY
      if line and line != self.sent and line not in out:
        out.append(line)
    return out

  @contextlib.contextmanager
  def step(self, name):
    start = time.time()
    self.rx = ''
    self.sent = ''
    try:
      yield self
    except Exception as e:
      elapsed = time.time() - start
      self.results.append((name, False, elapsed))
      print(f'{FAIL} {name:<{self.name_width}} {e}  {DIM(f"{elapsed:.1f}s")}')
      raise StepFailed(str(e))
    elapsed = time.time() - start
    self.results.append((name, True, elapsed))
    detail = ' > '.join(self.lines())
    if len(detail) > 52:
      detail = detail[:49] + '...'
    print(f'{PASS} {name:<{self.name_width}} {detail:<52} '
          f'{DIM(f"{elapsed:5.1f}s")}')

  # --------------------------------------------------------------- cleanup
  def dump_failure_context(self):
    print(color('\n--- last UART lines ---', '33'))
    for line in self.history[-10:]:
      print(f'  {line}')
    print(color('--- tail of renode.log ---', '33'))
    try:
      with open(RENODE_LOG) as f:
        tail = clean(f.read()).strip().split('\n')[-8:]
      for line in tail:
        print(f'  {line[:110]}')
    except OSError:
      print('  (no log available)')

  def cleanup(self):
    if self.uart_sock:
      with contextlib.suppress(Exception):
        self.uart_sock.close()
    if self.renode:
      with contextlib.suppress(Exception):
        self.renode.sendline('quit')
        time.sleep(0.5)
      if self.renode.isalive():
        try:
          os.killpg(os.getpgid(self.renode.pid), signal.SIGKILL)
        except Exception:
          self.renode.close(force=True)
    with contextlib.suppress(Exception):
      self.log_file.close()


def main():
  sim = RenodeSimulationTest()
  t0 = time.time()
  ok = False

  try:
    sim.start_renode()

    with sim.step('boot'):
      sim.expect('CAL REQUIRED', timeout=15.0)

    with sim.step('CAL'):
      sim.uart('CAL')
      time.sleep(0.2)
      # Both limit pins dropped in one monitor line to avoid skew gaps.
      sim.renode_cmd('sysbus.gpioPortB OnGPIO 10 false; '
                     'sysbus.gpioPortB OnGPIO 8 false')
      sim.expect('ZERO', timeout=15.0)
      # Release switches (active-low) for backoff travel.
      sim.renode_cmd('sysbus.gpioPortB OnGPIO 10 true; '
                     'sysbus.gpioPortB OnGPIO 8 true')
      sim.expect('CAL OK', timeout=30.0)

    with sim.step('GO'):
      sim.uart('GO')
      sim.expect('HOLD', timeout=60.0)
    time.sleep(0.2)

    with sim.step('RET'):
      sim.uart('RET')
      try:
        sim.expect('RETURNED', timeout=90.0)
      except StepFailed:
        sim.expect('OK', timeout=10.0)  # fallback for older firmware logs
    time.sleep(0.5)

    with sim.step('OPEN'):
      sim.uart('OPEN')
      sim.expect('OPENED', timeout=60.0)
    time.sleep(0.5)

    with sim.step('CLOSE'):
      sim.uart('CLOSE')
      sim.expect('CLOSED', timeout=60.0)
    time.sleep(0.2)

    with sim.step('ON'):
      sim.uart('ON')
      sim.expect('LIGHT & FAN ON', timeout=15.0)

    with sim.step('OFF'):
      sim.uart('OFF')
      sim.expect('LIGHT OFF, FAN TIMER STARTED', timeout=15.0)

    ok = True
  except StepFailed:
    sim.dump_failure_context()
  except Exception as e:  # boot/socket/pexpect errors outside a step
    print(f'{FAIL} setup    {e}')
    sim.dump_failure_context()
  finally:
    sim.cleanup()

  passed = sum(1 for _, p, _ in sim.results if p)
  total = len(sim.results)
  summary = f'\n{passed}/{total} steps passed in {time.time() - t0:.1f}s'
  print(color(summary, '32' if ok else '31'))
  sys.exit(0 if ok else 1)


if __name__ == '__main__':
  main()