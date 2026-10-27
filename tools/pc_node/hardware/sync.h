// Reemplazo de hardware/sync.h: en la PC, __wfi() espera el próximo tick
// o a que lleguen bytes por el puerto virtual (ver pc_stubs.c).
#pragma once
void __wfi(void);
