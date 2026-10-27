#pragma once
#include <Arduino.h>

void netBegin();                    // arranca la tarea de red (núcleo 0)
void netSend(const String &json);   // encola un mensaje para el servidor (cualquier tarea)
void netMicStart();                 // push-to-talk
void netMicStop();
void netSetHost(const String &host);   // se guardan en NVS; se aplican al reiniciar
void netSetPort(uint16_t port);
void netResetWifi();                // borra la WiFi guardada y abre el portal
