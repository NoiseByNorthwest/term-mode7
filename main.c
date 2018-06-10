#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <time.h> 

#include <unistd.h>

#include <ncurses.h>

#if HAVE_SIXEL
#include <sixel.h>

static const size_t pixbuf_width = 640;
static const size_t pixbuf_height = 360;
unsigned char pixbuf[pixbuf_width * pixbuf_height];
static const size_t default_mipmap_count_sixel = 1;
static const size_t default_mipmap_count_character = 5;
#endif /* HAVE_SIXEL */

static void terminate_ncurses(void);
static void restore_colors(void);

typedef struct {
    size_t width;
    size_t height;
    uint8_t * data;
    uint8_t colors[256][4];
} image_t;

static image_t * image_create(const char * file_name);
static void image_quantize(image_t * image, size_t max_color_count);
static image_t * image_create_downsized_copy(const image_t * image, size_t w, size_t h);
static void image_destroy(image_t * image);

typedef struct {
    image_t * image;
    size_t ratio;
} texture_mimap_t;

typedef struct {
    texture_mimap_t mipmaps[8];
    size_t mipmap_count;
} texture_t;

static texture_t * texture_create(const char * file_name, size_t max_color_count, size_t mipmap_count);
static void texture_destroy(texture_t * texture);

typedef struct {
    float nums[3][3];
} mat3_t;

typedef struct {
    float x, y;
} vec2_t;

static void mat3_identity(mat3_t * m);
static void mat3_copy(mat3_t * a, const mat3_t * b);
static void mat3_mult(mat3_t * a, const mat3_t * b);
static void mat3_translate(mat3_t * m, float x, float y);
static void mat3_scale(mat3_t * m, float x, float y);
static void mat3_rotate(mat3_t * m, float x);
static void mat3_transform(const mat3_t * m, vec2_t * v);

static float wrap_repeat(float v, float min, float max);

#if HAVE_SIXEL
static void renderersixel_init(uint8_t colors[][4]);
static void renderersixel_draw(size_t x, size_t y, uint8_t colors[][4], uint8_t color_idx);
#endif /* HAVE_SIXEL */
static void renderer256_init(uint8_t colors[][4]);
static void renderer256_draw(size_t x, size_t y, uint8_t colors[][4], uint8_t color_idx);
static void renderer16_init(uint8_t colors[][4]);
static void renderer16_draw(size_t x, size_t y, uint8_t colors[][4], uint8_t color_idx);
static void renderer1_init(uint8_t colors[][4]);
static void renderer1_draw(size_t x, size_t y, uint8_t colors[][4], uint8_t color_idx);

static size_t current_time_ns(void);

#define KB_EVENT_RELEASE 0x8000
static int kb_event_get();

typedef struct {
    float acceleration;
    float deceleration;
    float max;

    size_t last_time_ms;
    float velocity;

    int active;
    int reverse;
} accelerator_t;

static void accelerator_init(accelerator_t * accelerator, float acceleration, float deceleration, float max);
static void accelerator_press(accelerator_t * accelerator, int reverse);
static void accelerator_release(accelerator_t * accelerator);
static float accelerator_step_distance(accelerator_t * accelerator);
static float accelerator_velocity(const accelerator_t * accelerator);

#if HAVE_SIXEL
static int
sixel_write(char *data, int size, void *priv)
{
    return fwrite(data, 1, size, (FILE *)priv);
}

static SIXELSTATUS
output_sixel(uint8_t colors[][4])
{
    sixel_output_t *output = NULL;
    sixel_dither_t *dither = NULL;
    unsigned char palette[256 * 3];
    int i;
    SIXELSTATUS status;

    status = sixel_output_new(&output, sixel_write, stdout, NULL);
    if (SIXEL_FAILED(status))
        goto end;
    status = sixel_dither_new(&dither, 256, NULL);
    if (SIXEL_FAILED(status))
        goto end;

    sixel_dither_set_pixelformat(dither, SIXEL_PIXELFORMAT_PAL8);
    if (SIXEL_FAILED(status))
        goto end;
    for (i = 0; i < 256; ++i) {
       palette[i * 3 + 0] = colors[i][0];
       palette[i * 3 + 1] = colors[i][1];
       palette[i * 3 + 2] = colors[i][2];
    }
    sixel_dither_set_palette(dither, (unsigned char *)palette);
    if (SIXEL_FAILED(status))
        goto end;
    printf("\033[H");
    status = sixel_encode(
        pixbuf, pixbuf_width, pixbuf_height,
        SIXEL_PIXELFORMAT_PAL8, dither, output);
    if (SIXEL_FAILED(status))
        goto end;
end:
    sixel_output_unref(output);
    sixel_dither_unref(dither);

    return status;
}
#endif /* HAVE_SIXEL */


