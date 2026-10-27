#include "service_photo.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "lisa_log.h"
#include "lsfs.h"
#include "sysheap.h"
#include "tiny_jpeg.h"
#include "jpeglib.h"

#define TAG "nano.photo"

#define PHOTO_FILE_MAX  (256U * 1024U)
#define PHOTO_PIX_MAX   (240U * 180U * 4U)
#define PHOTO_PATH_MAX  64

/* ---------- 路径与命名 ---------- */

static char photo_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool name_is_jpg(const char *n)
{
    size_t l = strlen(n);

    if (l >= PHOTO_NAME_LEN) {
        return false;
    }
    if (l > 5 && photo_lower(n[l - 5]) == '.' && photo_lower(n[l - 4]) == 'j' &&
        photo_lower(n[l - 3]) == 'p' && photo_lower(n[l - 2]) == 'e' && photo_lower(n[l - 1]) == 'g') {
        return true;
    }
    return l > 4 && photo_lower(n[l - 4]) == '.' && photo_lower(n[l - 3]) == 'j' &&
           photo_lower(n[l - 2]) == 'p' && photo_lower(n[l - 1]) == 'g';
}

/* IMG_0001.JPG → 1；其它文件名按 -1 处理（没有序号，不参与 next 计算）。 */
static int photo_index_of(const char *name)
{
    int v = 0;

    if (strncmp(name, "IMG_", 4) != 0) {
        return -1;
    }
    for (int i = 4; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return -1;
        }
        v = v * 10 + (name[i] - '0');
    }
    return v;
}

static void photo_path_of(char *out, int out_len, const char *name)
{
    snprintf(out, out_len, "%s%s", PHOTO_ROOT, name);
}

/* ---------- 目录扫描：同时给出下一序号与最新若干张 ---------- */

struct photo_scan {
    photo_entry_t *items;
    int max;
    int count;
    int next_index;
};

static void photo_scan_push(struct photo_scan *sc, const char *name, uint32_t size, int key)
{
    int at = sc->count;

    if (sc->max <= 0) {
        return;
    }
    /* items 按 key 降序保持：找插入位。满了就只可能挤掉最后一个。 */
    while (at > 0 && photo_index_of(sc->items[at - 1].name) < key) {
        at--;
    }
    if (sc->count >= sc->max) {
        if (at >= sc->count) {
            return;
        }
        sc->count--;
    }
    for (int i = sc->count; i > at; i--) {
        sc->items[i] = sc->items[i - 1];
    }
    memset(&sc->items[at], 0, sizeof(sc->items[at]));
    strncpy(sc->items[at].name, name, PHOTO_NAME_LEN - 1);
    sc->items[at].size = size;
    sc->count++;
}

static void photo_scan_run(struct photo_scan *sc)
{
    struct lsfs_dir_t dir;
    struct lsfs_dirent entry;

    sc->count = 0;
    sc->next_index = 1;

    lsfs_dir_t_init(&dir);
    if (lsfs_opendir(&dir, PHOTO_ROOT) != 0) {
        return;
    }
    while (lsfs_readdir(&dir, &entry) == 0) {
        if (entry.name[0] == '\0' || (uint8_t)entry.name[0] == 0xE5) {
            if (entry.name[0] == '\0') {
                break;
            }
            continue;
        }
        if (entry.type != LSFS_DIR_ENTRY_FILE || !name_is_jpg(entry.name)) {
            continue;
        }

        int idx = photo_index_of(entry.name);
        if (idx >= 0 && idx + 1 > sc->next_index) {
            sc->next_index = idx + 1;
        }
        photo_scan_push(sc, entry.name, (uint32_t)entry.size, idx);
    }
    lsfs_closedir(&dir);
}

bool service_photo_card_ready(void)
{
    struct lsfs_dir_t dir;
    bool ok;

    lsfs_dir_t_init(&dir);
    ok = (lsfs_opendir(&dir, PHOTO_ROOT) == 0);
    if (ok) {
        lsfs_closedir(&dir);
    }
    return ok;
}

int service_photo_list(photo_entry_t *items, int max)
{
    struct photo_scan sc = { .items = items, .max = max };

    if (!items || max <= 0) {
        return 0;
    }
    photo_scan_run(&sc);
    return sc.count;
}

/* ---------- 编码写盘：tiny_jpeg 的输出直接喂进 lsfs_write ---------- */

