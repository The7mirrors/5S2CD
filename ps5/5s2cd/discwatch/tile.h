#pragma once
#include <stddef.h>
#include <stdint.h>

/* the pictures a tile can be made from: any may be NULL (or a file that isn't there) */
typedef struct
{
	const char* cover; /* the PS2 case's front */
	const char* hero;  /* SteamGridDB hero art (a wide banner) */
	const char* logo;  /* SteamGridDB logo (transparent PNG) */
	const char* grid;  /* SteamGridDB square grid */
	const char* shot;  /* a screenshot for the background (libretro's title screen or in-game snap, no key needed) */
	const char* title; /* the title screen, for the icon */
	const char* font;  /* a TTF/OTF for the label */
	const char* label; /* "PlayStation 2" under the logo */
} TileArt;

/* 0 on success: icon_out gets a 512x512 PNG, pic_out a 3840x2160 BC7 DDS (either may be NULL); used says what was used */
int tile_make_art(const TileArt* a, const char* icon_out, const char* pic_out, char* used, size_t used_size);
int tile_make(const char* cover_path, const char* icon_out, const char* pic_out);
int tile_write_png(const char* path, const uint8_t* rgb, int w, int h);
int tile_write_bc7_dds(const char* path, const uint8_t* rgb, int w, int h);
