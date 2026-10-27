#pragma once
// Audio de la placa: ES8311 (DAC + ADC) por I2S a 16 kHz y amplificador FM8002E.
// Reproducción: la red mete PCM con audioPush(); una tarea lo saca al altavoz y calcula
// el nivel de la boca. Micrófono: con audioMic(true) la tarea de captura deja frames de
// 1024 muestras en un ring buffer que la red vacía con audioMicPop().
#include <Arduino.h>

bool audioBegin();
void audioOpen();                          // empieza un say con audio
void audioPush(const uint8_t *pcm, size_t n);
void audioClose();                         // llegó audio_end: suena lo que queda y para
bool audioPlaying();
float audioMouth();                        // 0 / 0.5 / 1 según el RMS de lo que suena ahora
void audioMic(bool on);
size_t audioMicPop(uint8_t *dst, size_t max);
void audioBeep();
void audioChirp(bool up);                  // pitido de micro: sube al abrir, baja al cerrar
void audioMicTest();                       // serie m: nivel de cada canal durante 1 s
