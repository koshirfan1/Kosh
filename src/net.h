#ifndef KHAN_NET_H
#define KHAN_NET_H

#include "interpreter.h"

// Register network/HTTP functions into the global environment.
void net_register_all(Environment *env);

#endif