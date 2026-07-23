# Fibló → Tempo del DAW (Spectrum to Rhythm → CC20)

El Fibló emite **CC20** por su salida MIDI cuando activas **Tempo sync** en el panel
Spectrum to Rhythm. La codificación es:

```
CC20 = round(BPM) − 20      (por tanto  BPM = valor_CC20 + 20)
```

Rango: valor 0–127 → **20–147 BPM**, resolución 1 BPM. El BPM sale del **pulso 1**
(el primer slot activo) × 2^k (botones x2 / /2 del panel).

Un plugin **no puede** escribir el tempo del host directamente (el estándar VST3 da
el tempo en solo-lectura), y el sistema de MIDI-map de los DAW no escucha el MIDI
interno de un plugin. Por eso el tempo hay que "cerrarlo" por una de estas dos vías.

---

## Vía A — Ableton Live (fácil, sin drivers): device Max for Live

**Archivo:** `Fibló Tempo Sync.amxd` (ya copiado a tu
`Documents\Ableton\User Library\Presets\MIDI Effects\Max MIDI Effect\`).

Qué hace: recibe el CC20 y ajusta `live_set.tempo = valor + 20` vía la Live API.
Requiere Ableton **Suite** o licencia Max for Live (tu Live 12 Trial lo incluye).

**Pasos:**
1. Crea una **pista MIDI** nueva (la llamo "Tempo").
2. En esa pista: **MIDI From → (la pista del Fibló) → 2º selector "Fibló Galàctic"**,
   y **Monitor: In**. (Igual que cualquier pista que reciba los pulsos.)
3. Arrastra **Fibló Tempo Sync** desde el navegador (User Library → Presets →
   MIDI Effects) a esa pista. *(O arrástralo desde la carpeta del repo `m4l\`.)*
4. En el Fibló: abre **Spectrum to Rhythm**, rutea al menos un parcial y activa
   **Tempo sync (CC20)**.
5. Toca una nota → el device recibe el CC20 → **el tempo de Live salta al BPM del
   pulso 1**. Cambia de nota o pulsa x2 / /2 y el tempo sigue.

No hace falta mapear nada ni instalar drivers. El device también deja pasar el MIDI
(passthrough), así que no molesta al resto de la cadena.

> La primera vez, Live tarda unos segundos en arrancar Max for Live. Si el device
> muestra un error en Max, dímelo y lo reviso.

---

## Vía B — Cualquier DAW (Reaper, FL, Cubase con soporte, Bitwig…): loopMIDI

Necesita un puerto MIDI virtual porque hay que sacar el CC20 de la salida MIDI del
plugin y volver a meterlo por una **entrada** que el DAW pueda mapear a su tempo.

**Setup único:**
1. Instala **loopMIDI** (gratis, Tobias Erichsen) y crea un puerto (p. ej. "Fibló").
2. En el DAW, enruta la **salida MIDI del Fibló** hacia el puerto **loopMIDI**
   (en Ableton: MIDI To → loopMIDI; en Reaper: un envío MIDI de la pista al puerto).
3. Activa el puerto **loopMIDI como entrada de control** del DAW:
   - Ableton: Preferences → Link/MIDI → activa **Remote** en la entrada loopMIDI.
   - Reaper: Preferences → MIDI Devices → habilita la entrada + "control".
   - FL Studio: MIDI settings → habilita el puerto de entrada.
4. **MIDI-map el control de Tempo** del DAW al **CC20** que llega por loopMIDI, y fija
   el rango del mapeo a **20–147 BPM** (así el valor recibido ES exactamente el BPM).
5. En el Fibló, activa **Tempo sync (CC20)**. Toca una nota → el tempo se mueve.

> En Ableton, la Vía A es más cómoda. La Vía B es la universal.

---

## Reconstruir el device (si tocas el patch)

El device es `FibloTempoSync.maxpat` (JSON de Max) envuelto en el contenedor `.amxd`.
Para regenerar el `.amxd` tras editar el `.maxpat`:

```powershell
powershell -ExecutionPolicy Bypass -File m4l\build_amxd.ps1
```

Estructura del contenedor `.amxd` (chunks, tamaños little-endian):

```
"ampf" [4] "mmmm"          ← marcador de tipo: mmmm=MIDI, aaaa=audio, iiii=instrumento
"meta" [4] 00 00 00 00
"ptch" [N] <N bytes del .maxpat JSON>
```

El tipo va también dentro del JSON en `project.amxdtype` (= el marcador como entero
de 32 bits; "mmmm" = 1836411501). Ambos deben coincidir.

Grafo del patch:
```
midiin ─┬─> midiparse ─(outlet 2: [ctl#, valor])─> route 20 ─> + 20 ─> prepend set tempo ─┐
        └─> midiout (passthrough)                                                          v
live.thisdevice ─(bang al cargar)─> [path live_set] ─> live.path ─(id)──────────────> live.object
```
