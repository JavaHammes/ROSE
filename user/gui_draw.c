#include <stddef.h>
#include <stdint.h>

#include "rose/font.h"
#include "rose/gui.h"

static int32_t maximum(int32_t left, int32_t right) {
        return left > right ? left : right;
}

static int32_t minimum(int32_t left, int32_t right) {
        return left < right ? left : right;
}

static struct rose_gui_rectangle intersect(struct rose_gui_rectangle left,
                                            struct rose_gui_rectangle right) {
        int32_t x = maximum(left.x, right.x);
        int32_t y = maximum(left.y, right.y);
        int32_t edge_x = minimum(left.x + left.width, right.x + right.width);
        int32_t edge_y = minimum(left.y + left.height, right.y + right.height);
        if (edge_x <= x || edge_y <= y) {
                return (struct rose_gui_rectangle){x, y, 0, 0};
        }
        return (struct rose_gui_rectangle){x, y, edge_x - x, edge_y - y};
}

void rose_gui_canvas_initialize(struct rose_gui_canvas *canvas,
                                uint32_t *pixels, uint32_t width,
                                uint32_t height, uint32_t pixel_stride) {
        if (canvas == NULL) return;
        canvas->pixels = pixels;
        canvas->width = width;
        canvas->height = height;
        canvas->pixel_stride = pixel_stride;
        canvas->clip = (struct rose_gui_rectangle){
            0, 0, (int32_t)width, (int32_t)height};
}

struct rose_gui_rectangle rose_gui_canvas_set_clip(
    struct rose_gui_canvas *canvas, struct rose_gui_rectangle clip) {
        if (canvas == NULL) {
                return (struct rose_gui_rectangle){0, 0, 0, 0};
        }
        struct rose_gui_rectangle previous = canvas->clip;
        struct rose_gui_rectangle surface = {
            0, 0, (int32_t)canvas->width, (int32_t)canvas->height};
        canvas->clip = intersect(intersect(previous, surface), clip);
        return previous;
}

void rose_gui_canvas_restore_clip(struct rose_gui_canvas *canvas,
                                  struct rose_gui_rectangle clip) {
        if (canvas == NULL) return;
        struct rose_gui_rectangle surface = {
            0, 0, (int32_t)canvas->width, (int32_t)canvas->height};
        canvas->clip = intersect(surface, clip);
}

static bool clipped_bounds(struct rose_gui_canvas *canvas, int32_t x,
                           int32_t y, int32_t width, int32_t height,
                           struct rose_gui_rectangle *result) {
        if (canvas == NULL || canvas->pixels == NULL || width <= 0 ||
            height <= 0) {
                return false;
        }
        *result = intersect(canvas->clip,
                            (struct rose_gui_rectangle){x, y, width, height});
        return result->width > 0 && result->height > 0;
}

void rose_gui_canvas_fill(struct rose_gui_canvas *canvas, int32_t x, int32_t y,
                          int32_t width, int32_t height, uint32_t color) {
        struct rose_gui_rectangle bounds;
        if (!clipped_bounds(canvas, x, y, width, height, &bounds)) return;
        color &= UINT32_C(0x00ffffff);
        for (int32_t row = bounds.y; row < bounds.y + bounds.height; row++) {
                for (int32_t column = bounds.x;
                     column < bounds.x + bounds.width; column++) {
                        canvas->pixels[(size_t)row * canvas->pixel_stride +
                                       (size_t)column] = color;
                }
        }
}

static uint32_t blend_pixel(uint32_t destination, uint32_t source) {
        uint32_t alpha = source >> 24U;
        if (alpha == 0U) return destination;
        if (alpha == 255U) return source & UINT32_C(0x00ffffff);
        uint32_t inverse = 255U - alpha;
        uint32_t red = (((source >> 16U) & 255U) * alpha +
                        ((destination >> 16U) & 255U) * inverse + 127U) /
                       255U;
        uint32_t green = (((source >> 8U) & 255U) * alpha +
                          ((destination >> 8U) & 255U) * inverse + 127U) /
                         255U;
        uint32_t blue = ((source & 255U) * alpha +
                         (destination & 255U) * inverse + 127U) /
                        255U;
        return (red << 16U) | (green << 8U) | blue;
}

