#pragma once
#define FW_VERSION "1.11.0"
#if defined(BOARD_175)
#define DEVICE_ID "pana-amoled175"
// redonda 466x466: una página a la vez (el avatar · consola · grafo), se cambia deslizando (ui175.cpp)
#define SCR_W 466
#define SCR_H 466
#elif defined(BOARD_P4)
#define DEVICE_ID "pana-p4-4c"
// redonda 720x720, esfera viva con páginas de homelab, agenda y redes, y grafo (ui720.cpp)
#define SCR_W 720
#define SCR_H 720
#endif

#define PTT_MS 300                     // mantener la cara este tiempo = hablar
#define FRAME_MS 33                    // ~30 fps
