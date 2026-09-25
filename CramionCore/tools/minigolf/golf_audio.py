# Musica y efectos del minigolf (chiptune), sin dependencias: WAV 16 bits mono.
import math, random, struct, sys, os, array

RATE = 44100
out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)

NOTE = {'C': 0, 'C#': 1, 'D': 2, 'D#': 3, 'E': 4, 'F': 5, 'F#': 6, 'G': 7, 'G#': 8, 'A': 9, 'A#': 10, 'B': 11}

def freq(name):
    n, octave = name[:-1], int(name[-1])
    midi = 12 * (octave + 1) + NOTE[n]
    return 440.0 * 2 ** ((midi - 69) / 12)

def buffer(seconds):
    return array.array('f', [0.0]) * int(seconds * RATE)

def add_tone(buf, start, dur, f, vol, wave='square', duty=0.5, attack=0.005, release=0.06, vibrato=0.0, slide=0.0):
    i0 = int(start * RATE)
    n = int(dur * RATE)
    rel = int(release * RATE)
    att = max(1, int(attack * RATE))
    phase = 0.0
    total = n + rel
    for k in range(total):
        i = i0 + k
        if i >= len(buf):
            break
        t = k / RATE
        ff = f * (1.0 + slide * t) * (1.0 + vibrato * math.sin(2 * math.pi * 5.5 * t) * min(1.0, t * 3))
        phase += ff / RATE
        p = phase - math.floor(phase)
        if wave == 'square':
            s = 1.0 if p < duty else -1.0
        elif wave == 'triangle':
            s = 4 * abs(p - 0.5) - 1
        elif wave == 'saw':
            s = 2 * p - 1
        else:
            s = math.sin(2 * math.pi * p)
        env = min(1.0, k / att)
        if k >= n:
            env *= max(0.0, 1.0 - (k - n) / max(rel, 1))
        else:
            env *= 1.0 - 0.25 * (k / max(n, 1))  # leve caida
        buf[i] += s * env * vol

def add_kick(buf, start, vol=0.9):
    i0 = int(start * RATE)
    phase = 0.0
    for k in range(int(0.18 * RATE)):
        i = i0 + k
        if i >= len(buf): break
        t = k / RATE
        f = 50 + 110 * math.exp(-t * 30)
        phase += f / RATE
        buf[i] += math.sin(2 * math.pi * phase) * math.exp(-t * 16) * vol

def add_noise(buf, start, dur, vol, decay, seed, hp=True):
    rnd = random.Random(seed)
    i0 = int(start * RATE)
    last = 0.0
    for k in range(int(dur * RATE)):
        i = i0 + k
        if i >= len(buf): break
        t = k / RATE
        x = rnd.uniform(-1, 1)
        y = x - last if hp else (x + last) * 0.5
        last = x
        buf[i] += y * math.exp(-t * decay) * vol

def lowpass(buf, amount):
    y = 0.0
    for i in range(len(buf)):
        y += (buf[i] - y) * amount
        buf[i] = y

def write(name, buf, peak=0.85):
    m = max(1e-6, max(abs(x) for x in buf))
    g = peak / m
    data = array.array('h', (int(max(-1.0, min(1.0, x * g)) * 32767) for x in buf))
    path = os.path.join(out_dir, name)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data) * 2) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b'data' + struct.pack('<I', len(data) * 2))
        f.write(data.tobytes())
    print(name, round(len(buf) / RATE, 1), 's')

CHORDS = {
    'C': ['C3', 'E4', 'G4', 'C5'], 'Am': ['A2', 'C4', 'E4', 'A4'], 'F': ['F2', 'A3', 'C4', 'F4'],
    'G': ['G2', 'B3', 'D4', 'G4'], 'Dm': ['D3', 'F4', 'A4', 'D5'], 'Em': ['E3', 'G3', 'B3', 'E4'],
}

def song(name, bpm, progression, melody, drums=True, swing=0.0, intro_bars=0, lead_vol=0.22, lead_duty=0.5):
    eighth = 60.0 / bpm / 2
    bar = eighth * 8
    length = bar * len(progression)
    buf = buffer(length + 0.5)
    for b, chord in enumerate(progression):
        t0 = b * bar
        notes = CHORDS[chord]
        # Bajo (triangulo): raiz en corcheas con salto de octava.
        for e in range(8):
            f = freq(notes[0]) * (2 if e % 4 == 2 else 1)
            add_tone(buf, t0 + e * eighth, eighth * 0.8, f, 0.30, 'triangle', release=0.03)
        # Arpegio (cuadrada fina) en semicorcheas.
        for s in range(16):
            n = notes[1 + (s % 3)] if s % 4 != 3 else notes[3]
            add_tone(buf, t0 + s * eighth / 2, eighth / 2 * 0.7, freq(n) * 2, 0.045, 'square', duty=0.125, release=0.02)
        if drums and b >= intro_bars:
            for beat in range(4):
                tb = t0 + beat * eighth * 2
                if beat in (0, 2):
                    add_kick(buf, tb)
                else:
                    add_noise(buf, tb, 0.14, 0.35, 22, seed=b * 10 + beat, hp=False)  # caja
                for h in range(2):
                    th = tb + h * eighth + (swing * eighth if h == 1 else 0)
                    add_noise(buf, th, 0.04, 0.12, 90, seed=b * 100 + beat * 2 + h)  # charles
    # Melodia (cuadrada con vibrato).
    t = 0.0
    for note, eighths in melody:
        d = eighths * eighth
        if note != 'R':
            add_tone(buf, t, d * 0.9, freq(note), lead_vol, 'square', duty=lead_duty, vibrato=0.004, release=0.08)
            add_tone(buf, t + eighth * 0.75, d * 0.8, freq(note), lead_vol * 0.25, 'square', duty=lead_duty, release=0.08)  # eco
        t += d
    lowpass(buf, 0.35)
    # Bucle limpio: el final entra suave en el principio.
    fade = int(0.02 * RATE)
    for k in range(fade):
        buf[k] *= k / fade
    write(name, buf[:int(length * RATE)])

