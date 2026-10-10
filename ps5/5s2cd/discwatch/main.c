/* discwatch (2026-10-09, AI-assisted): PS2 disc tiles on the PS5 home screen, for PS5SX2 Disc.
 *
 * Watches every disc drive (/dev/cd0 = the PS5's own, /dev/cd1.. = USB drives). When a PS2 disc goes in, it makes a
 * tile for it in /data/homebrew/PPSA9922x: the PS5SX2 Disc eboot (hard-linked, no extra space), disc-tile.txt naming
 * the drive (the app then boots that disc, no shelf), and sce_sys with the game's name, its cover as the icon and the
 * cover stretched as the background. ShadowMount+ (its API on 127.0.0.1:10101) registers the tile; when the disc
 * comes out the tile is uninstalled and its folder removed. Each drive has its own tile. Nothing is started by itself.
 *
 * Covers: /data/PCSX2/covers/<SERIAL>.jpg (the emulator's own), else downloaded from xlenore/ps2-covers into there.
 * Names: /data/PCSX2/resources/GameIndex.yaml (name-en, else name). Log: /data/PCSX2/disc-tiles/discwatch.log.
 * GPL-3.0-or-later, like PS5SX2. */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "dw_https.h"
#include "json.h"
#include "log.h"
#include "tile.h"
#include "util.h"

/* the PS5's notification request, as the PS5SX2 Installer and the payload SDK's samples lay it out (0xC30 bytes) */
typedef struct
{
	int32_t type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
	char use_icon_image_uri;
	char message[1024];
	char uri[1024];
	char unkstr[1024];
	char pad[3];
} notify_request_t;
_Static_assert(sizeof(notify_request_t) == 0xC30, "notification request must be 0xC30 bytes");
int sceKernelSendNotificationRequest(int32_t device, void* request, size_t size, int32_t blocking);

#define VERSION "1.2"
#define APP_SRC "/data/homebrew/PPSA99213"
#define HB "/data/homebrew"
#define WORK "/data/PCSX2/disc-tiles"
#define COVERS "/data/PCSX2/covers"
#define GAMEDB "/data/PCSX2/resources/GameIndex.yaml"
#define WHITELIST "/data/whitelist.txt"
#define MAX_DRIVES 8
#define NSLOTS 8
#define POLL_MS 2500
#define LOCK_PORT 9131

static const char* const kSlots[NSLOTS] = {"PPSA99221", "PPSA99222", "PPSA99223", "PPSA99224", "PPSA99225", "PPSA99226", "PPSA99227", "PPSA99228"};

typedef struct
{
	int present;      /* a disc was readable last time */
	off_t media;      /* its size, to notice a swap without reading */
	char serial[16];  /* "" = not a PS2 disc */
	int slot;         /* its tile, -1 = none */
	int fails;        /* consecutive failed tile attempts */
} Drive;

static Drive g_drv[MAX_DRIVES];
static int g_slot_busy[NSLOTS];  /* drive index + 1, 0 = free */
static int g_slot_stale[NSLOTS]; /* uninstall still to be done (it was busy) */
static int g_next_slot;

/* ---------- log ---------- */
static FILE* g_log;
void log_line(const char* fmt, ...)
{
	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(stamp, sizeof stamp, "%m-%d %H:%M:%S", &tm);
	va_list ap;
	va_start(ap, fmt);
	char msg[1024];
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	printf("[discwatch] %s\n", msg);
	if (g_log)
	{
		fprintf(g_log, "%s %s\n", stamp, msg);
		fflush(g_log);
	}
}
void notify(const char* fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	log_line("notify: %s", msg);
	static notify_request_t req;
	memset(&req, 0, sizeof req);
	req.target_id = -1;
	str_copy(req.message, sizeof req.message, msg);
	sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* ---------- small file helpers ---------- */
static int write_file(const char* path, const void* data, size_t len)
{
	FILE* f = fopen(path, "wb");
	if (!f)
		return -1;
	int ok = fwrite(data, 1, len, f) == len;
	ok = fclose(f) == 0 && ok;
	chmod(path, 0777);
	return ok ? 0 : -1;
}
static char* read_file(const char* path, size_t* len_out)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* b = n >= 0 ? malloc((size_t)n + 1) : NULL;
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n)
	{
		free(b);
		b = NULL;
	}
	fclose(f);
	if (b)
	{
		b[n] = 0;
		if (len_out)
			*len_out = (size_t)n;
	}
	return b;
}
static int copy_file(const char* src, const char* dst)
{
	size_t n;
	char* b = read_file(src, &n);
	if (!b)
		return -1;
	int r = write_file(dst, b, n);
	free(b);
	return r;
}
static void rm_tree(const char* path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode))
	{
		DIR* d = opendir(path);
		if (d)
		{
			struct dirent* e;
			while ((e = readdir(d)))
			{
				if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
					continue;
				char sub[512];
				snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
				rm_tree(sub);
			}
			closedir(d);
		}
		rmdir(path);
	}
	else
		unlink(path);
}
/* hard link (no extra space), else a copy */
static int link_or_copy(const char* src, const char* dst)
{
	unlink(dst);
	if (link(src, dst) == 0)
		return 0;
	const int e = errno;
	if (copy_file(src, dst) == 0)
	{
		log_line("link %s failed (errno %d): copied instead", dst, e);
		return 0;
	}
	return -1;
}
/* mtimes a minute back, so ShadowMount+'s 10 s "still being written" wait doesn't hold the new tile back */
static void age(const char* path)
{
	struct timeval tv[2];
	gettimeofday(&tv[0], NULL);
	tv[0].tv_sec -= 60;
	tv[1] = tv[0];
	utimes(path, tv);
}

