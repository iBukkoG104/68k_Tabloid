#!/bin/sh
# run-tests.sh - prova headless di display e touch. Niente finestra, niente
# dipendenze: gira anche in CI. Esce con 1 al primo controllo fallito.
set -e
cd "$(dirname "$0")/.."
H=./m68k-harness
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# ckre <descrizione> <espressione regolare> <file>
# Per le righe che dipendono dal rumore dell'ADC del touch: il valore esatto
# puo' ballare di un LSB a seconda di quando il firmware campiona.
ckre() {
    if grep -qE "$2" "$3"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1 (nessuna riga come: $2)"
        fail=1
    fi
}

ck() {   # ck <descrizione> <atteso> <file>
    if grep -qF "$2" "$3"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1 (non trovo: $2)"
        fail=1
    fi
}

echo "== geometria della finestra: scala, centratura, mappatura del mouse =="
if $H --panel-selftest >"$TMP/g.out" 2>&1; then
    echo "  ok   autotest geometria (scala intera, bande, angoli)"
else
    cat "$TMP/g.out"; echo "  FAIL autotest geometria"; fail=1
fi
if [ -x ./build/panel-e2e ]; then
    # driver dummy: niente finestra vera, ma SDL sintetizza gli eventi del
    # mouse, quindi la catena evento -> pannello viene provata davvero.
    if SDL_VIDEODRIVER=dummy ./build/panel-e2e >"$TMP/e.out" 2>&1; then
        echo "  ok   end-to-end mouse -> dito, finestra ingrandita e schermo intero"
    else
        grep -E 'FAIL|logical size' "$TMP/e.out" || cat "$TMP/e.out"
        echo "  FAIL end-to-end della mappatura del mouse"; fail=1
    fi
else
    echo "  --   end-to-end saltato (compilato senza SDL2)"
fi

echo "== Z8530 SCC: baud rate, FIFO, interrupt =="
if $H --scc-selftest >"$TMP/s.out" 2>&1; then
    echo "  ok   autotest del chip (tempi, FIFO da 3, IP/IUS, vettori, priorita')"
else
    cat "$TMP/s.out"; echo "  FAIL autotest del chip"; fail=1
fi
if python3 test/scc-pty-test.py "$H" test/serial.bin >"$TMP/sp.out" 2>&1; then
    sed -n 's/^/  /p' "$TMP/sp.out"
    echo "  ok   giro completo pty -> FIFO -> interrupt -> gestore -> tx"
else
    cat "$TMP/sp.out"; echo "  FAIL giro completo sul canale B"; fail=1
fi

echo "== display.bin: barre di colore, sfumatura, cornice =="
$H test/display.bin -n -f -c 40000000 -S "$TMP/display.ppm" >"$TMP/d.out" 2>"$TMP/d.err"
ck "384000 pixel disegnati" "quadro disegnato" "$TMP/d.out"
if grep -q 'BUS ERROR' "$TMP/d.err"; then echo "  FAIL bus error"; fail=1; else echo "  ok   nessun bus error"; fi
python3 - "$TMP/display.ppm" <<'PY'
import sys
f=open(sys.argv[1],'rb'); assert f.readline().strip()==b'P6'
w,h=map(int,f.readline().split()); f.readline(); d=f.read()
px=lambda x,y:tuple(d[(y*w+x)*3:(y*w+x)*3+3])
assert (w,h)==(800,480), (w,h)
bars=[px(x,100) for x in range(50,800,100)]
want=[(0,0,0),(0,0,255),(255,0,0),(0,255,0),(255,0,255),(255,255,0),(0,255,255),(255,255,255)]
assert bars==want, bars
assert px(400,0)==(255,255,255) and px(0,240)==(255,255,255), "cornice"
assert px(100,300)[0] < px(700,300)[0], "sfumatura"
print("  ok   immagine: 8 barre, cornice, sfumatura crescente")
PY