MENU_PROG = ['C', 'Am', 'F', 'G', 'C', 'Am', 'Dm', 'G', 'F', 'G', 'Em', 'Am', 'F', 'G', 'C', 'C']
MENU_MELODY = [
    ('E5', 1), ('G5', 1), ('C6', 2), ('B5', 1), ('C6', 1), ('G5', 2),
    ('A5', 2), ('E5', 2), ('C5', 2), ('E5', 2),
    ('F5', 1), ('A5', 1), ('C6', 2), ('A5', 1), ('C6', 1), ('F6', 2),
    ('D6', 3), ('C6', 1), ('B5', 2), ('G5', 2),
    ('E5', 1), ('G5', 1), ('C6', 2), ('B5', 1), ('C6', 1), ('E6', 2),
    ('D6', 1), ('C6', 1), ('A5', 2), ('E5', 2), ('A5', 2),
    ('F5', 2), ('A5', 2), ('D6', 1), ('C6', 1), ('A5', 2),
    ('B5', 4), ('R', 2), ('G5', 1), ('A5', 1),
    ('A5', 2), ('C6', 2), ('F6', 2), ('E6', 1), ('D6', 1),
    ('D6', 2), ('B5', 2), ('G5', 2), ('B5', 2),
    ('G5', 1), ('B5', 1), ('E6', 2), ('D6', 1), ('B5', 1), ('G5', 2),
    ('A5', 3), ('B5', 1), ('C6', 2), ('E6', 2),
    ('F6', 2), ('E6', 1), ('D6', 1), ('C6', 2), ('A5', 2),
    ('B5', 2), ('D6', 2), ('G6', 2), ('F6', 1), ('D6', 1),
    ('E6', 2), ('D6', 1), ('C6', 1), ('G5', 2), ('E5', 2),
    ('C6', 4), ('R', 4),
]
song('menu.wav', 108, MENU_PROG, MENU_MELODY, drums=True, intro_bars=4, lead_vol=0.2)

# Musica de los niveles: mas tranquila (de fondo), melodia en saltos.
LEVEL_PROG = ['F', 'G', 'Em', 'Am', 'F', 'G', 'C', 'C', 'Dm', 'G', 'Em', 'Am', 'F', 'G', 'C', 'G']
LEVEL_MELODY = []
for chord in LEVEL_PROG:
    n = CHORDS[chord]
    up = [n[1][:-1] + str(int(n[1][-1]) + 1), n[2][:-1] + str(int(n[2][-1]) + 1), n[3][:-1] + str(int(n[3][-1]) + 1)]
    LEVEL_MELODY += [(up[0], 1), ('R', 1), (up[1], 1), (up[2], 1), ('R', 1), (up[1], 1), (up[0], 2)]
song('nivel.wav', 116, LEVEL_PROG, LEVEL_MELODY, drums=True, swing=0.25, lead_vol=0.12, lead_duty=0.25)

# --- Efectos ---
b = buffer(0.25)
add_noise(b, 0, 0.03, 0.8, 120, seed=1)
add_tone(b, 0, 0.02, 1400, 0.6, 'sine', release=0.08)
add_tone(b, 0, 0.02, 700, 0.5, 'sine', release=0.12)
write('golpe.wav', b)

b = buffer(0.2)
add_tone(b, 0, 0.01, 620, 0.7, 'sine', release=0.09)
add_noise(b, 0, 0.02, 0.2, 200, seed=2)
write('rebote.wav', b, peak=0.6)

b = buffer(1.6)
add_tone(b, 0, 0.05, 220, 0.8, 'sine', release=0.2, slide=-2.0)  # cae en el hoyo
add_noise(b, 0.02, 0.1, 0.4, 40, seed=3, hp=False)
for k, n in enumerate(['C5', 'E5', 'G5', 'C6']):
    add_tone(b, 0.25 + k * 0.1, 0.12, freq(n), 0.3, 'square', duty=0.25, release=0.1)
add_tone(b, 0.65, 0.5, freq('E6'), 0.25, 'square', duty=0.25, vibrato=0.01, release=0.3)
add_tone(b, 0.65, 0.5, freq('C6'), 0.2, 'square', duty=0.5, release=0.3)
write('hoyo.wav', b)

b = buffer(0.7)
add_tone(b, 0, 0.45, 700, 0.35, 'square', duty=0.25, slide=-1.4, release=0.15)
write('fuera.wav', b, peak=0.6)

b = buffer(0.12)
add_tone(b, 0, 0.03, freq('A5'), 0.4, 'square', duty=0.25, release=0.04)
add_tone(b, 0.035, 0.03, freq('E6'), 0.4, 'square', duty=0.25, release=0.04)
write('click.wav', b, peak=0.5)

b = buffer(2.2)
for k, n in enumerate(['C5', 'E5', 'G5', 'C6', 'E6', 'G6']):
    add_tone(b, k * 0.09, 0.08, freq(n), 0.25, 'square', duty=0.25, release=0.05)
for n in ['C5', 'E5', 'G5', 'C6']:
    add_tone(b, 0.6, 1.1, freq(n), 0.18, 'square', duty=0.5, vibrato=0.006, release=0.4)
write('fanfarria.wav', b)