/* ---------- the disc ---------- */
static int find_root_file(int fd, const uint8_t* pvd, const char* want, uint32_t* lba, uint32_t* size)
{
	const uint8_t* root = pvd + 156;
	uint32_t dlba = root[2] | root[3] << 8 | root[4] << 16 | (uint32_t)root[5] << 24;
	uint32_t dlen = root[10] | root[11] << 8 | root[12] << 16 | (uint32_t)root[13] << 24;
	if (dlen > 64 * 2048)
		dlen = 64 * 2048;
	dlen = (dlen + 2047) / 2048 * 2048;
	uint8_t* dir = malloc(dlen);
	if (!dir)
		return -1;
	if (pread(fd, dir, dlen, (off_t)dlba * 2048) != (ssize_t)dlen)
	{
		free(dir);
		return -1;
	}
	const size_t wl = strlen(want);
	for (uint32_t o = 0; o + 33 < dlen;)
	{
		uint8_t len = dir[o];
		if (!len)
		{
			o = (o / 2048 + 1) * 2048;
			continue;
		}
		uint8_t nl = dir[o + 32];
		if (nl >= wl && o + 33 + wl <= dlen && !strncasecmp((const char*)dir + o + 33, want, wl))
		{
			*lba = dir[o + 2] | dir[o + 3] << 8 | dir[o + 4] << 16 | (uint32_t)dir[o + 5] << 24;
			*size = dir[o + 10] | dir[o + 11] << 8 | dir[o + 12] << 16 | (uint32_t)dir[o + 13] << 24;
			free(dir);
			return 0;
		}
		o += len;
	}
	free(dir);
	return -1;
}