struct photo_sink {
    struct lsfs_file_t *fp;
    uint32_t wrote;
    int err;
};

static void photo_tje_write(void *ctx, void *data, int size)
{
    struct photo_sink *s = (struct photo_sink *)ctx;
    long n;

    if (s->err != 0 || size <= 0) {
        return;
    }
    n = (long)lsfs_write(s->fp, data, (size_t)size);
    if (n != (long)size) {
        s->err = -1;
    } else {
        s->wrote += (uint32_t)size;
    }
}

static void rgb565_to_888(const uint16_t *src, uint8_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint16_t p = src[i];

        dst[i * 3 + 0] = (uint8_t)(((((p >> 11) & 0x1FU) * 255U) + 15U) / 31U);
        dst[i * 3 + 1] = (uint8_t)(((((p >> 5) & 0x3FU) * 255U) + 31U) / 63U);
        dst[i * 3 + 2] = (uint8_t)(((((p & 0x1FU) * 255U) + 15U) / 31U));
    }
}

int service_photo_save(const uint16_t *rgb565, uint16_t w, uint16_t h, int quality,
                       char *name_out, int name_out_len, uint32_t *out_bytes)
{
    struct photo_scan sc = { .max = 0 };
    struct lsfs_file_t fp;
    struct photo_sink sink = { 0 };
    char path[PHOTO_PATH_MAX];
    char name[PHOTO_NAME_LEN];
    uint8_t *rgb888;
    size_t npix = (size_t)w * (size_t)h;
    int ret = -1;

    if (!rgb565 || w == 0 || h == 0) {
        return -1;
    }
    if (quality < 1 || quality > 3) {
        quality = 2;
    }

    /* 走一遍目录：既拿到下一序号，也顺便确认卡能读目录。 */
    photo_scan_run(&sc);
    if (sc.next_index <= 0) {
        sc.next_index = 1;
    }
    snprintf(name, sizeof(name), "IMG_%04d.JPG", sc.next_index);
    photo_path_of(path, sizeof(path), name);

    rgb888 = psram_malloc(npix * 3U);
    if (!rgb888) {
        LISA_LOGE(TAG, "RGB888 缓冲不足（需 %u B）", (unsigned)(npix * 3U));
        return -1;
    }
    rgb565_to_888(rgb565, rgb888, (uint32_t)npix);

    lsfs_file_t_init(&fp);
    if (lsfs_open(&fp, path, LSFS_O_WRITE | LSFS_O_CREATE | LSFS_O_TRUNC) != 0) {
        LISA_LOGE(TAG, "打开 %s 失败", path);
        psram_free(rgb888);
        return -1;
    }
    sink.fp = &fp;

    if (tje_encode_with_func(photo_tje_write, &sink, quality, (int)w, (int)h, 3, rgb888) == 1 &&
        sink.err == 0) {
        lsfs_close(&fp);
        if (name_out && name_out_len > 0) {
            strncpy(name_out, name, (size_t)name_out_len - 1);
            name_out[name_out_len - 1] = '\0';
        }
        if (out_bytes) {
            *out_bytes = sink.wrote;
        }
        LISA_LOGI(TAG, "已存 %s %u B (q=%d)", name, sink.wrote, quality);
        ret = 0;
    } else {
        lsfs_close(&fp);
        /* 半张图比没有图更糟：删掉。 */
        lsfs_unlink(path);
        LISA_LOGE(TAG, "编码或写入失败，已删除 %s", name);
    }

    psram_free(rgb888);
    return ret;
}

/* ---------- 解码显示：IJG → RGB565 → 等比放进目标矩形 ---------- */

struct photo_jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void photo_jerr_exit(j_common_ptr cinfo)
{
    struct photo_jerr *e = (struct photo_jerr *)cinfo->err;

    longjmp(e->jump, 1);
}

static void photo_jerr_emit(j_common_ptr cinfo, int msg_level)
{
    (void)cinfo;
    (void)msg_level;
}

static uint16_t pack565(uint32_t r8, uint32_t g8, uint32_t b8)
{
    return (uint16_t)((((r8 >> 3) & 0x1FU) << 11) | (((g8 >> 2) & 0x3FU) << 5) | ((b8 >> 3) & 0x1FU));
}

/* 采样位置：把目标格 i 映射成源下标 + 8bit 小数。32 位放不下 (2i+1)*n*65536，
 * 所以这里只在每条轴上算一次 64 位除法，逐像素只用查表结果。 */