int main()
{
    const struct {
        const char * file_name;
        size_t default_color_count;
        size_t padding_box_size;
        vec2_t padding_box_pos;
    } maps[] = {
        {"assets/maps/mariocircuit-1.bmp", 15, 8, {0, 1016}},
        {"assets/maps/ghostvalley-3.bmp", 12, 8, {0, 0}},
        {"assets/maps/bowsercastle-3.bmp", 8, 8, {32, 40}},
        {"assets/maps/chocoisland-2.bmp", 10, 8, {384, 192}},
        {"assets/maps/mariocircuit-3.bmp", 15, 8, {472, 0}},
        {"assets/maps/donutplains-3.bmp", 13, 8, {448, 896}},
        {"assets/maps/koopabeach-1.bmp", 9, 8, {0, 0}},
        {"assets/maps/vanillalake-2.bmp", 22, 8, {0, 0}}
    };

    const size_t map_count = sizeof(maps) / sizeof(maps[0]);
    size_t current_map = 0;

    size_t color_count = maps[current_map].default_color_count;
#if HAVE_SIXEL
    size_t mipmap_count = default_mipmap_count_sixel;
#else
    size_t mipmap_count = default_mipmap_count_character;
#endif /* HAVE_SIXEL */
    texture_t * texture = texture_create(
        maps[current_map].file_name,
        color_count,
        mipmap_count
    );

    if (!texture) {
        fprintf(stderr, "Cannot read image: %s\n", maps[current_map].file_name);
        exit(1);
    }

    initscr();
    atexit(terminate_ncurses);

    nodelay(stdscr, 1);
    keypad(stdscr, 1);
    curs_set(0);
    noecho();
    start_color();
    restore_colors();

    static struct {
        const char * name;
        void (*init)(uint8_t [][4]);
        void (*draw)(size_t, size_t, uint8_t [][4], uint8_t);
    } renderers[] = {
        {"monochrome", renderer1_init, renderer1_draw},
        {"16 colors", renderer16_init, renderer16_draw},
        {"256 colors", renderer256_init, renderer256_draw},
#if HAVE_SIXEL
        {"sixel", renderersixel_init, renderersixel_draw},
#endif /* HAVE_SIXEL */
    };

    const int true_color_support = can_change_color() && COLORS >= 256;
#if HAVE_SIXEL
    const size_t renderer_count = true_color_support ? 4 : 3;
#else
    const size_t renderer_count = true_color_support ? 3 : 2;
#endif /* HAVE_SIXEL */
    size_t current_renderer = renderer_count - 1;

    renderers[current_renderer].init(texture->mipmaps[0].image->colors);

    const vec2_t default_position = {860, 758};
    vec2_t position = default_position;
    /* fix broken ratio since pixels are not square */
    const vec2_t default_scale = {1 * 0.08, 1.8 * 0.08};
    vec2_t scale = default_scale;

    float orientation = 0;
    int perspective = 1;
    size_t rendered_frame_count = 0;

    accelerator_t move_accelerator;
    accelerator_init(&move_accelerator, 600, 150, 150);

    accelerator_t turn_accelerator;
    accelerator_init(&turn_accelerator, M_PI * 0.3, M_PI * 8, M_PI * 0.8);

    int stop = 0;
    while (!stop) {
        usleep(5 * 1000);

        const int evt = kb_event_get();
        switch (evt) {
            case 'q':
                stop = 1;
                break;

            case KEY_UP:
                accelerator_press(&move_accelerator, 0);
                break;

            case KEY_UP | KB_EVENT_RELEASE:
                accelerator_release(&move_accelerator);
                break;

            case KEY_DOWN:
                accelerator_press(&move_accelerator, 1);
                break;

            case KEY_DOWN | KB_EVENT_RELEASE:
                accelerator_release(&move_accelerator);
                break;

            case KEY_LEFT:
                accelerator_press(&turn_accelerator, 1);
                break;

            case KEY_LEFT | KB_EVENT_RELEASE:
                accelerator_release(&turn_accelerator);
                break;

            case KEY_RIGHT:
                accelerator_press(&turn_accelerator, 0);
                break;

            case KEY_RIGHT | KB_EVENT_RELEASE:
                accelerator_release(&turn_accelerator);
                break;

            case 'e':
                position.x -= 5 * cosf(orientation);
                position.y -= 5 * sinf(orientation);
                break;

            case 'r':
                position.x += 5 * cosf(orientation);
                position.y += 5 * sinf(orientation);
                break;

            case 'v':
                scale.x *= 1.1;
                scale.y *= 1.1;
                break;

            case 'b':
                scale.x /= 1.1;
                scale.y /= 1.1;
                break;

            case 'c':
                position = default_position;
                scale = default_scale;
                orientation = 0;
                break;

            case 'p':
                perspective = !perspective;
                break;

            case 'g':
                current_renderer++;
                if (current_renderer >= renderer_count) {
                    current_renderer = 0;
                }

                restore_colors();
                renderers[current_renderer].init(texture->mipmaps[0].image->colors);

                break;

            case 'h':
            case 'j':
            case 'k':
            case 'l':
            case 'm':
                if (evt == 'h') {
                    mipmap_count--;
                }

                if (evt == 'j') {
                    mipmap_count++;
                }

                if (mipmap_count < 1) {
                    mipmap_count = 1;
                }

                if (mipmap_count > 8) {
                    mipmap_count = 8;
                }

                if (evt == 'k') {
                    color_count--;
                }

                if (evt == 'l') {
                    color_count++;
                }

                if (color_count < 2) {
                    color_count = 2;
                }

                if (color_count > 256) {
                    color_count = 256;
                }

                if (evt == 'm') {
                    current_map++;
                    if (current_map >= map_count) {
                        current_map = 0;
                    }

                    color_count = maps[current_map].default_color_count;
                    mipmap_count = default_mipmap_count_character;
#if HAVE_SIXEL
                    if (current_renderer == 3) { /* sixel */
                        mipmap_count = default_mipmap_count_sixel;
                    }
#endif /* HAVE_SIXEL */
                }

                texture_destroy(texture);
                texture = texture_create(maps[current_map].file_name, color_count, mipmap_count);
                if (!texture) {
                    fprintf(stderr, "Cannot read image: %s\n", maps[current_map].file_name);
                    exit(1);
                }

                renderers[current_renderer].init(texture->mipmaps[0].image->colors);

                break;
        }

        const float move_distance = accelerator_step_distance(&move_accelerator);
        position.y -= move_distance * cosf(orientation);
        position.x += move_distance * sinf(orientation);

        orientation += accelerator_step_distance(&turn_accelerator);

        int scr_w, scr_h;
#if HAVE_SIXEL
        if (current_renderer == 3) { /* sixel */
            scr_h = pixbuf_height;
            scr_w = pixbuf_width;
        } else {
#endif /* HAVE_SIXEL */
            getmaxyx(stdscr, scr_h, scr_w);
            scr_w -= 1;
            scr_h -= 2;
#if HAVE_SIXEL
        }
#endif /* HAVE_SIXEL */

        const vec2_t center = {scr_w / 2.f, scr_h * 0.8};
        size_t rendered_pixel_count = 0;
        int i, j;

        for (i = 0; i < scr_h; i++) {
            mat3_t view_mat;
            mat3_identity(&view_mat);
            mat3_translate(&view_mat, position.x, position.y);
            mat3_translate(&view_mat, center.x, center.y);
            mat3_rotate(&view_mat, orientation);

            /*
             * This formula should be rewrote, simplified and parametrized (fov, perspective angle)
             */
            vec2_t perspective_factor = {
                (scr_w / (i + 1.f)),
                (((i + 1.f) / scr_h) + 3 * scr_w / scr_h)
                    / ((i + 1.f) / scr_h)
            };

            if (!perspective) {
                perspective_factor.x = 30;
                perspective_factor.y = perspective_factor.x;
            }

            mat3_scale(
                &view_mat,
                scale.x * perspective_factor.x,
                scale.y * perspective_factor.y
            );

            mat3_translate(&view_mat, -center.x, -center.y);

            size_t mimap_idx = texture->mipmap_count - roundf(
                ((i + 1) / (float) scr_h) * texture->mipmap_count
            );

            if (mimap_idx >= texture->mipmap_count) {
                mimap_idx = texture->mipmap_count - 1;
            }

            texture_mimap_t * const mipmap = &texture->mipmaps[mimap_idx];

            for (j = 0; j < scr_w; j++) {
                vec2_t tx = {j, i};
                mat3_transform(&view_mat, &tx);

                if (0
                    || !(0 <= tx.x && tx.x < texture->mipmaps[0].image->width)
                    || !(0 <= tx.y && tx.y < texture->mipmaps[0].image->height)
                ) {
                    tx.x = wrap_repeat(
                        tx.x,
                        maps[current_map].padding_box_pos.x,
                        maps[current_map].padding_box_pos.x + maps[current_map].padding_box_size - 1
                    );

                    tx.y = wrap_repeat(
                        tx.y,
                        maps[current_map].padding_box_pos.y,
                        maps[current_map].padding_box_pos.y + maps[current_map].padding_box_size - 1
                    );
                }

                tx.x /= mipmap->ratio;
                tx.y /= mipmap->ratio;

                const uint8_t color_idx = mipmap->image->data[(int)tx.y * mipmap->image->width + (int)tx.x];
                if ((color_idx + rendered_frame_count) % 13) {
#if HAVE_SIXEL
                    if (current_renderer != 3)
#endif /* HAVE_SIXEL */
                    continue;
                }

                renderers[current_renderer].draw(
                    j,
                    i,
                    mipmap->image->colors,
                    color_idx
                );

                rendered_pixel_count++;
            }
        }

        rendered_frame_count++;

#if HAVE_SIXEL
        if (current_renderer == 3) { /* sixel */
            output_sixel(texture->mipmaps[0].image->colors);
            continue;
        }
#endif /* HAVE_SIXEL */

        refresh();
        renderers[current_renderer].draw(0, i, texture->mipmaps[0].image->colors, 5);
        printw(
            "move spd: %6.1f, turn spd: %4.1f, colors: %3lu, mipmaps: %lu, renderer: %10s, map: %s\n",
            accelerator_velocity(&move_accelerator),
            accelerator_velocity(&turn_accelerator),
            color_count,
            mipmap_count,
            renderers[current_renderer].name,
            strrchr(maps[current_map].file_name, '/') + 1
        );
    }

    terminate_ncurses();
    texture_destroy(texture);

    return 0;
}