echo "== features.bin: letture dal chip, specchio, scroll hardware =="
$H test/features.bin -n -f -c 40000000 -S "$TMP/features.ppm" >"$TMP/f.out" 2>"$TMP/f.err"
ck "read_ddb"                "read_ddb       : 01 57 61 01 FF" "$TMP/f.out"
ck "set_pixel_data_interface" "pixel interface: 03"            "$TMP/f.out"
ck "get_power_mode"          "power mode     : 9C"             "$TMP/f.out"
ck "read_memory_start"       "read_memory    : F800 07E0 001F FFFF" "$TMP/f.out"
python3 - "$TMP/features.ppm" <<'PY'
import sys
f=open(sys.argv[1],'rb'); f.readline(); w,h=map(int,f.readline().split()); f.readline(); d=f.read()
px=lambda x,y:tuple(d[(y*w+x)*3:(y*w+x)*3+3])
xs=[];ys=[]
for y in range(h):
    for x in range(w):
        r,g,b=px(x,y)
        if r>200 and g>200 and b<100: xs.append(x); ys.append(y)
assert (min(xs),max(xs))==(760,799), (min(xs),max(xs))   # set_address_mode flipX
assert (min(ys),max(ys))==(100,139), (min(ys),max(ys))   # scroll VSP=140
assert px(400,20)==(255,255,255), "fascia fissa in alto"
assert px(400,460)==(0,255,0),    "fascia fissa in basso"
assert px(400,40)==(0,0,255),     "prima riga dell'area di scroll"
print("  ok   immagine: flipX a destra, scroll VSP=140, fasce fisse intatte")
PY

echo "== touch.bin: XPT2046 con tocchi da script =="
$H test/touch.bin -n -f -c 30000000 --touch-script test/touch.script \
   -S "$TMP/touch.ppm" >"$TMP/t.out" 2>"$TMP/t.err"
ckre "primo tocco"           'tap @ (99|100|101),(99|100|101)'     "$TMP/t.out"
ckre "tocco dopo la pulizia" 'tap @ (199|200|201),(119|120|121)'   "$TMP/t.out"
python3 - "$TMP/touch.ppm" <<'PY'
import sys
f=open(sys.argv[1],'rb'); f.readline(); w,h=map(int,f.readline().split()); f.readline(); d=f.read()
px=lambda x,y:tuple(d[(y*w+x)*3:(y*w+x)*3+3])
seen=[]
for y in range(h):
    for x in range(w):
        r,g,b=px(x,y)
        if r>200 and g>200 and b<100 and not any(abs(x-a)<20 and abs(y-c)<20 for a,c in seen):
            seen.append((x+5,y+5))
assert len(seen)==2, seen          # la barra in alto ha pulito i tocchi precedenti
for want in ((200,120),(600,360)):
    assert any(abs(a-want[0])<=3 and abs(b-want[1])<=3 for a,b in seen), (want, seen)
print("  ok   immagine: tela pulita dal tocco sulla barra, 2 quadrati al posto giusto")
PY

echo "== penirq.bin: /PENIRQ cablato su IRQ autovector 5 =="
$H test/penirq.bin -n -f -p 5 -c 30000000 --touch-script test/touch.script \
   >"$TMP/p.out" 2>"$TMP/p.err"
if grep -q 'BUS ERROR\|HALT' "$TMP/p.err"; then
    echo "  FAIL eccezione durante il gestore"; fail=1
else
    echo "  ok   nessuna eccezione"
fi
ck "il gestore e' partito"        "irq 100 -> " "$TMP/p.out"
ck "segue il dito durante il drag" "irq 300 -> 600,350" "$TMP/p.out"
ck "arriva all'ultimo tocco"       "irq 600 -> 600,360" "$TMP/p.out"

echo
if [ $fail -eq 0 ]; then echo "tutto a posto."; else echo "ci sono test falliti."; fi
exit $fail
