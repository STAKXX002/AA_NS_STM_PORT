#!/usr/bin/env python3
import socket
import time
import sys
import os
import signal
import pexpect

HOST = '127.0.0.1'
PORT = 1234

def main():
    print("[+] Starting Renode Simulation Process...")
    
    # Spawn headless Renode session
    renode = pexpect.spawn(
        'renode --disable-gui --console simulation/scripts/run_stm32.resc',
        encoding='utf-8',
        timeout=15
    )
    renode.logfile = sys.stdout

    sock = None

    try:
        renode.expect(r'\(stm32f446\)')
        print("\n[+] Renode booted successfully.")

        # Connect socket to emulated USART2 VCP
        time.sleep(1)
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, PORT))
        sock.settimeout(5.0)
        print(f"[+] Connected to UART socket at {HOST}:{PORT}")

        def send_cmd(cmd):
            sock.sendall(f"{cmd}\n".encode('utf-8'))
            print(f"\n[UART TX]: {cmd}")

        def read_until(expected_substring, timeout=15.0):
            start = time.time()
            buffer = ""
            while time.time() - start < timeout:
                try:
                    data = sock.recv(1024)
                    if data:
                        text = data.decode('utf-8', errors='ignore')
                        buffer += text
                        print(text, end='', flush=True)
                        if expected_substring in buffer:
                            return True
                except socket.timeout:
                    pass
            raise RuntimeError(f"Timeout waiting for expected output: '{expected_substring}'")

        # Wait for system boot sequence
        read_until("CAL REQUIRED")

        # --- TEST 1: CALIBRATION ---
        send_cmd("CAL")
        time.sleep(0.5)

        print("\n[+] Triggering stepper limit switches (PB10 & PB8 active-low)...")
        renode.sendline("sysbus.gpioPortB OnGPIO 10 false")
        renode.sendline("sysbus.gpioPortB OnGPIO 8 false")
        renode.expect(r'\(stm32f446\)')

        read_until("ZERO")
        print("\n[PASS] Limit switch hit detected!")

        # Release limit switches so motors can perform backoff travel
        renode.sendline("sysbus.gpioPortB OnGPIO 10 true")
        renode.sendline("sysbus.gpioPortB OnGPIO 8 true")
        renode.expect(r'\(stm32f446\)')

        # Wait for physical backoff step to complete (allow up to 20s)
        read_until("CAL OK", timeout=20.0)
        print("\n[PASS] Calibration sequence complete!")

        # --- TEST 2: GO COMMAND ---
        send_cmd("GO")
        read_until("HOLD", timeout=45.0)
        print("\n[PASS] Move to target reached HOLD state!")

        # --- TEST 3: RET COMMAND ---
        send_cmd("RET")
        read_until("RETURNED", timeout=45.0)
        print("\n[PASS] Return sequence completed!")

        # --- TEST 4: ASYNCHRONOUS HATCH ACTUATION ---
        send_cmd("OPEN")
        read_until("OPENING")
        # Increase timeout to 20.0s to allow Renode's virtual time to reach 5000ms
        read_until("OPENED", timeout=20.0)  
        print("\n[PASS] Hatch OPEN timed sequence completed!")

        send_cmd("CLOSE")
        read_until("CLOSING")
        read_until("CLOSED", timeout=20.0)
        print("\n[PASS] Hatch CLOSE timed sequence completed!")

        # --- TEST 5: LIGHT RELAY CONTROL ---
        send_cmd("ON")
        read_until("LIGHT ON")
        print("\n[PASS] Relay LIGHT ON acknowledged!")

        send_cmd("OFF")
        read_until("LIGHT OFF")
        print("\n[PASS] Relay LIGHT OFF acknowledged!")

        print("\n[SUCCESS] ALL SIMULATION TESTS PASSED SUCCESSFULLY!")

        time.sleep(0.2)

    except Exception as e:
        print(f"\n[FAIL] Simulation Test Failed: {e}")
    finally:
        if sock:
            try:
                sock.close()
            except Exception:
                pass
        
        print("\n[+] Terminating Renode process group...")
        try:
            renode.sendline("quit")
            time.sleep(0.5)
        except Exception:
            pass

        if renode.isalive():
            try:
                pgid = os.getpgid(renode.pid)
                os.killpg(pgid, signal.SIGKILL)
            except Exception:
                renode.close(force=True)

if __name__ == "__main__":
    main()