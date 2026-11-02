#include "wad_mem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"

#define WAD_MEM_MAX_FDS 4
#define WAD_MEM_CHUNK (64 * 1024)

static const char *TAG = "wad_mem";

static uint8_t *s_data;
static size_t s_size;
static char s_name[32];
static bool s_used[WAD_MEM_MAX_FDS];
static off_t s_pos[WAD_MEM_MAX_FDS];

static bool valid_fd(int fd)
{
    return fd >= 0 && fd < WAD_MEM_MAX_FDS && s_used[fd];
}

static int wad_mem_open(const char *path, int flags, int mode)
{
    (void)mode;
    if (strcmp(path, s_name) != 0) {
        errno = ENOENT;
        return -1;
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
        errno = EROFS;
        return -1;
    }
    for (int fd = 0; fd < WAD_MEM_MAX_FDS; fd++) {
        if (!s_used[fd]) {
            s_used[fd] = true;
            s_pos[fd] = 0;
            return fd;
        }
    }
    errno = ENFILE;
    return -1;
}

static ssize_t wad_mem_read(int fd, void *dst, size_t size)
{
    if (!valid_fd(fd)) {
        errno = EBADF;
        return -1;
    }
    size_t left = s_pos[fd] < (off_t)s_size ? s_size - (size_t)s_pos[fd] : 0;
    if (size > left) {
        size = left;
    }
    memcpy(dst, s_data + s_pos[fd], size);
    s_pos[fd] += size;
    return (ssize_t)size;
}

static off_t wad_mem_lseek(int fd, off_t offset, int whence)
{
    if (!valid_fd(fd)) {
        errno = EBADF;
        return -1;
    }
    off_t base;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = s_pos[fd];
        break;
    case SEEK_END:
        base = (off_t)s_size;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (base + offset < 0) {
        errno = EINVAL;
        return -1;
    }
    s_pos[fd] = base + offset;
    return s_pos[fd];
}

static int wad_mem_close(int fd)
{
    if (!valid_fd(fd)) {
        errno = EBADF;
        return -1;
    }
    s_used[fd] = false;
    return 0;
}

static int wad_mem_fstat(int fd, struct stat *st)
{
    if (!valid_fd(fd)) {
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = (off_t)s_size;
    return 0;
}

static const esp_vfs_fs_ops_t s_ops = {
    .open = wad_mem_open,
    .read = wad_mem_read,
    .lseek = wad_mem_lseek,
    .close = wad_mem_close,
    .fstat = wad_mem_fstat,
};

esp_err_t wad_mem_load(const char *src_path, const char *name)
{
    int64_t t0 = esp_timer_get_time();
    int src = open(src_path, O_RDONLY);
    if (src < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    struct stat st;
    if (fstat(src, &st) != 0 || st.st_size <= 0) {
        close(src);
        return ESP_FAIL;
    }

    uint8_t *data = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        close(src);
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        size_t want = (size_t)st.st_size - got;
        ssize_t n = read(src, data + got, want < WAD_MEM_CHUNK ? want : WAD_MEM_CHUNK);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(src);
    if (got != (size_t)st.st_size) {
        ESP_LOGE(TAG, "Short read of %s: %u/%u bytes", src_path, (unsigned)got, (unsigned)st.st_size);
        free(data);
        return ESP_FAIL;
    }

    s_data = data;
    s_size = got;
    snprintf(s_name, sizeof(s_name), "/%s", name);
    esp_err_t err = esp_vfs_register_fs(WAD_MEM_BASE, &s_ops, ESP_VFS_FLAG_STATIC | ESP_VFS_FLAG_READONLY_FS, NULL);
    if (err != ESP_OK) {
        free(data);
        s_data = NULL;
        s_size = 0;
        return err;
    }
    ESP_LOGI(TAG, "%s copied to PSRAM (%u bytes) in %lld ms", src_path, (unsigned)s_size,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}
