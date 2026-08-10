# YiyoOverlay

Overlay de rendimiento independiente para juegos DX11/DX12, pensado para
coexistir con OptiScaler (no lo modifica ni reemplaza ninguno de sus
archivos). Arquitectura equivalente a RTSS/MangoHUD/Special-K: una DLL que
se carga en el proceso del juego, engancha `Present()` vía vtable hooking
(MinHook) y dibuja con Dear ImGui.

## Estructura

```
YiyoOverlay/
├─ CMakeLists.txt
├─ config.ini
└─ src/
   ├─ dllmain.cpp        # entrada de la DLL, arranca hooks + hilo de métricas
   ├─ overlay.h/.cpp      # estado compartido y dibujo ImGui
   ├─ metrics.h/.cpp       # hilo de métricas (CPU/RAM/GPU/VRAM), cada 500ms
   ├─ hook_dx11.h/.cpp     # hook de IDXGISwapChain::Present para DX11
   └─ hook_dx12.h/.cpp     # hook de Present + ExecuteCommandLists para DX12
```

ImGui y MinHook se descargan automáticamente vía `FetchContent` al
configurar el proyecto (no vienen vendorizados en este repo).

## Compilar

Requisitos: Visual Studio 2022 con "Desarrollo para el escritorio con C++",
CMake 3.21+, conexión a internet (para FetchContent la primera vez).

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

La DLL resultante queda en `build/bin/Release/YiyoOverlay.dll`, junto a una
copia de `config.ini`.

## Cómo se carga en el juego

Este proyecto entrega la DLL del overlay, **no un inyector**. Dos formas
estándar de cargarla en un juego que ya posees (mismo enfoque que usan
ReShade/ENB), de menor a mayor invasividad:

1. **DLL proxy** (recomendado): renombrar `YiyoOverlay.dll` a un nombre que
   el juego ya cargue por orden de búsqueda de Windows (p. ej. `dxgi.dll` o
   `version.dll` en la carpeta del ejecutable) y reenviar las exportaciones
   reales de esa DLL del sistema a la copia original. Es el método que usa
   ReShade y es compatible con que OptiScaler haga lo mismo con otro nombre
   de proxy (p. ej. `dxgi.dll` vs `winmm.dll`) sin pisarse.
2. **Inyector genérico** (`LoadLibrary` remoto): útil en desarrollo/debug,
   pero rompe con juegos que tienen anticheat a nivel kernel — no
   recomendado para juegos con EAC/BattlEye.

No incluí código de inyección aquí a propósito: la elección depende de qué
juegos vas a usar y de si ya tienes un loader (el de OptiScaler, por
ejemplo, podría cargar esta DLL igual que carga la suya).

## Qué extendería primero

En este orden, por relación esfuerzo/beneficio:

1. **Filtrar el contador GPU % a solo `engtype_3D`.** Ahora mismo
   `GpuCounter::Sample()` suma *todas* las instancias de "GPU Engine"
   (3D, Copy, VideoDecode, etc.), lo que puede sobreestimar el % en juegos
   que usan decodificación de video o compute en paralelo. Filtrar por
   nombre de instancia (`*engtype_3D`) da un número mucho más fiel a lo que
   muestra el Administrador de Tareas.
2. **1% lows / 0.1% lows** (ya en tu roadmap v2): son baratos de calcular
   una vez que ya tienes `frameTimeHistory` — solo hace falta ordenar la
   ventana de muestras y tomar percentiles. Yo lo añadiría antes que temas
   visuales porque es la métrica que más le importa a quien usa este tipo
   de overlay.
3. **CPU MHz en tiempo real.** Ahora mismo mando el `~MHz` base del
   registro (no refleja boost). Cambiarlo a
   `\Processor Information(_Total)\% Processor Performance` multiplicado
   por el base clock da la frecuencia efectiva real y es solo tocar
   `metrics.cpp`.
4. **Robustecer el hook DX12 ante resize de ventana.** El código actual no
   libera/recrea los back buffers cuando el juego llama a `ResizeBuffers`;
   en resoluciones que cambian en caliente (alt-tab en ventana, cambio de
   resolución) hay que hookear también `ResizeBuffers` y reconstruir
   `g_backBuffers`/`g_rtvHandles`. Sin esto, cambiar de resolución puede
   crashear el overlay (no el juego, porque corre en su propio try/catch
   de comandos, pero conviene arreglarlo pronto).
5. **Temperaturas (v2 en tu documento).** Requieren NVAPI (NVIDIA) o ADL
   (AMD) — no hay forma universal sin SDK propietario. Alternativa más
   simple: enlazar contra `LibreHardwareMonitorLib` (maneja ambos vendors
   y es MIT), a costa de una dependencia más pesada.

## Notas de diseño

- **Por qué se hookean DX11 y DX12 a la vez:** ambos hooks se instalan
  siempre; el que no corresponde a la API real del juego simplemente nunca
  se dispara porque nunca se crea el objeto correspondiente. Evita tener
  que detectar la API de antemano.
- **Por qué no se usa `WndProc` para el hotkey:** `GetAsyncKeyState` desde
  el render thread evita subclasear la ventana solo para un toggle, lo cual
  reduce superficie de conflicto con el hook de input que pueda tener
  OptiScaler u otro overlay coexistiendo.
- **VRAM:** se usa `IDXGIAdapter3::QueryVideoMemoryInfo`, que da el
  "budget" del sistema operativo, no la VRAM física total de la tarjeta al
  100%. En la práctica coincide con la VRAM total en la inmensa mayoría de
  sistemas modernos.