static void photo_axis_map(uint16_t n_src, uint16_t n_dst, uint32_t *map)
{
    for (uint32_t i = 0; i < n_dst; i++) {
        uint32_t idx, frac;

        if (n_src <= 1U) {
            map[i] = 0U;
            continue;
        }
        /* 中心对齐采样：q = (i + 0.5) * n_src / n_dst，16.16 定点再减半像素 */
        {
            uint64_t q = (uint64_t)(2U * i + 1U) * (uint64_t)n_src * 65536ULL;
            int64_t s = (int64_t)(q / (2ULL * (uint64_t)n_dst)) - 32768;

            if (s < 0) {
                s = 0;
            }
            idx = (uint32_t)s >> 16;
            frac = ((uint32_t)s >> 8) & 0xFFU;
        }
        if (idx + 1U >= n_src) {
            idx = n_src - 2U;
            frac = 0xFFU;
        }
        map[i] = (idx << 8) | frac;
    }
}

/* 双线性等比缩放：源图放进 dst 中间，四周留黑。 */
static void photo_fit(const uint16_t *src, uint16_t sw, uint16_t sh,
                      uint16_t *dst, uint16_t dw, uint16_t dh)
{
    uint16_t rw, rh, ox, oy;
    uint32_t *maps;

    if ((uint32_t)sw * dh >= (uint32_t)sh * dw) {
        rw = dw;
        rh = (uint16_t)((uint32_t)sh * dw / sw);
    } else {
        rh = dh;
        rw = (uint16_t)((uint32_t)sw * dh / sh);
    }
    if (rw == 0U) {
        rw = 1U;
    }
    if (rh == 0U) {
        rh = 1U;
    }
    ox = (uint16_t)((dw - rw) / 2U);
    oy = (uint16_t)((dh - rh) / 2U);

    maps = psram_malloc((size_t)(rw + rh) * sizeof(uint32_t));
    if (!maps) {
        return;
    }
    photo_axis_map(sw, rw, maps);
    photo_axis_map(sh, rh, maps + rw);

    /* 先清干净：外来图比例不合时，留黑的部分不能是上一张的残留。 */
    for (uint32_t y = 0; y < dh; y++) {
        uint16_t *line = dst + (size_t)y * dw;
        for (uint32_t x = 0; x < dw; x++) {
            line[x] = 0;
        }
    }

    for (uint32_t y = 0; y < rh; y++) {
        uint32_t ym = maps[rw + y];
        uint32_t y0 = ym >> 8;
        uint32_t wy = ym & 0xFFU;
        const uint16_t *r0 = src + (size_t)y0 * sw;
        const uint16_t *r1 = (y0 + 1U < sh) ? (r0 + sw) : r0;
        uint16_t *out = dst + (size_t)(oy + y) * dw + ox;

        for (uint32_t x = 0; x < rw; x++) {
            uint32_t xm = maps[x];
            uint32_t x0 = xm >> 8;
            uint32_t wx = xm & 0xFFU;
            uint32_t a = r0[x0];
            uint32_t b = (x0 + 1U < sw) ? r0[x0 + 1U] : a;
            uint32_t c = r1[x0];
            uint32_t d = (x0 + 1U < sw) ? r1[x0 + 1U] : c;
            uint32_t w00 = (256U - wx) * (256U - wy);
            uint32_t w01 = wx * (256U - wy);
            uint32_t w10 = (256U - wx) * wy;
            uint32_t w11 = wx * wy;
            uint32_t r = (((a >> 11) & 0x1FU) * w00 + ((b >> 11) & 0x1FU) * w01 +
                          ((c >> 11) & 0x1FU) * w10 + ((d >> 11) & 0x1FU) * w11) >> 16;
            uint32_t g = (((a >> 5) & 0x3FU) * w00 + ((b >> 5) & 0x3FU) * w01 +
                          ((c >> 5) & 0x3FU) * w10 + ((d >> 5) & 0x3FU) * w11) >> 16;
            uint32_t bl = ((a & 0x1FU) * w00 + (b & 0x1FU) * w01 +
                           (c & 0x1FU) * w10 + (d & 0x1FU) * w11) >> 16;

            out[x] = (uint16_t)((r << 11) | (g << 5) | bl);
        }
    }
    psram_free(maps);
}