/* the PS2 serial ("SLES-50725") from SYSTEM.CNF's BOOT2 line; "" when it isn't a PS2 disc */
static void read_serial(int fd, char* serial, size_t size)
{
	serial[0] = 0;
	uint8_t pvd[2048];
	if (pread(fd, pvd, sizeof pvd, 16 * 2048) != (ssize_t)sizeof pvd || memcmp(pvd + 1, "CD001", 5))
		return;
	uint32_t lba, len;
	if (find_root_file(fd, pvd, "SYSTEM.CNF", &lba, &len) != 0 || len == 0 || len > 4096)
		return;
	char cnf[2049];
	if (pread(fd, cnf, 2048, (off_t)lba * 2048) != 2048)
		return;
	cnf[len < 2048 ? len : 2048] = 0;
	char* b = strstr(cnf, "BOOT2");
	if (!b)
		return; /* PS1 discs have BOOT, PS2 ones BOOT2 */
	char* eol = strpbrk(b, "\r\n");
	if (eol)
		*eol = 0;
	char* name = strrchr(b, '\\');
	if (!name)
		name = strrchr(b, ':');
	if (!name)
		return;
	name++;
	char out[16];
	size_t k = 0;
	for (char* p = name; *p && *p != ';' && k < sizeof out - 1; p++)
	{
		if (*p == '.')
			continue;
		out[k++] = *p == '_' ? '-' : (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
	}
	out[k] = 0;
	if (k >= 9)
		str_copy(serial, size, out);
}

/* ---------- the game's name ---------- */
static void game_title(const char* serial, char* title, size_t size)
{
	str_copy(title, size, serial);
	FILE* f = fopen(GAMEDB, "rb");
	if (!f)
		return;
	char line[1024], key[32], name[256] = "", name_en[256] = "";
	snprintf(key, sizeof key, "%s:", serial);
	int in = 0;
	while (fgets(line, sizeof line, f))
	{
		if (!in)
		{
			if (!strncmp(line, key, strlen(key)))
				in = 1;
			continue;
		}
		if (line[0] != ' ')
			break; /* the next serial */
		char* q;
		const int en = !strncmp(line, "  name-en:", 10);
		if ((en || !strncmp(line, "  name:", 7)) && (q = strchr(line, '"')))
		{
			char* e = strrchr(q + 1, '"');
			if (e && e > q)
			{
				*e = 0;
				str_copy(en ? name_en : name, sizeof name, q + 1);
			}
		}
	}
	fclose(f);
	if (name_en[0])
		str_copy(title, size, name_en);
	else if (name[0])
		str_copy(title, size, name);
}

/* ---------- the cover ---------- */
static int get_cover(const char* serial, char* path, size_t size)
{
	snprintf(path, size, COVERS "/%s.jpg", serial);
	struct stat st;
	if (stat(path, &st) == 0 && st.st_size > 1024)
		return 0;
	char url[256], tmp[300];
	snprintf(url, sizeof url, "/xlenore/ps2-covers/main/covers/default/%s.jpg", serial);
	snprintf(tmp, sizeof tmp, "%s.part", path);
	mkdir(COVERS, 0777);
	FILE* f = fopen(tmp, "wb");
	if (!f)
		return -1;
	int status = 0;
	const int r = dw_https_get("raw.githubusercontent.com", url, NULL, f, &status);
	const int ok = fclose(f) == 0 && r == 0 && status == 200;
	log_line("cover %s: %s (status %d)", serial, ok ? "downloaded" : "not downloaded", status);
	if (!ok || stat(tmp, &st) != 0 || st.st_size < 1024)
	{
		unlink(tmp);
		return -1;
	}
	chmod(tmp, 0777);
	return rename(tmp, path);
}

/* ---------- SteamGridDB art (only with the user's own API key in steamgriddb-key.txt) ---------- */
#define SGDB_KEY WORK "/steamgriddb-key.txt"
#define ART WORK "/art"
#define SGDB_FILTER "types=static&nsfw=false&humor=false&epilepsy=false"

/* "Authorization: Bearer <key>" from the first long run of letters and digits in the key file; -1 without one */
static int sgdb_auth(char* auth, size_t size)
{
	char* k = read_file(SGDB_KEY, NULL);
	if (!k)
		return -1;
	int rc = -1;
	for (char* p = k; *p; p++)
	{
		size_t n = 0;
		while ((p[n] >= '0' && p[n] <= '9') || (p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z'))
			n++;
		if (n >= 24 && n < 128)
		{
			p[n] = 0;
			snprintf(auth, size, "Authorization: Bearer %s", p);
			rc = 0;
			break;
		}
		if (n)
			p += n - 1;
	}
	free(k);
	return rc;
}

static int split_url(const char* url, char* host, size_t hs, char* path, size_t ps)
{
	if (strncmp(url, "https://", 8))
		return -1;
	const char* h = url + 8;
	const char* s = strchr(h, '/');
	if (!s || (size_t)(s - h) >= hs)
		return -1;
	memcpy(host, h, (size_t)(s - h));
	host[s - h] = 0;
	str_copy(path, ps, s);
	return 0;
}

/* GET into a file; 0 only for a 200 with at least min_bytes */
static int fetch(const char* url, const char* auth, const char* dst, size_t min_bytes)
{
	char host[128], path[1024], tmp[300];
	if (split_url(url, host, sizeof host, path, sizeof path) != 0)
		return -1;
	snprintf(tmp, sizeof tmp, "%s.part", dst);
	FILE* f = fopen(tmp, "wb");
	if (!f)
		return -1;
	int status = 0;
	const int r = dw_https_get(host, path, auth, f, &status);
	int ok = fclose(f) == 0 && r == 0 && status == 200;
	struct stat st;
	ok = ok && stat(tmp, &st) == 0 && (size_t)st.st_size >= min_bytes;
	if (!ok)
	{
		if (status != 200)
			log_line("get %s: status %d", url, status);
		unlink(tmp);
		return status == 200 ? -1 : -status - 1000;
	}
	chmod(tmp, 0777);
	return rename(tmp, dst);
}

static js_node* sgdb_json(const char* route, const char* auth)
{
	char url[1024];
	snprintf(url, sizeof url, "https://www.steamgriddb.com/api/v2%s", route);
	const char* tmp = WORK "/sgdb.json";
	if (fetch(url, auth, tmp, 2) != 0)
		return NULL;
	size_t n = 0;
	char* text = read_file(tmp, &n);
	unlink(tmp);
	if (!text)
		return NULL;
	js_node* root = js_parse(text, n);
	free(text);
	if (!root || !js_is_true(js_get(root, "success")))
	{
		js_free(root);
		return NULL;
	}
	return root;
}

static void pct_encode(char* dst, size_t size, const char* src)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t k = 0;
	for (const unsigned char* p = (const unsigned char*)src; *p && k + 4 < size; p++)
	{
		if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_' || *p == '.')
			dst[k++] = (char)*p;
		else
		{
			dst[k++] = '%';
			dst[k++] = hex[*p >> 4];
			dst[k++] = hex[*p & 15];
		}
	}
	dst[k] = 0;
}

/* the SteamGridDB game for this name: its id, 0 when none */
static long long sgdb_find(const char* auth, const char* name, char* matched, size_t msize)
{
	char enc[512], route[600];
	pct_encode(enc, sizeof enc, name);
	snprintf(route, sizeof route, "/search/autocomplete/%s", enc);
	js_node* root = sgdb_json(route, auth);
	long long id = 0;
	const js_node* data = root ? js_get(root, "data") : NULL;
	const js_node* first = data && data->type == JS_ARR ? data->child : NULL;
	if (first && js_int64(js_get(first, "id"), &id) == 0)
	{
		const char* nm = js_str(js_get(first, "name"));
		str_copy(matched, msize, nm ? nm : "?");
	}
	js_free(root);
	return id;
}

/* the best-scored image of a kind (grids/heroes/logos), preferring one at least min_w wide among the top five */
static int sgdb_pick(const char* auth, const char* kind, long long id, const char* query, int min_w, char* url, size_t usize)
{
	char route[400];
	snprintf(route, sizeof route, "/%s/game/%lld?%s", kind, id, query);
	js_node* root = sgdb_json(route, auth);
	if (!root)
		return -1;
	const js_node* data = js_get(root, "data");
	const js_node* pick = NULL;
	int k = 0;
	for (const js_node* n = data && data->type == JS_ARR ? data->child : NULL; n && k < 5; n = n->next, k++)
	{
		long long w = 0;
		js_int64(js_get(n, "width"), &w);
		if (!pick)
			pick = n;
		if (min_w && w >= min_w)
		{
			pick = n;
			break;
		}
	}
	const char* u = pick ? js_str(js_get(pick, "url")) : NULL;
	if (u)
		str_copy(url, usize, u);
	js_free(root);
	return u ? 0 : 1; /* 1 = the game has none */
}

/* hero, logo and square grid for a disc, kept in art/<serial>/ (".none" = SteamGridDB has none); "" for the ones missing */
static void sgdb_art(const char* serial, const char* title, char* hero, char* logo, char* grid, size_t size)
{
	hero[0] = logo[0] = grid[0] = 0;
	char auth[256];
	if (sgdb_auth(auth, sizeof auth) != 0)
		return; /* no key: covers only, as before */
	char dir[160], idfile[200];
	mkdir(ART, 0777);
	snprintf(dir, sizeof dir, ART "/%s", serial);
	mkdir(dir, 0777);
	snprintf(idfile, sizeof idfile, "%s/game-id.txt", dir);
	long long id = -1;
	char* idt = read_file(idfile, NULL);
	if (idt)
	{
		id = atoll(idt);
		free(idt);
	}
	if (id < 0)
	{
		char matched[256] = "";
		id = sgdb_find(auth, title, matched, sizeof matched);
		if (!id)
		{
			/* "Rally Fusion - Race of Champions" -> "Rally Fusion" */
			char shortname[256];
			str_copy(shortname, sizeof shortname, title);
			char* dash = strstr(shortname, " - ");
			if (dash)
			{
				*dash = 0;
				id = sgdb_find(auth, shortname, matched, sizeof matched);
			}
		}
		log_line("SteamGridDB: %s -> %s (game %lld)", title, id ? matched : "not found", id);
		char line[32];
		snprintf(line, sizeof line, "%lld\n", id);
		write_file(idfile, line, strlen(line));
	}
	if (id <= 0)
		return;
	static const struct { const char* kind; const char* file; const char* query; int min_w; } kinds[] = {
		{"heroes", "hero", SGDB_FILTER "&mimes=image/png,image/jpeg", 3840},
		{"logos", "logo", SGDB_FILTER "&mimes=image/png", 0},
		{"grids", "grid", SGDB_FILTER "&mimes=image/png,image/jpeg&dimensions=512x512,1024x1024", 0},
	};
	char* outs[3] = {hero, logo, grid};
	for (int i = 0; i < 3; i++)
	{
		char path[220], none[230];
		snprintf(path, sizeof path, "%s/%s", dir, kinds[i].file);
		snprintf(none, sizeof none, "%s.none", path);
		struct stat st;
		if (stat(path, &st) == 0 && st.st_size > 0)
		{
			str_copy(outs[i], size, path);
			continue;
		}
		if (stat(none, &st) == 0)
			continue;
		char url[1024];
		const int r = sgdb_pick(auth, kinds[i].kind, id, kinds[i].query, kinds[i].min_w, url, sizeof url);
		if (r == 1)
			write_file(none, "", 0);
		else if (r == 0 && fetch(url, NULL, path, 512) == 0)
		{
			log_line("SteamGridDB %s: %s", kinds[i].file, url);
			str_copy(outs[i], size, path);
		}
	}
}

/* ---------- libretro thumbnails (no account or key): the title screen and an in-game screenshot ---------- */
#define REDUMP_DAT WORK "/ps2-redump.dat"
#define REDUMP_URL "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/redump/Sony%20-%20PlayStation%202.dat"

/* the Redump name libretro files the disc's pictures under ("V-Rally 3 (Europe) (En,Fr,De,Es,It)"), from its serial */
static int redump_name(const char* serial, char* name, size_t size)
{
	struct stat st;
	const time_t now = time(NULL);
	if (stat(REDUMP_DAT, &st) != 0 || st.st_size < 100000 || now - st.st_mtime > 60L * 86400)
	{
		const int r = fetch(REDUMP_URL, NULL, REDUMP_DAT, 100000);
		log_line("libretro database: %s", r == 0 ? "downloaded" : "not downloaded");
		if (r != 0 && stat(REDUMP_DAT, &st) != 0)
			return -1;
	}
	FILE* f = fopen(REDUMP_DAT, "rb");
	if (!f)
		return -1;
	char line[1024], last[512] = "", want[64];
	snprintf(want, sizeof want, "\tserial \"%s\"", serial);
	int found = -1;
	while (fgets(line, sizeof line, f))
	{
		if (!strncmp(line, "\tname \"", 7))
		{
			char* e = strrchr(line + 7, '"');
			if (e)
			{
				*e = 0;
				str_copy(last, sizeof last, line + 7);
			}
		}
		else if (!strncmp(line, want, strlen(want)) && line[strlen(want)] == '\n' && last[0])
		{
			str_copy(name, size, last);
			found = 0;
			break;
		}
	}
	fclose(f);
	return found;
}

/* art/<serial>/snap and title (".none" = libretro has none); "" for the ones missing */
static void libretro_art(const char* serial, char* snap, char* title, size_t size)
{
	snap[0] = title[0] = 0;
	char dir[160];
	mkdir(ART, 0777);
	snprintf(dir, sizeof dir, ART "/%s", serial);
	mkdir(dir, 0777);
	static const char* const kinds[2][2] = {{"Named_Snaps", "snap"}, {"Named_Titles", "title"}};
	char* outs[2] = {snap, title};
	char name[512] = "";
	int looked = 0;
	for (int i = 0; i < 2; i++)
	{
		char path[220], none[230];
		snprintf(path, sizeof path, "%s/%s", dir, kinds[i][1]);
		snprintf(none, sizeof none, "%s.none", path);
		struct stat st;
		if (stat(path, &st) == 0 && st.st_size > 0)
		{
			str_copy(outs[i], size, path);
			continue;
		}
		if (stat(none, &st) == 0)
			continue;
		if (!looked)
		{
			looked = 1;
			if (redump_name(serial, name, sizeof name) != 0)
				log_line("libretro: %s not in the database", serial);
			else
			{
				/* libretro's file names: these characters become '_' */
				for (char* p = name; *p; p++)
					if (strchr("&*/:`<>?\\|\"", *p))
						*p = '_';
				log_line("libretro: %s = %s", serial, name);
			}
		}
		if (!name[0])
			return;
		char enc[1024], url[1300];
		pct_encode(enc, sizeof enc, name);
		snprintf(url, sizeof url, "https://thumbnails.libretro.com/Sony%%20-%%20PlayStation%%202/%s/%s.png", kinds[i][0], enc);
		const int r = fetch(url, NULL, path, 512);
		if (r == 0)
			str_copy(outs[i], size, path);
		else if (r == -404 - 1000)
			write_file(none, "", 0);
	}
}

/* ---------- ShadowMount+ ---------- */
static int smp_post(const char* route, const char* body, char* reply, size_t reply_size)
{
	if (reply_size)
		reply[0] = 0;
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	struct timeval tv = {15, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(10101);
	a.sin_addr.s_addr = htonl(0x7F000001);
	if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0)
	{
		log_line("ShadowMount+ API not reachable (errno %d)", errno);
		close(fd);
		return -1;
	}
	char req[1024];
	const int n = snprintf(req, sizeof req,
		"POST %s HTTP/1.1\r\nHost: 127.0.0.1:10101\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
		route, strlen(body), body);
	send(fd, req, (size_t)n, 0);
	char buf[2048];
	size_t got = 0;
	ssize_t r;
	while (got < sizeof buf - 1 && (r = recv(fd, buf + got, sizeof buf - 1 - got, 0)) > 0)
		got += (size_t)r;
	buf[got] = 0;
	close(fd);
	int code = 0;
	sscanf(buf, "HTTP/1.%*d %d", &code);
	const char* json = strstr(buf, "\r\n\r\n");
	if (reply_size)
		str_copy(reply, reply_size, json ? json + 4 : "");
	return code;
}

/* ---------- the unlock list ---------- */
static void whitelist_add(const char* tid)
{
	size_t n = 0;
	char* w = read_file(WHITELIST, &n);
	if (w && strstr(w, tid))
	{
		free(w);
		return;
	}
	FILE* f = fopen(WHITELIST, "ab");
	if (f)
	{
		if (w && n && w[n - 1] != '\n')
			fputc('\n', f);
		fprintf(f, "%s\n", tid);
		fclose(f);
		log_line("%s added to " WHITELIST, tid);
	}
	free(w);
}

/* ---------- tiles ---------- */
static void json_escape(char* dst, size_t size, const char* src)
{
	size_t k = 0;
	for (const char* p = src; *p && k + 7 < size; p++)
	{
		const unsigned char c = (unsigned char)*p;
		if (c == '"' || c == '\\')
		{
			dst[k++] = '\\';
			dst[k++] = (char)c;
		}
		else if (c < 0x20)
			k += (size_t)snprintf(dst + k, size - k, "\\u%04x", c);
		else
			dst[k++] = (char)c;
	}
	dst[k] = 0;
}

static char* replace_all(const char* s, const char* from, const char* to)
{
	sbuf b;
	sb_init(&b);
	const size_t fl = strlen(from);
	for (const char* p = s;;)
	{
		const char* q = strstr(p, from);
		if (!q)
		{
			sb_puts(&b, p);
			break;
		}
		sb_append(&b, p, (size_t)(q - p));
		sb_puts(&b, to);
		p = q + fl;
	}
	sb_append(&b, "", 1);
	return b.data;
}

static void remove_tile(int slot, const char* why)
{
	const char* tid = kSlots[slot];
	char dir[128];
	snprintf(dir, sizeof dir, HB "/%s", tid);
	rm_tree(dir); /* only our own files: links, disc-tile.txt, the generated sce_sys */
	char reply[512], body[64];
	snprintf(body, sizeof body, "{\"title_id\":\"%s\"}", tid);
	const int code = smp_post("/api/v1/games/uninstall", body, reply, sizeof reply);
	log_line("tile %s removed (%s): uninstall HTTP %d %s", tid, why, code, reply);
	g_slot_stale[slot] = code == 409 || code <= 0; /* busy (the game is running) or no answer: again later */
}

static int make_tile(int drive, const char* serial)
{
	int slot = -1;
	for (int k = 0; k < NSLOTS && slot < 0; k++)
	{
		const int s = (g_next_slot + k) % NSLOTS;
		if (!g_slot_busy[s] && !g_slot_stale[s])
			slot = s;
	}
	if (slot < 0)
	{
		log_line("no free tile slot for /dev/cd%d", drive);
		return -1;
	}
	g_next_slot = (slot + 1) % NSLOTS; /* round robin, kept across restarts: a slot's old art is long gone from the home screen's cache */
	{
		char ns[16];
		snprintf(ns, sizeof ns, "%d\n", g_next_slot);
		write_file(WORK "/next-slot.txt", ns, strlen(ns));
	}
	const char* tid = kSlots[slot];
	char title[256], cover[256], dir[128], p[256], q[256];
	game_title(serial, title, sizeof title);
	const int have_cover = get_cover(serial, cover, sizeof cover) == 0;

	snprintf(dir, sizeof dir, HB "/%s", tid);
	rm_tree(dir);
	mkdir(dir, 0777);
	snprintf(p, sizeof p, "%s/sce_module", dir);
	mkdir(p, 0777);
	int ok = link_or_copy(APP_SRC "/eboot.bin", (snprintf(p, sizeof p, "%s/eboot.bin", dir), p)) == 0;
	DIR* d = opendir(APP_SRC "/sce_module");
	if (d)
	{
		struct dirent* e;
		while ((e = readdir(d)))
		{
			if (e->d_name[0] == '.')
				continue;
			snprintf(p, sizeof p, APP_SRC "/sce_module/%s", e->d_name);
			snprintf(q, sizeof q, "%s/sce_module/%s", dir, e->d_name);
			ok = ok && link_or_copy(p, q) == 0;
		}
		closedir(d);
	}
	char devline[32];
	snprintf(devline, sizeof devline, "/dev/cd%d\n", drive);
	snprintf(p, sizeof p, "%s/disc-tile.txt", dir);
	ok = ok && write_file(p, devline, strlen(devline)) == 0;

	/* sce_sys is made beside, then renamed in: ShadowMount+ never sees half of it */
	char sys_tmp[160], sys[160];
	snprintf(sys_tmp, sizeof sys_tmp, "%s/sce_sys.new", dir);
	snprintf(sys, sizeof sys, "%s/sce_sys", dir);
	mkdir(sys_tmp, 0777);
	char* pj = read_file(APP_SRC "/sce_sys/param.json", NULL);
	if (pj)
	{
		char esc[512], tname[600], content[64], concept[16];
		json_escape(esc, sizeof esc, title);
		snprintf(tname, sizeof tname, "\"titleName\": \"%s\"", esc);
		snprintf(content, sizeof content, "UP9000-%s_00-PS5SX2DISCTILE%02d", tid, slot + 1);
		snprintf(concept, sizeof concept, "%s", tid + 4);
		char* a = replace_all(pj, "\"titleName\": \"PS5SX2 Disc\"", tname);
		char* b = replace_all(a, "UP9000-PPSA99213_00-PCSX2ORBISDISC1", content);
		char* c = replace_all(b, "\"99213\"", (snprintf(q, sizeof q, "\"%s\"", concept), q));
		char* e = replace_all(c, "PPSA99213", tid);
		snprintf(p, sizeof p, "%s/param.json", sys_tmp);
		ok = ok && write_file(p, e, strlen(e)) == 0;
		free(a); free(b); free(c); free(e); free(pj);
	}
	else
		ok = 0;
	char icon[200], pic[200];
	snprintf(icon, sizeof icon, "%s/icon0.png", sys_tmp);
	snprintf(pic, sizeof pic, "%s/pic0.dds", sys_tmp);
	char hero[256], logo[256], grid[256], snap[256], tscreen[256], used[96] = "the emulator's own";
	sgdb_art(serial, title, hero, logo, grid, sizeof hero);
	libretro_art(serial, snap, tscreen, sizeof snap);
	/* the background screenshot: the in-game one or the title screen, whichever comes up this time */
	const char* shot = snap[0] ? snap : (tscreen[0] ? tscreen : NULL);
	if (snap[0] && tscreen[0] && (time(NULL) ^ getpid()) & 1)
		shot = tscreen;
	const TileArt ta = {have_cover ? cover : NULL, hero[0] ? hero : NULL, logo[0] ? logo : NULL, grid[0] ? grid : NULL,
		shot, tscreen[0] ? tscreen : NULL, "/preinst/common/font/SST-Bold.otf", "PlayStation 2"};
	int art = -1;
	if (ta.cover || ta.hero || ta.grid || ta.shot || ta.title)
	{
		art = tile_make_art(&ta, icon, pic, used, sizeof used);
		if (art != 0)
		{
			log_line("art for %s couldn't be made (%d); the emulator's art instead", serial, art);
			str_copy(used, sizeof used, "the emulator's own");
		}
	}
	if (art != 0)
	{
		ok = ok && copy_file(APP_SRC "/sce_sys/icon0.png", icon) == 0;
		ok = ok && copy_file(APP_SRC "/sce_sys/pic0.dds", pic) == 0;
	}
	chmod(icon, 0777);
	chmod(pic, 0777);
	if (!ok)
	{
		log_line("tile %s for %s: couldn't be written", tid, serial);
		rm_tree(dir);
		return -1;
	}
	age(icon);
	age(pic);
	snprintf(p, sizeof p, "%s/param.json", sys_tmp);
	age(p);
	age(sys_tmp);
	rename(sys_tmp, sys);
	age(sys);
	age(dir);
	whitelist_add(tid);

	g_slot_busy[slot] = drive + 1;
	g_drv[drive].slot = slot;
	char reply[512];
	const int code = smp_post("/api/v1/scan", "{\"reset_attempts\":true}", reply, sizeof reply);
	log_line("tile %s: %s (%s) from /dev/cd%d, art: %s; scan HTTP %d %s", tid, title, serial, drive, used, code, reply);
	notify("PS2 disc: %s\nIts tile is on the home screen in a moment.", title);
	return 0;
}

/* ---------- the drives ---------- */
static void poll_drive(int i)
{
	Drive* d = &g_drv[i];
	char dev[16];
	snprintf(dev, sizeof dev, "/dev/cd%d", i);
	struct stat st;
	off_t media = 0;
	int fd = stat(dev, &st) == 0 ? open(dev, O_RDONLY) : -1;
	if (fd >= 0 && (ioctl(fd, DIOCGMEDIASIZE, &media) != 0 || media <= 0))
	{
		close(fd);
		fd = -1;
	}
	if (fd < 0)
	{
		if (d->present)
		{
			log_line("%s: disc out", dev);
			if (d->slot >= 0)
			{
				g_slot_busy[d->slot] = 0;
				remove_tile(d->slot, "disc out");
			}
		}
		memset(d, 0, sizeof *d);
		d->slot = -1;
		return;
	}
	if (d->present && media == d->media && (d->slot >= 0 || !d->serial[0] || d->fails >= 3))
	{
		close(fd); /* the same disc: nothing to read */
		return;
	}
	if (!d->present || media != d->media)
	{
		if (d->slot >= 0)
		{
			g_slot_busy[d->slot] = 0;
			remove_tile(d->slot, "disc changed");
			d->slot = -1;
		}
		d->fails = 0;
		read_serial(fd, d->serial, sizeof d->serial);
		d->present = 1;
		d->media = media;
		log_line("%s: disc in, %lld bytes, %s", dev, (long long)media, d->serial[0] ? d->serial : "not a PS2 game disc");
	}
	close(fd);
	if (d->serial[0] && d->slot < 0 && make_tile(i, d->serial) != 0)
		d->fails++;
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	/* one watcher only: the second one finds the port taken and leaves */
	const int lock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(LOCK_PORT);
	a.sin_addr.s_addr = htonl(0x7F000001);
	if (lock < 0 || bind(lock, (struct sockaddr*)&a, sizeof a) != 0)
	{
		printf("[discwatch] already running\n");
		return 0;
	}
	listen(lock, 1);
	mkdir("/data/PCSX2", 0777);
	mkdir(WORK, 0777);
	g_log = fopen(WORK "/discwatch.log", "a");
	log_line("discwatch " VERSION " started");
	char* ns = read_file(WORK "/next-slot.txt", NULL);
	if (ns)
	{
		g_next_slot = atoi(ns) % NSLOTS;
		if (g_next_slot < 0)
			g_next_slot = 0;
		free(ns);
	}
	else
		g_next_slot = 3; /* 1.0/1.1 used PPSA99221-23 */
	for (int i = 0; i < MAX_DRIVES; i++)
		g_drv[i].slot = -1;
	/* tiles left from before (a restart with a disc in): away, and made again for the discs still there */
	for (int s = 0; s < NSLOTS; s++)
	{
		char dir[128];
		snprintf(dir, sizeof dir, HB "/%s", kSlots[s]);
		struct stat st;
		if (stat(dir, &st) == 0)
			remove_tile(s, "left from before");
	}
	time_t key_mtime = -1;
	for (;;)
	{
		/* a new or changed SteamGridDB key: the tiles of the discs already in are made again with its art */
		struct stat ks;
		const time_t km = stat(SGDB_KEY, &ks) == 0 ? ks.st_mtime : 0;
		if (key_mtime >= 0 && km != key_mtime)
		{
			log_line("SteamGridDB key %s: tiles made again", km ? "changed" : "removed");
			for (int i = 0; i < MAX_DRIVES; i++)
				if (g_drv[i].slot >= 0)
				{
					g_slot_busy[g_drv[i].slot] = 0;
					remove_tile(g_drv[i].slot, "new art");
					g_drv[i].slot = -1;
					g_drv[i].fails = 0;
				}
		}
		key_mtime = km;
		for (int i = 0; i < MAX_DRIVES; i++)
			poll_drive(i);
		for (int s = 0; s < NSLOTS; s++)
			if (g_slot_stale[s] && !g_slot_busy[s])
				remove_tile(s, "retry");
		sleep_ms(POLL_MS);
	}
}
