/*
 * Save the current frame as a 24-bit BMP.
 *
 * Debugging aid: BB10 has no usable screen-capture CLI for a sandboxed app
 * (screencapture is a PPS service), so the app photographs itself. BMP keeps
 * it dependency-free; the file lands in $HOME and is pulled off the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

int screenshot_save(SDL_Surface *s, const char *path) {
	FILE *fp;
	int y, x, bpp;
	int row_bytes, pad;
	unsigned char *row;

	if (s == NULL || path == NULL) {
		return -1;
	}
	if (SDL_LockSurface(s) != 0) {
		return -1;
	}
	bpp = s->format->BytesPerPixel;
	row_bytes = s->w * 3;
	pad = (4 - (row_bytes % 4)) % 4;
	row = malloc((size_t)row_bytes + (size_t)pad);
	if (row == NULL) {
		SDL_UnlockSurface(s);
		return -1;
	}
	fp = fopen(path, "wb");
	if (fp == NULL) {
		free(row);
		SDL_UnlockSurface(s);
		return -1;
	}
	{
		unsigned char hdr[54];
		unsigned int file_size = 54u + (unsigned int)((row_bytes + pad) * s->h);
		memset(hdr, 0, sizeof(hdr));
		hdr[0] = 'B'; hdr[1] = 'M';
		hdr[2] = (unsigned char)(file_size);
		hdr[3] = (unsigned char)(file_size >> 8);
		hdr[4] = (unsigned char)(file_size >> 16);
		hdr[5] = (unsigned char)(file_size >> 24);
		hdr[10] = 54;
		hdr[14] = 40;
		hdr[18] = (unsigned char)(s->w);
		hdr[19] = (unsigned char)(s->w >> 8);
		hdr[20] = (unsigned char)(s->w >> 16);
		hdr[21] = (unsigned char)(s->w >> 24);
		hdr[22] = (unsigned char)(s->h);
		hdr[23] = (unsigned char)(s->h >> 8);
		hdr[24] = (unsigned char)(s->h >> 16);
		hdr[25] = (unsigned char)(s->h >> 24);
		hdr[26] = 1;              /* planes */
		hdr[28] = 24;             /* bits per pixel */
		fwrite(hdr, 1, sizeof(hdr), fp);
	}
	for (y = s->h - 1; y >= 0; --y) {          /* BMP rows are bottom-up */
		unsigned char *src = (unsigned char *)s->pixels + y * s->pitch;
		for (x = 0; x < s->w; ++x) {
			Uint32 pix = 0;
			Uint8 r, g, b;
			switch (bpp) {
			case 4: pix = *(Uint32 *)(src + x * 4); break;
			case 2: pix = *(Uint16 *)(src + x * 2); break;
			default: pix = src[x]; break;
			}
			SDL_GetRGB(pix, s->format, &r, &g, &b);
			row[x * 3 + 0] = b;
			row[x * 3 + 1] = g;
			row[x * 3 + 2] = r;
		}
		if (pad) {
			memset(row + row_bytes, 0, (size_t)pad);
		}
		fwrite(row, 1, (size_t)row_bytes + (size_t)pad, fp);
	}
	fclose(fp);
	free(row);
	SDL_UnlockSurface(s);
	return 0;
}
