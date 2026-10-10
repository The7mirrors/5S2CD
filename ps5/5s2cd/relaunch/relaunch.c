// PS5SX2 Disc relaunch helper (2026-10-09, AI-assisted). The app's own LoadExec never came back with the public RADV
// driver, so "back to the menu" sends this payload to the ELF loader (127.0.0.1:9021) and closes; this waits until the
// app has gone, then starts it again from the home screen's way in, which lands on the shelf with the last game picked.
// Built with the ps5-payload-dev SDK. SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
	uint32_t sz;
	uint32_t user_id;
	uint32_t app_opt;
	uint64_t crash_report;
	uint32_t check_flag;
} LncAppParam;

int sceLncUtilLaunchApp(const char* title_id, const char* argv[], LncAppParam* param);
int sceLncUtilGetAppId(const char* title_id);
int sceUserServiceInitialize(void* params);
int sceUserServiceGetForegroundUser(int32_t* user);
int sceUserServiceGetInitialUser(int32_t* user);

static const char kTitle[] = "PPSA99213";

int main(void)
{
	// Up to 15 s for the app to be gone (its id stops answering), then a moment for the system to settle.
	for (int i = 0; i < 150 && sceLncUtilGetAppId(kTitle) > 0; i++)
		usleep(100000);
	usleep(800000);
	int32_t user = -1;
	sceUserServiceInitialize(NULL);
	if (sceUserServiceGetForegroundUser(&user) != 0 || user < 0)
		sceUserServiceGetInitialUser(&user);
	LncAppParam p;
	memset(&p, 0, sizeof(p));
	p.sz = sizeof(p);
	p.user_id = (uint32_t)user;
	int rc = -1;
	for (int attempt = 0; attempt < 5; attempt++)
	{
		rc = sceLncUtilLaunchApp(kTitle, NULL, &p);
		printf("[ps5sx2-relaunch] launch %s attempt %d user %d -> 0x%x\n", kTitle, attempt + 1, user, (unsigned)rc);
		if (rc >= 0)
			break;
		usleep(700000);
	}
	return rc >= 0 ? 0 : 1;
}
