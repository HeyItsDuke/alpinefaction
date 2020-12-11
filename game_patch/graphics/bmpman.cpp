#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include <cassert>
#include "graphics_internal.h"
#include "graphics.h"

int get_surface_pitch(int w, rf::BmFormat format)
{
    switch (format) {
        case rf::BM_FORMAT_8888_ARGB:
            return w * 4;
        case rf::BM_FORMAT_888_RGB:
            return w * 3;
        case rf::BM_FORMAT_565_RGB:
        case rf::BM_FORMAT_1555_ARGB:
        case rf::BM_FORMAT_4444_ARGB:
            return w * 2;
        case rf::BM_FORMAT_8_ALPHA:
        case rf::BM_FORMAT_8_PALETTED:
            return w;
        case rf::BM_FORMAT_DXT1:
            // 4x4 pixels per block, 64 bits per block
            return (w + 3) / 4 * 64 / 8;
        case rf::BM_FORMAT_DXT2:
        case rf::BM_FORMAT_DXT3:
        case rf::BM_FORMAT_DXT4:
        case rf::BM_FORMAT_DXT5:
            // 4x4 pixels per block, 128 bits per block
            return (w + 3) / 4 * 128 / 8;
        default:
            xlog::warn("Unknown format %d in get_surface_pitch", format);
            return -1;
    }
}

int get_surface_num_rows(int h, rf::BmFormat format)
{
    switch (format) {
        case rf::BM_FORMAT_DXT1:
        case rf::BM_FORMAT_DXT2:
        case rf::BM_FORMAT_DXT3:
        case rf::BM_FORMAT_DXT4:
        case rf::BM_FORMAT_DXT5:
            // 4x4 pixels per block
            return (h + 3) / 4;
        default:
            return h;
    }
}

size_t get_surface_length_in_bytes(int w, int h, rf::BmFormat format)
{
    return get_surface_pitch(w, format) * get_surface_num_rows(h, format);
}

bool bm_is_compressed_format(rf::BmFormat format)
{
    switch (format) {
        case rf::BM_FORMAT_DXT1:
        case rf::BM_FORMAT_DXT2:
        case rf::BM_FORMAT_DXT3:
        case rf::BM_FORMAT_DXT4:
        case rf::BM_FORMAT_DXT5:
            return true;
        default:
            return false;
    }
}

FunHook<rf::BmType(const char*, int*, int*, rf::BmFormat*, int*, int*, int*, int*, int*, int*, int)>
bm_read_header_hook{
    0x0050FCB0,
    [](const char* filename, int* width_out, int* height_out, rf::BmFormat *pixel_fmt_out, int *num_levels_out,
    int *num_levels_external_mips_out, int *num_frames_out, int *fps_out, int *total_bytes_m2v_out,
    int *vbm_ver_out, int a11) {

        *num_levels_external_mips_out = 1;
        *num_levels_out = 1;
        *fps_out = 0;
        *num_frames_out = 1;
        *total_bytes_m2v_out = -1;
        *vbm_ver_out = 1;

        rf::File dds_file;
        std::string filename_without_ext{get_filename_without_ext(filename)};
        auto dds_filename = filename_without_ext + ".dds";
        if (dds_file.open(dds_filename.c_str()) == 0) {
            xlog::trace("Loading %s", dds_filename.c_str());
            auto bm_type = read_dds_header(dds_file, width_out, height_out, pixel_fmt_out, num_levels_out);
            if (bm_type != rf::BM_TYPE_NONE) {
                return bm_type;
            }
        }

        xlog::trace("Loading bitmap header for '%s'", filename);
        auto bm_type = bm_read_header_hook.call_target(filename, width_out, height_out, pixel_fmt_out, num_levels_out,
            num_levels_external_mips_out, num_frames_out, fps_out, total_bytes_m2v_out, vbm_ver_out, a11);
        xlog::trace("Bitmap header for '%s': type %d size %dx%d pixel_fmt %d levels %d frames %d",
            filename, bm_type, *width_out, *height_out, *pixel_fmt_out, *num_levels_out, *num_frames_out);

        // Sanity checks
        // Prevents heap corruption when width = 0 or height = 0
        if (*width_out <= 0 || *height_out <= 0 || *pixel_fmt_out == rf::BM_FORMAT_NONE || *num_levels_out < 1 || *num_frames_out < 1) {
            bm_type = rf::BM_TYPE_NONE;
        }

        if (bm_type == rf::BM_TYPE_NONE) {
            xlog::warn("Failed load bitmap header for '%s'", filename);
        }

        return bm_type;
    },
};

FunHook<rf::BmFormat(int, void**, void**)> bm_lock_hook{
    0x00510780,
    [](int bmh, void** pixels_out, void** palette_out) {
        auto& bm_entry = rf::bm_bitmaps[rf::bm_handle_to_index_anim_aware(bmh)];
        if (bm_entry.bm_type == rf::BM_TYPE_DDS) {
            lock_dds_bitmap(bm_entry);
            *pixels_out = bm_entry.locked_data;
            *palette_out = bm_entry.locked_palette;
            return bm_entry.format;
        }
        else {
            auto pixel_fmt = bm_lock_hook.call_target(bmh, pixels_out, palette_out);
            if (pixel_fmt == rf::BM_FORMAT_NONE) {
                *pixels_out = nullptr;
                *palette_out = nullptr;
                xlog::warn("bm_lock failed");
            }
            return pixel_fmt;
        }
    },
};

FunHook<bool(int)> bm_has_alpha_hook{
    0x00510710,
    [](int bm_handle) {
        auto format = rf::bm_get_format(bm_handle);
        switch (format) {
            case rf::BM_FORMAT_4444_ARGB:
            case rf::BM_FORMAT_1555_ARGB:
            case rf::BM_FORMAT_8888_ARGB:
            case rf::BM_FORMAT_DXT1:
            case rf::BM_FORMAT_DXT2:
            case rf::BM_FORMAT_DXT3:
            case rf::BM_FORMAT_DXT4:
            case rf::BM_FORMAT_DXT5:
                return true;
            default:
                return false;
        }
    },
};

FunHook<void(int)> bm_free_entry_hook{
    0x0050F240,
    [](int bm_index) {
        rf::bm_bitmaps[bm_index].dynamic = false;
        bm_free_entry_hook.call_target(bm_index);
    },
};

void bm_set_dynamic(int bm_handle, bool dynamic)
{
    int bm_index = rf::bm_handle_to_index_anim_aware(bm_handle);
    rf::bm_bitmaps[bm_index].dynamic = dynamic;
}

bool bm_is_dynamic(int bm_handle)
{
    int bm_index = rf::bm_handle_to_index_anim_aware(bm_handle);
    return rf::bm_bitmaps[bm_index].dynamic;
}

void bm_change_format(int bm_handle, rf::BmFormat format)
{
    int bm_idx = rf::bm_handle_to_index_anim_aware(bm_handle);
    auto& bm = rf::bm_bitmaps[bm_idx];
    assert(bm.bm_type == rf::BM_TYPE_USER);
    if (bm.format != format) {
        gr_delete_texture(bm_handle);
        bm.format = format;
    }
}

void bm_apply_patches()
{
    bm_read_header_hook.install();
    bm_lock_hook.install();
    bm_has_alpha_hook.install();
    bm_free_entry_hook.install();
}