static void terminate_ncurses(void)
{
    static int called = 0;
    if (called) {
        return;
    }

    called = 1;

    restore_colors();
    standend();
    endwin();
}

static void restore_colors(void)
{
    static int init = 0;
    static short colors[256][3];
    static short pairs[256][2];

    size_t max_colors = sizeof(colors) / sizeof(colors[0]);
    if (max_colors > COLORS) {
        max_colors = COLORS;
    }

    size_t max_pairs = sizeof(pairs) / sizeof(pairs[0]);
    if (max_pairs > COLOR_PAIRS) {
        max_pairs = COLOR_PAIRS;
    }

    if (!init) {
        init = 1;

        size_t i;
        for (i = 0; i < max_colors; i++) {
            color_content(
                i,
                &colors[i][0],
                &colors[i][1],
                &colors[i][2]
            );
        }

        for (i = 0; i < max_pairs; i++) {
            pair_content(
                i,
                &pairs[i][0],
                &pairs[i][1]
            );
        }

        return;
    }

    size_t i;
    for (i = 0; i < max_colors; i++) {
        init_color(
            i,
            colors[i][0],
            colors[i][1],
            colors[i][2]
        );
    }

    for (i = 0; i < max_pairs; i++) {
        init_pair(
            i,
            pairs[i][0],
            pairs[i][1]
        );
    }

    attrset(A_NORMAL);
}

