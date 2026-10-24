#ifndef OBB_H
#define OBB_H

#include <stdint.h>
#include <stddef.h>

#define OBB_ROOT "UnrealEngine3/UDKGame/"

int obb_open(const char *path);
int obb_find(const char *name, uint32_t *offset, uint32_t *size);
void *obb_load(const char *name, uint32_t *size);
int obb_read(uint32_t offset, void *buf, uint32_t size);
const char *obb_name_at(uint32_t off, uint32_t len);

#endif
