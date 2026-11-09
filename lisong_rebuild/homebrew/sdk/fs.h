/* ACTOS drive / FAT file-system calls (see fs.s). Names are 8.3 as returned by fs_dir_*. */
#ifndef FS_H
#define FS_H
#include <stdint.h>
uint8_t  drv_active_ud(uint8_t ud, uint8_t mode);
uint8_t  drv_active_drive(char letter, uint8_t mode);
uint8_t  fs_init(void);
uint8_t  fs_cd(const char *name);                 /* ":" = root, "\\" = parent */
uint8_t  fs_dir_first(char *buf, const char *ext3);
uint8_t  fs_dir_next(char *buf, const char *ext3);
uint16_t fs_fopen(const char *name);              /* 0 = failed */
uint8_t  fs_fread(void *buf, uint16_t handle);    /* one 512-byte sector, 0 = failed */
void     fs_fclose(uint16_t handle);
#endif