static image_t * image_create(const char * file_name)
{
    /*
     * Only indexed BMP v3 are supported, without RLE compression.
     * Use this ImageMagick command to convert an image to this format:
     *   convert in.png -colors 256 -compress none BMP3:out.bmp
     */

    image_t * image = NULL;
    FILE * fp = NULL;

    image = malloc(sizeof(*image));
    if (!image) {
        goto error;
    }

    image->data = NULL;

    fp = fopen(file_name, "rb");
    if (!fp) {
        goto error;
    }

    char header[54];
    size_t read;
    if ((read = fread(header, sizeof(header), 1, fp)) != 1) {
        goto error;
    }

    /* little endian only */
    image->width = *(uint32_t*)&header[18];
    image->height = *(uint32_t*)&header[22];

    image->data = malloc(image->width * image->height);
    if (!image->data) {
        goto error;
    }

    if (fread(image->colors, sizeof(image->colors), 1, fp) != 1) {
        goto error;
    }

    size_t i;
    /* BGRA -> RGBA */
    for (i = 0; i < 256; i++) {
        image->colors[i][0] ^= image->colors[i][2];
        image->colors[i][2] ^= image->colors[i][0];
        image->colors[i][0] ^= image->colors[i][2];
    }

    const size_t line_padding = image->width % 4;
    for (i = 0; i < image->height; i++) {
        if (
            fread(image->data + (image->height - i - 1) * image->width, image->width, 1, fp) != 1
        ) {
            goto error;
        }

        if (line_padding > 0) {
            fseek(fp, line_padding, SEEK_CUR);
        }
    }

    fclose(fp);

    return image;

error:
    if (image) {
        free(image->data);
    }

    free(image);

    if (fp) {
        fclose(fp);
    }

    return NULL;
}

