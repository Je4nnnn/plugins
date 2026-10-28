# BeatWeaver — VST2 de 64 bits para Windows

Efecto estéreo de tiempo y volumen inspirado en el funcionamiento documentado de Gross Beat.
El desarrollo, la interfaz, las curvas y los patrones son propios; no usa gráficos ni código del original.
No se afirma equivalencia exacta de sonido, interfaz, presets o proyectos con Gross Beat.

## Archivos para usarlo

- Plugin: [dist/BeatWeaver_x64.dll](dist/BeatWeaver_x64.dll).
- Captura de la interfaz: [dist/BeatWeaver.png](dist/BeatWeaver.png).
- Demostración: [dist/BeatWeaver-demo.wav](dist/BeatWeaver-demo.wav).
- Resultado de validación: [dist/validation.json](dist/validation.json).
- Banco inicial: [dist/BeatWeaver-Factory.bwv](dist/BeatWeaver-Factory.bwv).

La DLL requiere Windows y un host compatible con efectos VST2 de 64 bits.
Python y las herramientas de compilación sólo se necesitan para desarrollar o ejecutar las pruebas.

## Instalación en FL Studio

1. Crea una carpeta propia para plugins, por ejemplo `C:\VST64\BeatWeaver`.
2. Copia allí `dist\BeatWeaver_x64.dll`.
3. En FL Studio abre **Options → Manage plugins**.
4. Agrega esa carpeta a las rutas de búsqueda de plugins.
5. Activa la verificación de plugins y ejecuta **Find installed plugins**.
6. Busca **BeatWeaver** y cárgalo como efecto en un slot del Mixer.

El plugin necesita audio en la entrada: coloca un instrumento o un archivo de audio en ese canal.
Para los mapas temporales, inicia el transporte y deja entrar audio para llenar el historial.
La reversa o un retraso hacia audio aún no capturado pueden producir silencio al principio.
El procedimiento sigue la [guía oficial de instalación de plugins](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/basics_externalplugins.htm).
La DLL se valida con un host de pruebas propio; su carga en FL Studio no se ha comprobado aquí.

## Uso rápido

- **TIME BANK** contiene 36 patrones temporales; **VOLUME BANK**, 36 patrones de volumen.
- Haz clic en un slot y edita su curva en la pestaña correspondiente.
- Prueba **Half Speed**, **Reverse** o **Repeat 4**, y combina un patrón de volumen como **Gate 4**.
- **TIME MIX** mezcla el audio directo y el procesado temporalmente.
- **VOL MIX** ajusta cuánto se aplica la curva de volumen.
- **BYPASS** deja pasar el audio de entrada; **OUTPUT** controla el nivel de salida del efecto.

El eje horizontal recorre un ciclo. En tiempo, bajar la curva aumenta el retraso de lectura.
Una curva horizontal conserva la velocidad; las pendientes permiten frenar, acelerar e invertir.
En volumen, arriba equivale a nivel completo y abajo a silencio.

## Sincronización, MIDI y HOLD

- **LENGTH** fija el ciclo entre ¼ y 16 beats; el valor inicial es 4 beats.
- **TRIGGER** cuantiza los cambios de slot; **Free** los aplica inmediatamente.
- **POSITION** alinea la posición inicial a una rejilla del transporte; **Free** comienza desde el inicio.
- **LINK** selecciona el mismo número de slot en ambos bancos.
- **ONE SHOT** reproduce un ciclo y vuelve al slot 1.
- **KEY HELD** mantiene la selección MIDI mientras la tecla siga pulsada y restaura la selección al soltarla.
- **HOLD MODE: Last** repite el último ciclo de audio capturado al activar **HOLD**.
- **HOLD MODE: Next** captura el siguiente ciclo y luego lo repite mientras **HOLD** siga activo.
- **HOLD MODE: Off** desactiva esa captura. Su duración depende de **LENGTH**.
- **SCRATCH** agrega un desplazamiento temporal manual.

Con **MIDI** habilitado, envía notas al plugin desde el enrutamiento MIDI de tu host:

