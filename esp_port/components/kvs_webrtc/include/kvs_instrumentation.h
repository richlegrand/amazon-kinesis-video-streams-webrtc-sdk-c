/**
 * Periodic transport instrumentation, off unless
 * CONFIG_KVS_TRANSPORT_INSTRUMENTATION is set.
 *
 * The reports cost real time -- a timestamp per packet on the DTLS write path
 * and per send on the ICE path -- so they compile out entirely rather than
 * being merely silent. Genuine warnings and errors are not gated by this and
 * always print.
 */
#ifndef KVS_INSTRUMENTATION_H
#define KVS_INSTRUMENTATION_H

#include "sdkconfig.h"

#if defined(CONFIG_KVS_TRANSPORT_INSTRUMENTATION)
#define KVS_INSTR 1
#include "esp_log.h"
#define KVS_INSTR_LOGW(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#else
#define KVS_INSTR 0
#define KVS_INSTR_LOGW(tag, ...)                                                                                                                     \
    do {                                                                                                                                             \
    } while (0)
#endif

#endif /* KVS_INSTRUMENTATION_H */
