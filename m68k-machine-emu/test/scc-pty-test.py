#!/usr/bin/env python3
"""Prova end-to-end del canale B dell'SCC.

Avvia l'harness con --scc-b pty, si attacca al capo host del pty, manda dei
byte e controlla che tornino indietro: il giro completo passa per il pty,
la FIFO di ricezione del chip, l'interrupt di livello 4, il gestore dentro
test/serial.bin e poi la trasmissione a polling su RR0 D2.

Esce 0 se il giro funziona, 1 altrimenti.
"""
import os
import re
import subprocess
import sys
import time

HARNESS = sys.argv[1] if len(sys.argv) > 1 else "./m68k-harness"
ROM = sys.argv[2] if len(sys.argv) > 2 else "test/serial.bin"
MSG = b"PING-PPP-0123456789"

proc = subprocess.Popen(
    [HARNESS, ROM, "-n", "-f", "--scc-irq", "4", "--scc-b", "pty"],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)

def finish(ok, why=""):
    proc.terminate()
    try:
        _, err = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        _, err = proc.communicate()
    if not ok:
        sys.stderr.write(why + "\n")
        sys.stderr.write(err.decode(errors="replace")[-2000:])
    else:
        tail = err.decode(errors="replace")
        for line in tail.splitlines():
            if "SCC B:" in line or "SCC A:" in line:
                print("   ", line.strip())
    sys.exit(0 if ok else 1)

# il nome del pty compare su stderr appena l'harness parte
os.set_blocking(proc.stderr.fileno(), False)
pty_name, deadline, buf = None, time.time() + 10, b""
while time.time() < deadline and pty_name is None:
    chunk = proc.stderr.read(4096)
    if chunk:
        buf += chunk
        m = re.search(rb"canale B collegato a (\S+)", buf)
        if m:
            pty_name = m.group(1).decode()
    else:
        time.sleep(0.02)
if pty_name is None:
    finish(False, "non ho trovato il nome del pty nell'output dell'harness")
print("    pty:", pty_name)

fd = os.open(pty_name, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
time.sleep(0.3)                      # lascia finire l'init della ROM
os.read(fd, 4096) if False else None
try:
    os.write(fd, MSG)
except OSError as e:
    finish(False, "scrittura sul pty fallita: %s" % e)

got, deadline = b"", time.time() + 10
while time.time() < deadline and len(got) < len(MSG):
    try:
        chunk = os.read(fd, 256)
        if chunk:
            got += chunk
            continue
    except BlockingIOError:
        pass
    except OSError as e:
        finish(False, "lettura dal pty fallita: %s" % e)
    time.sleep(0.02)

if got != MSG:
    finish(False, "eco sbagliata:\n  spedito %r\n  tornato %r" % (MSG, got))
print("    %d byte spediti e tornati identici" % len(MSG))
finish(True)