int service_photo_render(const char *name, uint16_t *dst, uint16_t dst_w, uint16_t dst_h)
{
    struct jpeg_decompress_struct ci;
    struct photo_jerr jerr;
    struct lsfs_dirent st;
    struct lsfs_file_t fp;
    char path[PHOTO_PATH_MAX];
    uint8_t *jpg = NULL;
    uint16_t *rgb = NULL;
    uint8_t *row = NULL;
    size_t npix = 0;
    uint16_t out_w = 0;
    uint16_t out_h = 0;
    bool created = false;
    bool ok = false;

    if (!name || !dst || dst_w == 0 || dst_h == 0) {
        return -1;
    }
    photo_path_of(path, sizeof(path), name);
    if (lsfs_stat(path, &st) != 0 || st.size == 0 || (size_t)st.size > PHOTO_FILE_MAX) {
        return -1;
    }

    jpg = psram_malloc((size_t)st.size);
    if (!jpg) {
        return -1;
    }
    lsfs_file_t_init(&fp);
    if (lsfs_open(&fp, path, LSFS_O_READ) != 0) {
        psram_free(jpg);
        return -1;
    }
    if ((size_t)lsfs_read(&fp, jpg, (size_t)st.size) != (size_t)st.size) {
        lsfs_close(&fp);
        psram_free(jpg);
        return -1;
    }
    lsfs_close(&fp);

    ci.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = photo_jerr_exit;
    jerr.pub.emit_message = photo_jerr_emit;
    if (setjmp(jerr.jump)) {
        goto done;
    }

    jpeg_create_decompress(&ci);
    created = true;
    jpeg_mem_src(&ci, jpg, (size_t)st.size);
    (void)jpeg_read_header(&ci, TRUE);
    ci.out_color_space = JCS_RGB;
    ci.dct_method = JDCT_ISLOW;
    ci.do_fancy_upsampling = FALSE;
    (void)jpeg_start_decompress(&ci);

    npix = (size_t)ci.output_width * (size_t)ci.output_height;
    if (ci.output_width == 0 || ci.output_height == 0 || npix > PHOTO_PIX_MAX ||
        ci.output_components != 3) {
        goto done;
    }
    out_w = (uint16_t)ci.output_width;
    out_h = (uint16_t)ci.output_height;
    rgb = psram_malloc(npix * 2U);
    row = psram_malloc((size_t)ci.output_width * 3U);
    if (!rgb || !row) {
        goto done;
    }

    while (ci.output_scanline < ci.output_height) {
        JSAMPROW rp = row;

        (void)jpeg_read_scanlines(&ci, &rp, 1);
        /* jpeg_read_scanlines() 返回后 output_scanline 已经指向下一行。 */
        uint32_t base = ((uint32_t)ci.output_scanline - 1U) * (uint32_t)out_w;
        for (uint32_t x = 0; x < out_w; x++) {
            rgb[base + x] = pack565(rp[x * 3], rp[x * 3 + 1], rp[x * 3 + 2]);
        }
    }
    (void)jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    created = false;

    photo_fit(rgb, out_w, out_h, dst, dst_w, dst_h);
    ok = true;

done:
    if (created) {
        (void)jpeg_destroy_decompress(&ci);
    }
    if (jpg) {
        psram_free(jpg);
    }
    if (rgb) {
        psram_free(rgb);
    }
    if (row) {
        psram_free(row);
    }
    return ok ? 0 : -1;
}

int service_photo_read_jpeg(const char *name, uint8_t **data_out, uint32_t *size_out)
{
    struct lsfs_dirent st;
    struct lsfs_file_t fp;
    char path[PHOTO_PATH_MAX];
    uint8_t *data;

    if (!name || !data_out || !size_out) {
        return -1;
    }
    *data_out = NULL;
    *size_out = 0;
    photo_path_of(path, sizeof(path), name);
    if (lsfs_stat(path, &st) != 0 || st.size == 0 || (size_t)st.size > PHOTO_FILE_MAX) {
        return -1;
    }
    data = psram_malloc((size_t)st.size);
    if (!data) {
        return -1;
    }
    lsfs_file_t_init(&fp);
    if (lsfs_open(&fp, path, LSFS_O_READ) != 0) {
        psram_free(data);
        return -1;
    }
    if ((size_t)lsfs_read(&fp, data, (size_t)st.size) != (size_t)st.size) {
        lsfs_close(&fp);
        psram_free(data);
        return -1;
    }
    lsfs_close(&fp);
    *data_out = data;
    *size_out = (uint32_t)st.size;
    return 0;
}
