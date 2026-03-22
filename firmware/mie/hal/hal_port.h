#pragma once
// hal/hal_port.h — redirects to the canonical public header.
// The IHalPort interface is part of the public MIE API and lives at:
//   include/mie/hal_port.h
// This shim allows internal hal/ code that still uses the old relative path to
// compile without changes.
#include <mie/hal_port.h>