static void image_quantize(image_t * image, size_t max_color_count)
{
    static uint32_t stats[256];

    while (1) {
        size_t i;

        for (i = 0; i < 256; i++) {
            stats[i] = 0;
        }

        for (i = 0; i < image->width * image->height; i++) {
            stats[image->data[i]]++;
        }

        size_t color_count = 0;
        for (i = 0; i < 256; i++) {
            if (stats[i] > 0) {
                color_count++;
            }
        }

        if (color_count < 2 || color_count <= max_color_count) {
            break;
        }

        struct {
            uint8_t a;
            uint8_t b;
            float dist;
        } nearest = {0, 0, FLT_MAX};

        for (i = 0; i < 256; i++) {
            if (stats[i] == 0) {
                continue;
            }

            size_t j;
            for (j = 0; j < 256; j++) {
                if (i >= j) {
                    continue;
                }

                if (stats[j] == 0) {
                    continue;
                }

                const float dist = sqrtf(0
                    + powf(image->colors[i][0] - image->colors[j][0], 2)
                    + powf(image->colors[i][1] - image->colors[j][1], 2)
                    + powf(image->colors[i][2] - image->colors[j][2], 2)
                );

                if (nearest.dist > dist) {
                    nearest.dist = dist;
                    nearest.a = i;
                    nearest.b = j;
                }
            }
        }

        for (i = 0; i < 3; i++) {
            image->colors[nearest.a][i] = (0
                + image->colors[nearest.a][i] * stats[nearest.a]
                + image->colors[nearest.b][i] * stats[nearest.b]
            ) / (0
                + stats[nearest.a]
                + stats[nearest.b]
            );
        }

        for (i = 0; i < image->width * image->height; i++) {
            if (image->data[i] == nearest.b) {
                image->data[i] = nearest.a;
            }
        }
    }
}

