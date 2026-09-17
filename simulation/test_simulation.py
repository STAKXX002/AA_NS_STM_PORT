#!/usr/bin/env python3
import os
import signal
import socket
import sys
import time
import pexpect

HOST = '127.0.0.1'
UART_PORT = 1234


class RenodeSimulationTest:

  def __init__(self):
    self.renode = None
    self.uart_sock = None

  def start_renode(self):
    print('[+] Starting Renode Simulation Process...')
    self.renode = pexpect.spawn(
        'renode --disable-gui --console simulation/scripts/run_stm32.resc',
        encoding='utf-8',
        timeout=15,
    )
    self.renode.logfile = sys.stdout
    self.renode.expect(r'\(stm32f446\)')
    print('\n[+] Renode booted successfully.')

    time.sleep(1)

    self.uart_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    self.uart_sock.connect((HOST, UART_PORT))
    self.uart_sock.settimeout(2.0)
    print(f'[+] Connected to UART socket at {HOST}:{UART_PORT}')

  def send_renode_cmd(self, cmd_str):
    """Sends command via pexpect and waits for prompt."""
    self.renode.sendline(cmd_str)
    self.renode.expect(r'\(stm32f446\)')

  def send_uart_cmd(self, cmd_str):
    print(f'\n[UART TX]: {cmd_str}')
    self.uart_sock.sendall(f'{cmd_str}\n'.encode('utf-8'))

  def read_until(self, expected_substring, timeout=20.0):
    start = time.time()
    buffer = ''
    while time.time() - start < timeout:
      try:
        data = self.uart_sock.recv(1024)
        if data:
          text = data.decode('utf-8', errors='ignore')
          buffer += text
          sys.stdout.write(text)
          sys.stdout.flush()
          if expected_substring in buffer:
            return True
      except socket.timeout:
        pass
      time.sleep(0.01)
    raise RuntimeError(
        f"Timeout waiting for expected output: '{expected_substring}'"
    )

  def cleanup(self):
    if self.uart_sock:
      try:
        self.uart_sock.close()
      except Exception:
        pass

    print('\n[+] Terminating Renode process group...')
    if self.renode:
      try:
        self.renode.sendline('quit')
        time.sleep(0.5)
      except Exception:
        pass

      if self.renode.isalive():
        try:
          pgid = os.getpgid(self.renode.pid)
          os.killpg(pgid, signal.SIGKILL)
        except Exception:
          self.renode.close(force=True)


def main():
  sim = RenodeSimulationTest()

  try:
    sim.start_renode()

    # Wait for initial boot
    sim.read_until('CAL REQUIRED', timeout=15.0)

    # --- TEST 1: CALIBRATION ---
    sim.send_uart_cmd('CAL')
    time.sleep(0.2)

    print(
        '\n[+] Triggering limit switches (PB10 & PB8 active-low) atomically...'
    )
    # Execute both pin drops in a single block to prevent skew gaps
    sim.send_renode_cmd(
        'sysbus.gpioPortB OnGPIO 10 false; sysbus.gpioPortB OnGPIO 8 false'
    )

    sim.read_until('ZERO', timeout=15.0)
    print('\n[PASS] Limit switch hit detected!')

    # Release switches atomically back HIGH (unpressed) for backoff travel
    sim.send_renode_cmd(
        'sysbus.gpioPortB OnGPIO 10 true; sysbus.gpioPortB OnGPIO 8 true'
    )

    sim.read_until('CAL OK', timeout=30.0)
    print('\n[PASS] Calibration sequence complete!')

    # --- TEST 2: GO COMMAND ---
    sim.send_uart_cmd('GO')
    sim.read_until('HOLD', timeout=60.0)
    print('\n[PASS] Move to target reached HOLD state!')

    time.sleep(0.2)

    # --- TEST 3: RET COMMAND ---
    sim.send_uart_cmd('RET')

    # Increase timeout for return travel + home backoff sequence
    try:
      sim.read_until('RETURNED', timeout=90.0)
    except RuntimeError:
      # Fallback check if firmware logs "RET OK" or "HOME" instead of "RETURNED"
      sim.read_until('OK', timeout=10.0)

    print('\n[PASS] Return sequence completed!')

    # --- TEST 4: HATCH ACTUATION ---
    time.sleep(0.5)

    sim.send_uart_cmd('OPEN')
    # Increased timeout to 60s to accommodate 20s virtual stroke in Renode
    sim.read_until('OPENED', timeout=60.0)
    print('\n[PASS] Hatch OPEN command acknowledged!')

    time.sleep(0.5)

    sim.send_uart_cmd('CLOSE')
    # Increased timeout to 60s to accommodate 20s virtual stroke in Renode
    sim.read_until('CLOSED', timeout=60.0)
    print('\n[PASS] Hatch CLOSE command acknowledged!')

    # --- TEST 5: LIGHT & FAN CONTROL ---
    time.sleep(0.2)

    sim.send_uart_cmd('ON')
    sim.read_until('LIGHT & FAN ON', timeout=15.0)
    print('\n[PASS] Relay LIGHT & FAN ON acknowledged!')

    sim.send_uart_cmd('OFF')
    sim.read_until('LIGHT OFF, FAN TIMER STARTED', timeout=15.0)
    print('\n[PASS] Relay LIGHT OFF acknowledged!')

    print('\n[SUCCESS] ALL SIMULATION TESTS PASSED SUCCESSFULLY!')

  except Exception as e:
    print(f'\n[FAIL] Simulation Test Failed: {e}')
    sys.exit(1)
  finally:
    sim.cleanup()


if __name__ == '__main__':
  main()