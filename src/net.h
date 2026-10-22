#ifndef KOSH_NET_H
#define KOSH_NET_H

#include "interpreter.h"

// Register network/HTTP functions into the global environment.
void net_register_all(Environment *env);

#endif