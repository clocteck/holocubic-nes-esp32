#ifndef DEBUG_H
#define DEBUG_H

#include "config.h"
#include "nes_port.h"

#ifdef DEBUG
  #define LOG(msg)        nes_port_log(msg)
  #define LOGF(fmt, ...)  nes_port_logf(fmt, ##__VA_ARGS__)
#else
  #define LOG(msg)
  #define LOGF(fmt, ...)
#endif

#endif
