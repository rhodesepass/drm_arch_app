#pragma once
#include <apps/apps_types.h>
#include <stdint.h>
#include <stddef.h>

int apps_ipc_handler(apps_t *apps, uint8_t* rxbuf, size_t rxlen,uint8_t* txbuf, size_t txcap);