static image_t * image_create_downsized_copy(const image_t * image, size_t w, size_t h)
{
    if (0
        || w >= image->width || image->width % w
        || h >= image->height || image->height % h
    ) {
        fprintf(stderr, "Invalid downsizing parameters\n");
        exit(1);
    }

    image_t * new_image = malloc(sizeof(*image));
    if (!new_image) {
        goto error;
    }

    new_image->data = malloc(w * h);
    if (!new_image->data) {
        goto error;
    }

    new_image->width = w;
    new_image->height = h;

    memcpy(new_image->colors, image->colors, sizeof(new_image->colors));

    const size_t hr = image->height / new_image->height;
    const size_t wr = image->width / new_image->width;
    size_t i;
    for (i = 0; i < new_image->height; i++) {
        size_t j;
        for (j = 0; j < new_image->width; j++) {
            uint16_t stats[256] = {0};
            size_t k;
            for (k = 0; k < hr; k++) {
                size_t l;
                for (l = 0; l < wr; l++) {
                    stats[image->data[(i * hr + k) * image->width + (j * wr + l)]]++;
                }
            }

            size_t avg[3] = {0};
            for (k = 0; k < 256; k++) {
                if (stats[k] == 0) {
                    continue;
                }

                avg[0] += image->colors[k][0] * stats[k];
                avg[1] += image->colors[k][1] * stats[k];
                avg[2] += image->colors[k][2] * stats[k];
            }

            avg[0] /= hr * wr;
            avg[1] /= hr * wr;
            avg[2] /= hr * wr;

            struct {
                uint8_t idx;
                size_t dist;
            } nearest = {0, SIZE_MAX};

            for (k = 0; k < 256; k++) {
                /*
                 * This is not the vector length, we just need a fast approximation of relative distance.
                 */
                const size_t dist = 0
                    + abs((int)(avg[0] - image->colors[k][0]))
                    + abs((int)(avg[1] - image->colors[k][1]))
                    + abs((int)(avg[2] - image->colors[k][2]))
                ;

                if (nearest.dist > dist) {
                    nearest.dist = dist;
                    nearest.idx = k;
                }
            }

            new_image->data[i * new_image->width + j] = nearest.idx;
        }
    }

    return new_image;

error:
    if (new_image) {
        image_destroy(new_image);
    }

    return NULL;
}

static void image_destroy(image_t * image)
{
    free(image->data);
    free(image);
}

static texture_t * texture_create(const char * file_name, size_t max_color_count, size_t mipmap_count)
{
    texture_t * texture = malloc(sizeof(*texture));
    if (!texture) {
        return NULL;
    }

    texture->mipmap_count = 0;

    const size_t max_mipmap_count = sizeof(texture->mipmaps) / sizeof(texture->mipmaps[0]);

    if (mipmap_count == 0) {
        mipmap_count = 1;
    }

    if (mipmap_count > max_mipmap_count) {
        mipmap_count = max_mipmap_count;
    }

    texture->mipmaps[0].ratio = 1;
    texture->mipmaps[0].image = image_create(file_name);
    if (!texture->mipmaps[0].image) {
        goto error;
    }

    texture->mipmap_count++;

    image_quantize(texture->mipmaps[0].image, max_color_count);

    size_t i;
    for (i = 1; i < mipmap_count; i++) {
        texture->mipmaps[i].ratio = 2 * texture->mipmaps[i - 1].ratio;
        texture->mipmaps[i].image = image_create_downsized_copy(
            texture->mipmaps[0].image,
            texture->mipmaps[0].image->width / texture->mipmaps[i].ratio,
            texture->mipmaps[0].image->height / texture->mipmaps[i].ratio
        );

        if (!texture->mipmaps[i].image) {
            goto error;
        }

        texture->mipmap_count++;
    }

    return texture;

error:
    texture_destroy(texture);

    return NULL;
}

static void texture_destroy(texture_t * texture)
{
    size_t i;
    for (i = 0; i < texture->mipmap_count; i++) {
        image_destroy(texture->mipmaps[i].image);
    }

    free(texture);
}

static void mat3_identity(mat3_t * m)
{
    m->nums[0][0] = 1;
    m->nums[0][1] = 0;
    m->nums[0][2] = 0;

    m->nums[1][0] = 0;
    m->nums[1][1] = 1;
    m->nums[1][2] = 0;

    m->nums[2][0] = 0;
    m->nums[2][1] = 0;
    m->nums[2][2] = 1;
}

static void mat3_copy(mat3_t * a, const mat3_t * b)
{
    memcpy(a, b, sizeof(*a));
}

static void mat3_mult(mat3_t * a, const mat3_t * b)
{
    size_t i, j;
    mat3_t res;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            res.nums[i][j] = 0
                + a->nums[i][0] * b->nums[0][j]
                + a->nums[i][1] * b->nums[1][j]
                + a->nums[i][2] * b->nums[2][j]
            ;
        }
    }

    mat3_copy(a, &res);
}

static void mat3_translate(mat3_t * m, float x, float y)
{
    mat3_t transform_mat;
    mat3_identity(&transform_mat);
    transform_mat.nums[0][2] = x;
    transform_mat.nums[1][2] = y;

    mat3_mult(m, &transform_mat);
}

