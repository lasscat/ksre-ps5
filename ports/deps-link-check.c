/* SPDX-License-Identifier: GPL-3.0-or-later
 * Link-only check (never run) that every Ren'Py C dependency in build/sysroot resolves. */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <fribidi.h>
#include <png.h>
#include <stdio.h>
#include <jpeglib.h>
#include <webp/decode.h>
#include <zlib.h>
#include <bzlib.h>
#include <lzma.h>
#include <SDL2/SDL_image.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

int main(int argc, char **argv) {
	FT_Library ft; struct jpeg_error_mgr jerr;
	(void) argv;
	if (argc > 99) { /* never taken; keeps the references alive */
		FT_Init_FreeType(&ft);
		hb_buffer_destroy(hb_buffer_create());
		hb_ft_font_create_referenced(NULL);
		fribidi_get_bidi_type(0x5d0);
		png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
		jpeg_std_error(&jerr);
		WebPGetDecoderVersion();
		zlibVersion(); BZ2_bzlibVersion(); lzma_version_string();
		IMG_Load("x");
		avcodec_find_decoder(AV_CODEC_ID_VP9);
		avcodec_find_decoder(AV_CODEC_ID_OPUS);
		avformat_alloc_context();
		swr_alloc(); sws_getContext(1, 1, AV_PIX_FMT_YUV420P, 1, 1, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
	}
	puts("ok");
	return 0;
}