void rose_gui_canvas_blend(struct rose_gui_canvas *canvas, int32_t x,
                           int32_t y, int32_t width, int32_t height,
                           uint32_t color) {
        struct rose_gui_rectangle bounds;
        if (!clipped_bounds(canvas, x, y, width, height, &bounds)) return;
        for (int32_t row = bounds.y; row < bounds.y + bounds.height; row++) {
                for (int32_t column = bounds.x;
                     column < bounds.x + bounds.width; column++) {
                        size_t offset = (size_t)row * canvas->pixel_stride +
                                        (size_t)column;
                        canvas->pixels[offset] =
                            blend_pixel(canvas->pixels[offset], color);
                }
        }
}

static void pixel(struct rose_gui_canvas *canvas, int32_t x, int32_t y,
                  uint32_t color) {
        if (canvas == NULL || canvas->pixels == NULL || x < canvas->clip.x ||
            y < canvas->clip.y || x >= canvas->clip.x + canvas->clip.width ||
            y >= canvas->clip.y + canvas->clip.height) {
                return;
        }
        canvas->pixels[(size_t)y * canvas->pixel_stride + (size_t)x] =
            color & UINT32_C(0x00ffffff);
}

void rose_gui_canvas_line(struct rose_gui_canvas *canvas, int32_t x0,
                          int32_t y0, int32_t x1, int32_t y1,
                          uint32_t color) {
        int32_t delta_x = x1 > x0 ? x1 - x0 : x0 - x1;
        int32_t step_x = x0 < x1 ? 1 : -1;
        int32_t delta_y = y1 > y0 ? y0 - y1 : y1 - y0;
        int32_t step_y = y0 < y1 ? 1 : -1;
        int32_t error = delta_x + delta_y;
        while (true) {
                pixel(canvas, x0, y0, color);
                if (x0 == x1 && y0 == y1) break;
                int32_t twice = error * 2;
                if (twice >= delta_y) {
                        error += delta_y;
                        x0 += step_x;
                }
                if (twice <= delta_x) {
                        error += delta_x;
                        y0 += step_y;
                }
        }
}

void rose_gui_canvas_border(struct rose_gui_canvas *canvas,
                            struct rose_gui_rectangle rectangle,
                            int32_t thickness, uint32_t color) {
        if (thickness <= 0) return;
        if (thickness * 2 > rectangle.width) thickness = rectangle.width / 2;
        if (thickness * 2 > rectangle.height)
                thickness = rectangle.height / 2;
        rose_gui_canvas_fill(canvas, rectangle.x, rectangle.y, rectangle.width,
                             thickness, color);
        rose_gui_canvas_fill(canvas, rectangle.x,
                             rectangle.y + rectangle.height - thickness,
                             rectangle.width, thickness, color);
        rose_gui_canvas_fill(canvas, rectangle.x, rectangle.y + thickness,
                             thickness, rectangle.height - thickness * 2,
                             color);
        rose_gui_canvas_fill(canvas,
                             rectangle.x + rectangle.width - thickness,
                             rectangle.y + thickness, thickness,
                             rectangle.height - thickness * 2, color);
}

void rose_gui_canvas_rounded_rectangle(struct rose_gui_canvas *canvas,
                                       struct rose_gui_rectangle rectangle,
                                       int32_t radius, uint32_t color) {
        if (rectangle.width <= 0 || rectangle.height <= 0) return;
        if (radius < 0) radius = 0;
        int32_t maximum_radius = minimum(rectangle.width, rectangle.height) / 2;
        if (radius > maximum_radius) radius = maximum_radius;
        if (radius == 0) {
                rose_gui_canvas_fill(canvas, rectangle.x, rectangle.y,
                                     rectangle.width, rectangle.height, color);
                return;
        }
        rose_gui_canvas_fill(canvas, rectangle.x, rectangle.y + radius,
                             rectangle.width, rectangle.height - radius * 2,
                             color);
        for (int32_t row = 0; row < radius; row++) {
                int32_t dy = radius - row;
                int32_t inset = 0;
                while (inset < radius &&
                       (radius - inset) * (radius - inset) + dy * dy >
                           radius * radius) {
                        inset++;
                }
                int32_t width = rectangle.width - inset * 2;
                rose_gui_canvas_fill(canvas, rectangle.x + inset,
                                     rectangle.y + row, width, 1, color);
                rose_gui_canvas_fill(
                    canvas, rectangle.x + inset,
                    rectangle.y + rectangle.height - row - 1, width, 1,
                    color);
        }
}