| Números de nota MIDI | Banco | Slots |
| --- | --- | --- |
| 12–47 | Volumen | 1–36 |
| 48–83 | Tiempo | 1–36 |

Se indican números porque los nombres de octava varían entre hosts.
**ATTACK** suaviza subidas de volumen, hasta 500 ms; **RELEASE**, bajadas, hasta 1000 ms.
**TENSION** modifica la respuesta global de volumen; **CLICK FADE** suaviza saltos temporales y **DC CUT** elimina DC.

## Editar y guardar

- Doble clic en el gráfico: agrega un punto. Arrastra un punto para moverlo.
- Clic derecho en un punto o tramo: elige la curva o elimina un punto interior.
- Rueda sobre un tramo, o **Alt + arrastrar**: modifica su tensión o frecuencia según el modo.
- **SNAP** ajusta la rejilla; **Shift** permite movimiento vertical libre y ajuste fino de perillas.
- **SLIDE** desplaza conjuntamente los puntos posteriores; **EDIT LOCK** bloquea las ediciones del gráfico.
- **Delete** elimina el punto interior seleccionado; **Ctrl+C / Ctrl+V / Ctrl+Z** copian, pegan y deshacen.
- Clic derecho en un slot: renombra, copia, pega, restaura o intercambia con el slot vecino.
- Doble clic en un slot o **F2**: renombra. Doble clic en una perilla: restaura su valor inicial.

Hay 12 modos: Linear, Hold, Smooth, Single curve, Double curve, Alternate single,
Alternate double, Stairs, Smooth stairs, Pulse, Wave y Half sine.
El modo de un punto se aplica al tramo que sale de él hacia el siguiente punto.

En **MENU → Save bank as** guarda los 72 patrones y parámetros en un archivo `.bwv`.
**Open bank** restaura ese banco. El host también puede guardar el estado completo con su proyecto.
**PRESETS** carga uno de los patrones propios en el slot actual.

## Diferencias respecto de Gross Beat

- Interfaz propia, de tamaño fijo, con organización y gestos diferentes.
- Sincronización, Link, Key Held, One Shot y Hold Mode son opciones globales, no opciones por slot.
- Los 72 patrones y las fórmulas de las curvas son propios; no reproducen los presets originales.
- Importa bancos `.bwv`; no importa presets de Gross Beat, archivos `.fnv`, `.fst`, `.fxp` o `.fxb`.
- El remuestreo usa interpolación lineal; no implementa HQ sinc ni compensación de ataque de Gross Beat.
- No incluye todas las herramientas avanzadas del editor original, como análisis de audio o secuenciador de envelopes.
- Se entrega una DLL VST2 x64; no hay versión VST3 ni aplicación autónoma.
- No se han realizado pruebas A/B con Gross Beat ni se garantiza abrir proyectos que lo utilicen.

Las DLL de referencia presentes en `Gross Beat/` pertenecen al formato nativo de FL Studio.
Los detalles de esa inspección están en [reference-analysis.md](reference-analysis.md).

## Compilar y validar

Para compilar: Windows, Visual Studio Build Tools con **Desktop development with C++** y Windows SDK.
El script busca `vcvars64.bat`, compila C++17 y deja la DLL en `dist/`.

```powershell
.\build.ps1
```

Para ejecutar también las pruebas, instala Python de 64 bits, NumPy y Pillow:

```powershell
python -m pip install numpy pillow
.\build.ps1 -Test
```

La validación carga la DLL real y comprueba audio, MIDI, estado, transporte y editor con un host propio.
También genera la captura, el WAV de ejemplo y `validation.json`, con resultado y hash del binario probado.
La demo dura 32 segundos: 0–8 directo, 8–16 media velocidad, 16–24 reversa y 24–32 repetición con gate.

Fuentes funcionales: [manual de Gross Beat](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/Gross%20Beat.htm),
[editor y sincronización](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/GrossBeat_EnvelopePanel.htm)
y [opciones de procesamiento](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/GrossBeat_MainOptions.htm).