static void mat3_scale(mat3_t * m, float x, float y)
{
    mat3_t transform_mat;
    mat3_identity(&transform_mat);
    transform_mat.nums[0][0] = x;
    transform_mat.nums[1][1] = y;

    mat3_mult(m, &transform_mat);
}

static void mat3_rotate(mat3_t * m, float x)
{
    const float cos = cosf(x);
    const float sin = sinf(x);
    mat3_t transform_mat;
    mat3_identity(&transform_mat);
    transform_mat.nums[0][0] = cos;
    transform_mat.nums[0][1] = -sin;
    transform_mat.nums[1][0] = sin;
    transform_mat.nums[1][1] = cos;

    mat3_mult(m, &transform_mat);
}

static void mat3_transform(const mat3_t * m, vec2_t * v)
{
    const float x = 0
        + m->nums[0][0] * v->x
        + m->nums[0][1] * v->y
        + m->nums[0][2]
    ;

    v->y = 0
        + m->nums[1][0] * v->x
        + m->nums[1][1] * v->y
        + m->nums[1][2]
    ;

    v->x = x;
}

static float wrap_repeat(float v, float min, float max)
{
    v -= min;
    v = fmod(v, max - min);
    v += min;

    if (v < min) {
        v += max - min;
    }

    return v;
}

static void renderersixel_init(uint8_t colors[][4])
{
    restore_colors();
}

#if HAVE_SIXEL
static void renderersixel_draw(size_t x, size_t y, uint8_t colors[][4], uint8_t color_idx)
{
    pixbuf[y * pixbuf_width + x] = color_idx;
}
#endif

static void renderer256_init(uint8_t colors[][4])
{
    size_t i;
    for (i = 0; i < 256; i++) {
        init_pair(i + 1, COLOR_BLACK, i);
    }

    for (i = 0; i < 256; i++) {
        init_color(
            i,
            (1000 * colors[i][0]) / 255,
            (1000 * colors[i][1]) / 255,
            (1000 * colors[i][2]) / 255
        );
    }
}

static void renderer256_draw(size_t x, size_t y, uint8_t colors[][4], uint8_t color_idx)
{
    static int last_color_idx = -1;

    if (last_color_idx != color_idx) {
        last_color_idx = color_idx;
        attron(COLOR_PAIR(color_idx + 1));
    }

    mvaddch(y, x, ' ');
}

static void renderer16_init(uint8_t colors[][4])
{
    const int available_colors[] = {
        COLOR_BLACK,   /* 000 -> 0 */
        COLOR_BLUE,    /* 001 -> 1 */
        COLOR_GREEN,   /* 010 -> 2 */
        COLOR_CYAN,    /* 011 -> 3 */
        COLOR_RED,     /* 100 -> 4 */
        COLOR_MAGENTA, /* 101 -> 5 */
        COLOR_YELLOW,  /* 110 -> 6 */
        COLOR_WHITE,   /* 111 -> 7 */
    };

    const size_t available_color_count = sizeof(available_colors) / sizeof(available_colors[0]);

    size_t i;
    for (i = 0; i < available_color_count; i++) {
        init_pair(i + 1, available_colors[i], COLOR_BLACK);
    }
}

static void renderer16_draw(size_t x, size_t y, uint8_t colors[][4], uint8_t color_idx)
{
    const int available_colors[] = {
        COLOR_BLACK,   /* 000 -> 0 */
        COLOR_BLUE,    /* 001 -> 1 */
        COLOR_GREEN,   /* 010 -> 2 */
        COLOR_CYAN,    /* 011 -> 3 */
        COLOR_RED,     /* 100 -> 4 */
        COLOR_MAGENTA, /* 101 -> 5 */
        COLOR_YELLOW,  /* 110 -> 6 */
        COLOR_WHITE,   /* 111 -> 7 */
    };

    const size_t available_color_count = sizeof(available_colors) / sizeof(available_colors[0]);

    const uint8_t * color = colors[color_idx];

    int lum = 0;
    int max_comp = 0;
    size_t i;
    for (i = 0; i < 3; i++) {
        if (max_comp < color[i]) {
            max_comp = color[i];
        }

        const int comp_lum = roundf(color[i] / (255.f / 4));
        if (lum < comp_lum) {
            lum = comp_lum;
        }
    }

    int normalized_color = 0
        | (color[0] / (float) max_comp > 0.75 ? 1 : 0) << 2
        | (color[1] / (float) max_comp > 0.75 ? 1 : 0) << 1
        | (color[2] / (float) max_comp > 0.75 ? 1 : 0) << 0
    ;

    if (lum == 0) {
        normalized_color = 0;
    }

    attrset((lum <= 2 ? A_NORMAL : A_BOLD) | A_REVERSE);
    attron(COLOR_PAIR(normalized_color + 1));
    mvaddch(y, x, ' ');
}