void rose_gui_canvas_image(struct rose_gui_canvas *canvas, int32_t x,
                           int32_t y, const struct rose_gui_image *image) {
        if (canvas == NULL || image == NULL || image->pixels == NULL) return;
        struct rose_gui_rectangle bounds;
        if (!clipped_bounds(canvas, x, y, (int32_t)image->width,
                            (int32_t)image->height, &bounds)) {
                return;
        }
        for (int32_t row = bounds.y; row < bounds.y + bounds.height; row++) {
                for (int32_t column = bounds.x;
                     column < bounds.x + bounds.width; column++) {
                        size_t destination =
                            (size_t)row * canvas->pixel_stride +
                            (size_t)column;
                        size_t source =
                            (size_t)(row - y) * image->pixel_stride +
                            (size_t)(column - x);
                        uint32_t color = image->pixels[source];
                        canvas->pixels[destination] =
                            image->alpha
                                ? blend_pixel(canvas->pixels[destination],
                                              color)
                                : color & UINT32_C(0x00ffffff);
                }
        }
}

void rose_gui_canvas_text(struct rose_gui_canvas *canvas, int32_t x, int32_t y,
                          const char *text, uint32_t color, uint32_t scale) {
        if (scale == 0U) return;
        while (text != NULL && *text != '\0') {
                const uint8_t *glyph = rose_font_glyph(*text++);
                if (glyph == NULL) return;
                for (int32_t glyph_x = 0; glyph_x < ROSE_GUI_FONT_WIDTH;
                     glyph_x++) {
                        for (int32_t glyph_y = 0; glyph_y < ROSE_GUI_FONT_HEIGHT;
                             glyph_y++) {
                                if ((glyph[glyph_x] & (1U << glyph_y)) != 0U) {
                                        rose_gui_canvas_fill(
                                            canvas,
                                            x + glyph_x * (int32_t)scale,
                                            y + glyph_y * (int32_t)scale,
                                            (int32_t)scale, (int32_t)scale,
                                            color);
                                }
                        }
                }
                x += (ROSE_GUI_FONT_WIDTH + 1) * (int32_t)scale;
        }
}

void rose_gui_canvas_icon(struct rose_gui_canvas *canvas, int32_t x, int32_t y,
                          const struct rose_gui_icon *icon, uint32_t color,
                          uint32_t scale) {
        if (icon == NULL || scale == 0U) return;
        for (int32_t row = 0; row < ROSE_GUI_ICON_SIZE; row++) {
                for (int32_t column = 0; column < ROSE_GUI_ICON_SIZE; column++) {
                        if ((icon->rows[row] & (UINT8_C(0x80) >> column)) !=
                            0U) {
                                rose_gui_canvas_fill(
                                    canvas, x + column * (int32_t)scale,
                                    y + row * (int32_t)scale, (int32_t)scale,
                                    (int32_t)scale, color);
                        }
                }
        }
}

int32_t rose_gui_text_width(const char *text, uint32_t scale) {
        if (text == NULL || scale == 0U) return 0;
        int32_t count = 0;
        while (*text++ != '\0') count++;
        return count * (ROSE_GUI_FONT_WIDTH + 1) * (int32_t)scale;
}

void rose_gui_fill(struct rose_gui_context *context, int32_t x, int32_t y,
                   int32_t width, int32_t height, uint32_t color) {
        if (context == NULL) return;
        rose_gui_canvas_fill(&context->canvas, x, y, width, height, color);
}

void rose_gui_text(struct rose_gui_context *context, int32_t x, int32_t y,
                   const char *text, uint32_t color, uint32_t scale) {
        if (context == NULL) return;
        rose_gui_canvas_text(&context->canvas, x, y, text, color, scale);
}
