# Análisis de referencia: Gross Beat

Fecha: 28 de octubre de 2026. Alcance: inspección estática de cabeceras PE y archivos descriptivos locales, más consulta del manual oficial. No se ejecutaron, desensamblaron ni modificaron las DLL originales. No se copiaron recursos gráficos, mapas ni código.

## Archivos presentes y formato real

| Archivo | Tamaño | Arquitectura | Cabecera opcional |
| --- | ---: | --- | --- |
| `Gross Beat/Gross Beat.dll` | 3.638.592 bytes | i386, `Machine=0x014c` | PE32, `0x010b` |
| `Gross Beat/Gross Beat_x64.dll` | 5.719.360 bytes | AMD64, `Machine=0x8664` | PE32+, `0x020b` |

SHA-256:

```text
Gross Beat.dll      945c8e098c7b17db1975aa3ed0fc5d83aca4b0a278bda718fb2f61892df5ea8d
Gross Beat_x64.dll  fa723fcb8d04b8be99ffe3261730a1421eabd1d990e02bf05cd3dd661173d8ba
```

Ambas DLL publican exactamente estas cuatro exportaciones con nombre:

```text
CreatePlugInstance
TMethodImplementationIntercept
__dbk_fcall_wrapper
dbkFCallWrapperAddr
```

No publican `VSTPluginMain`, `main` ni `GetPluginFactory`. La interfaz observada corresponde a la variante nativa de FL Studio; no es una DLL VST2/VST3 estándar que pueda convertirse de formato renombrándola. Esta clasificación se infiere de las exportaciones, sin cargar el binario. Image-Line distingue expresamente sus plugins nativos, que sólo funcionan en FL Studio, de las versiones VST. [Diferencias entre formatos, Image-Line](https://support.image-line.com/action/knowledgebase?ans=504).

`Plugin.nfo` identifica el proveedor como Image-Line. El RTF lo describe como un efecto de repetición de beats. Los campos Windows FileVersion, ProductVersion y CompanyName consultados están vacíos. Existen `Artwork/Back.bmp`, `Artwork/About.png` y cinco mapas `.fnv`; no se incorporaron al desarrollo.

## Comportamiento documentado

El efecto procesa audio entrante, con un búfer circular de dos compases y 36 mapas temporales más 36 de volumen. Permite repetición, stutter, scratch, cambios de velocidad/pitch y gating. El mapa temporal representa un desplazamiento respecto al audio actual. [Manual principal](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/Gross%20Beat.htm).

El mezclador Time abarca 0–100 %, hay un offset para scratch y el botón HOLD repite un compás durante el transporte. Los slots temporales se editan, renombran, intercambian y automatizan. [Time Settings](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/GrossBeat_TimeSettings.htm).

Volumen tiene mezcla 0–100 %, ataque 0–500 ms, release 0–1000 ms y tensión −100–100 %. Sus 36 slots también admiten selección MIDI, automatización y renombrado. El manual contiene nombres de octava MIDI inconsistentes: C1/C4 en una sección y C2/C5 en otra; deben documentarse números de nota y verificarse. Distingue slot en edición, en cola y reproduciéndose. [Volume Settings](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/GrossBeat_VolumeSettings.htm).

Resumen del editor y sincronización:

- Área: cuatro beats horizontales, ocho verticales; indicador de transporte, guías de seguridad/reversa.
- Curvas: Single, Double, Alt Single, Alt Double, Hold, Stairs, Smooth Stairs, Pulse, Wave y Half Sine; tensión editable.
- Puntos: agregar/mover/eliminar, rejilla, bloqueo por eje, pasos, desplazamiento grupal y bloqueo de edición.
- Herramientas: estados, portapapeles, historial, inversión, escala/normalización, reducción/suavizado de puntos, secuenciador y análisis de audio.
- Link empareja slots. Key-held mantiene durante la nota; One-shot retorna al slot 1 tras una pasada.
- Hold: Off, Last (último compás almacenado) o Next (capturar el siguiente).
- Trigger Sync: inmediato o próximo límite global de ¼, ½, 1, 2 o 4 beats. Position Sync: inicio del mapa o posición según la rejilla global.
- Horizontal reproduce normalmente; descendente frena/pausa/invierte; ascendente acelera. Saltos reposicionan la lectura. La guía señala audio aún no capturado.

[Envelope Mapping Panel](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/GrossBeat_EnvelopePanel.htm).

Opciones globales: reducción de clics, compensación de ataque, eliminación de DC, remuestreo HQ con sinc adaptativa y visualización del control de scratch. Los algoritmos internos exactos no se especifican. [Main Options](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/GrossBeat_MainOptions.htm).

La documentación de la antigua variante VST incluye presets/bancos FXP/FXB, tamaños Full/Compact, MIDI learn, modo de render y barra de ayuda. Esa descripción no demuestra que las DLL locales implementen la API VST. [VST Hint Bar](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/GrossBeat_HintBar.htm). La base de soporte indica que Image-Line dejó de vender variantes VST/AU en 2021. [Soporte actual sobre formatos externos](https://support.image-line.com/action/knowledgebase?ans=506).

## Deducción útil para una implementación independiente

Con coordenadas normalizadas `x` horizontal, `y` creciendo hacia abajo, el retraso es `8*y` beats y un recorrido horizontal completo dura cuatro beats. Por cálculo, la velocidad relativa es `1 - 2*dy/dx`. Una recta `y:0→0,25` da media velocidad; `0→0,5`, pausa; `0→1`, reversa normal. Es una deducción del mapa documentado, no código recuperado del original.

## Límites de lo demostrado

Este análisis no prueba identidad sonora, visual ni compatibilidad de presets/proyectos. Sin código fuente o mediciones A/B no pueden afirmarse fórmulas exactas de tensión/interpolación, filtros, fades, latencia, anti-aliasing, tratamiento de seeks/cambios de tempo, prioridades MIDI, redondeos ni comportamiento en todos los hosts. Tampoco se verificaron licencias, activación, integridad de distribución o versión comercial de las DLL. Una nueva DLL requiere su propia API de plugin, identidad y recursos; cualquier equivalencia debe validarse con pruebas de audio y host explícitas.