static void renderer1_init(uint8_t colors[][4])
{
}

static void renderer1_draw(size_t x, size_t y, uint8_t colors[][4], uint8_t color_idx)
{
    const char charset[] = " .`^*:;+=%§$#";
    const size_t charset_size = sizeof(charset) -1;

    const uint8_t * color = colors[color_idx];
    int lum = 0;
    size_t i;
    for (i = 0; i < 3; i++) {
        const int comp_lum = roundf(color[i] / (255.f / charset_size));
        if (lum < comp_lum) {
            lum = comp_lum;
        }
    }

    if (lum >= charset_size) {
        lum = charset_size - 1;
    }

    mvaddch(y, x, charset[lum]);
}

static size_t current_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
}

static int kb_event_get()
{
    static struct {
        size_t count;
        size_t last_time_ms;
    } pressed_keys[512] = {{0}};

    static const size_t pressed_keys_size = sizeof(pressed_keys) / sizeof(pressed_keys[0]);

    const size_t current_time_ms = current_time_ns() / (1000 * 1000);

    const int c = getch();

    if (c == ERR) {
        goto release_next;
    }

    if (c >= pressed_keys_size) {
        goto release_next;
    }

    pressed_keys[c].last_time_ms = current_time_ms;
    pressed_keys[c].count++;

    if (pressed_keys[c].count > 1) {
        goto release_next;
    }

    printw("press = %d\n", c);

    return c;

release_next:
    {
        size_t i;
        for (i = 0; i < pressed_keys_size; i++) {
            if (pressed_keys[i].count == 0) {
                continue;
            }

            const size_t last_time_delay_ms = current_time_ms - pressed_keys[i].last_time_ms;
            if (
                (pressed_keys[i].count == 1 && last_time_delay_ms > 600)
                || (pressed_keys[i].count > 1 && last_time_delay_ms > 120)
            ) {
                pressed_keys[i].count = 0;

                printw("release = %d\n", i);

                return i | KB_EVENT_RELEASE;
            }
        }
    }

    return ERR;
}

static void accelerator_init(accelerator_t * accelerator, float acceleration, float deceleration, float max)
{
    accelerator->acceleration = acceleration;
    accelerator->deceleration = deceleration;
    accelerator->max = max;
    accelerator->last_time_ms = 0;
    accelerator->active = 0;
    accelerator->reverse = 0;
    accelerator->velocity = 0;
}

static void accelerator_press(accelerator_t * accelerator, int reverse)
{
    accelerator->active = 1;
    accelerator->reverse = reverse;
}

static void accelerator_release(accelerator_t * accelerator)
{
    accelerator->active = 0;
}

static float accelerator_step_distance(accelerator_t * accelerator)
{
    const size_t current_time_ms = current_time_ns() / (1000 * 1000);
    if (accelerator->last_time_ms == 0) {
        accelerator->last_time_ms = current_time_ms;
    }

    const float time_diff = (current_time_ms - accelerator->last_time_ms) / 1000.f;
    const float distance = accelerator->velocity * time_diff;

    const int dir = accelerator->reverse ? -1 : 1;

    if (accelerator->active) {
        accelerator->velocity += dir * accelerator->acceleration * time_diff;
        if (dir * accelerator->velocity > accelerator->max) {
            accelerator->velocity = dir * accelerator->max;
        }
    } else if (accelerator->velocity != 0) {
        accelerator->velocity -= dir * accelerator->deceleration * time_diff;
        if (dir * accelerator->velocity < 0) {
            accelerator->velocity = 0;
        }
    }

    accelerator->last_time_ms = current_time_ms;

    return distance;
}

static float accelerator_velocity(const accelerator_t * accelerator)
{
    return accelerator->velocity;
}
