#!/usr/bin/env python3
"""
latency_test.py — Mesure de la latence audio round-trip (loopback)

Usage :
    python scripts/latency_test.py [--device-index N] [--sample-rate 44100] [--buffer-size 128]

Prérequis :
    pip install pyaudio numpy

Méthode :
    1. Envoie un pulse (spike) sur la sortie audio
    2. Écoute l'entrée en boucle (loopback physique câble jack sortie → entrée Scarlett)
    3. Détecte quand le pulse arrive en entrée
    4. Calcule le délai = latence round-trip totale

Note : Pour tester correctement, connecte physiquement la sortie casque à l'entrée
de la Scarlett Solo avec un câble jack 3.5 mm → 6.35 mm.
"""

import argparse
import sys
import time

try:
    import numpy as np
    import pyaudio
except ImportError:
    print("Erreur : pyaudio et numpy requis.")
    print("  pip install pyaudio numpy")
    sys.exit(1)


PULSE_THRESHOLD = 0.1   # Niveau minimum pour détecter le pulse en entrée
TIMEOUT_SEC     = 2.0   # Temps maximum d'attente du pulse retour


def list_devices(pa: pyaudio.PyAudio) -> None:
    print("\nPériphériques audio disponibles :")
    for i in range(pa.get_device_count()):
        info = pa.get_device_info_by_index(i)
        direction = []
        if info["maxInputChannels"] > 0:
            direction.append("entrée")
        if info["maxOutputChannels"] > 0:
            direction.append("sortie")
        print(f"  [{i}] {info['name']}  ({', '.join(direction)})")
    print()


def measure_latency(
    device_index: int | None,
    sample_rate: int,
    buffer_size: int,
) -> float | None:
    pa = pyaudio.PyAudio()

    if device_index is None:
        list_devices(pa)
        print("Utilisation du périphérique par défaut.")
        print("Spécifie --device-index N pour choisir un périphérique ASIO.\n")

    # Buffer de sortie : pulse au milieu + silence
    output_buffer = np.zeros(buffer_size, dtype=np.float32)
    pulse_position = buffer_size // 2
    output_buffer[pulse_position] = 1.0  # pulse

    results: list[float] = []
    num_measurements = 5

    print(f"Paramètres : {sample_rate} Hz, buffer {buffer_size} samples "
          f"({1000 * buffer_size / sample_rate:.1f} ms par buffer)")
    print(f"Réalisation de {num_measurements} mesures...\n")

    kwargs: dict = {
        "format": pyaudio.paFloat32,
        "channels": 1,
        "rate": sample_rate,
        "frames_per_buffer": buffer_size,
    }
    if device_index is not None:
        kwargs["input_device_index"]  = device_index
        kwargs["output_device_index"] = device_index

    for trial in range(num_measurements):
        try:
            out_stream = pa.open(output=True, **kwargs)
            in_stream  = pa.open(input=True,  **kwargs)

            # Envoyer le pulse
            t_send = time.perf_counter()
            out_stream.write(output_buffer.tobytes())

            # Écouter le retour
            t_receive = None
            deadline  = t_send + TIMEOUT_SEC

            while time.perf_counter() < deadline:
                raw = in_stream.read(buffer_size, exception_on_overflow=False)
                buf = np.frombuffer(raw, dtype=np.float32)
                if np.max(np.abs(buf)) > PULSE_THRESHOLD:
                    t_receive = time.perf_counter()
                    break

            out_stream.stop_stream()
            in_stream.stop_stream()
            out_stream.close()
            in_stream.close()

            if t_receive is not None:
                latency_ms = (t_receive - t_send) * 1000.0
                results.append(latency_ms)
                print(f"  Mesure {trial + 1} : {latency_ms:.1f} ms")
            else:
                print(f"  Mesure {trial + 1} : TIMEOUT — pulse non détecté")
                print("    → Vérifie la connexion loopback (sortie → entrée Scarlett)")

        except Exception as e:
            print(f"  Mesure {trial + 1} : ERREUR — {e}")

        time.sleep(0.1)

    pa.terminate()

    if results:
        avg = sum(results) / len(results)
        min_lat = min(results)
        max_lat = max(results)
        print(f"\nRésultats ({len(results)}/{num_measurements} mesures valides) :")
        print(f"  Moyenne : {avg:.1f} ms")
        print(f"  Min     : {min_lat:.1f} ms")
        print(f"  Max     : {max_lat:.1f} ms")

        target = 20.0
        if avg <= target:
            print(f"\n  ✓ Latence OK (≤ {target} ms)")
        else:
            print(f"\n  ✗ Latence trop élevée (> {target} ms)")
            print("    → Réduire la taille du buffer ou activer le driver ASIO")
        return avg
    else:
        print("\nAucune mesure valide. Vérifie :")
        print("  1. Câble loopback connecté (sortie casque → entrée Scarlett)")
        print("  2. Périphérique ASIO sélectionné (--device-index N)")
        return None


def main() -> None:
    parser = argparse.ArgumentParser(description="Mesure la latence audio round-trip")
    parser.add_argument("--device-index", type=int, default=None,
                        help="Index du périphérique audio (voir liste)")
    parser.add_argument("--sample-rate", type=int, default=44100,
                        help="Fréquence d'échantillonnage (défaut: 44100)")
    parser.add_argument("--buffer-size", type=int, default=128,
                        help="Taille du buffer audio (défaut: 128)")
    parser.add_argument("--list-devices", action="store_true",
                        help="Lister les périphériques disponibles et quitter")
    args = parser.parse_args()

    pa = pyaudio.PyAudio()
    if args.list_devices:
        list_devices(pa)
        pa.terminate()
        return
    pa.terminate()

    measure_latency(args.device_index, args.sample_rate, args.buffer_size)


if __name__ == "__main__":
    main()
