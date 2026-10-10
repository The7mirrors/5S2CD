/* discwatch: one HTTPS GET into a file (HTTP/1.0, so no chunked bodies), with the TLS 1.2 Mbed TLS build PS5SX2 uses
   (ps5/third_party/mbedtls/port) and the installer's CA bundle. The server's certificate is always checked. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "dw_https.h"
#include "log.h"
#include "net.h"
#include "util.h"

extern const unsigned char dw_cacert[];
extern const size_t dw_cacert_len;

static int g_ready;
static mbedtls_x509_crt g_roots;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_ssl_config g_conf;

static int entropy_src(void* data, unsigned char* out, size_t len, size_t* olen)
{
	size_t done = 0;
	while (done < len)
	{
		size_t chunk = len - done > 256 ? 256 : len - done, got = chunk;
		int mib[2] = {CTL_KERN, KERN_ARND};
		if (sysctl(mib, 2, out + done, &got, NULL, 0) != 0 || got == 0 || got > chunk)
			return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
		done += got;
	}
	*olen = len;
	return 0;
}

static int setup(void)
{
	if (g_ready)
		return g_ready > 0 ? 0 : -1;
	g_ready = -1;
	mbedtls_x509_crt_init(&g_roots);
	mbedtls_entropy_init(&g_entropy);
	mbedtls_ctr_drbg_init(&g_drbg);
	mbedtls_ssl_config_init(&g_conf);
	int r = mbedtls_x509_crt_parse(&g_roots, dw_cacert, dw_cacert_len);
	if (r < 0)
	{
		log_line("https: CA bundle not parsed (-0x%x)", -r);
		return -1;
	}
	if ((r = mbedtls_entropy_add_source(&g_entropy, entropy_src, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG)) != 0 ||
		(r = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char*)"discwatch", 9)) != 0 ||
		(r = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
	{
		log_line("https: setup failed (-0x%x)", -r);
		return -1;
	}
	mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&g_conf, &g_roots, NULL);
	mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
	g_ready = 1;
	return 0;
}

static int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
	const ssize_t r = net_send(*(int*)ctx, buf, len);
	return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)r;
}
static int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
	const ssize_t r = net_recv(*(int*)ctx, buf, len);
	return r < 0 ? (net_timed_out() ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR) : (int)r;
}

int dw_https_get(const char* host, const char* path, const char* extra_header, FILE* out, int* status)
{
	*status = 0;
	if (setup() != 0)
		return -1;
	const int fd = net_connect(host, 443, 10000, 15000);
	if (fd < 0)
	{
		log_line("https: %s: no connection (%s)", host, err_get());
		return -1;
	}
	mbedtls_ssl_context ssl;
	mbedtls_ssl_init(&ssl);
	int sock = fd, rc = -1, r;
	if ((r = mbedtls_ssl_setup(&ssl, &g_conf)) != 0 || (r = mbedtls_ssl_set_hostname(&ssl, host)) != 0)
		goto done;
	mbedtls_ssl_set_bio(&ssl, &sock, bio_send, bio_recv, NULL);
	while ((r = mbedtls_ssl_handshake(&ssl)) != 0)
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
			goto done;
	char req[1024];
	const int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PS5SX2-discwatch/1.1\r\n%s%sConnection: close\r\n\r\n",
		path, host, extra_header ? extra_header : "", extra_header ? "\r\n" : "");
	for (int sent = 0; sent < n;)
	{
		r = mbedtls_ssl_write(&ssl, (const unsigned char*)req + sent, (size_t)(n - sent));
		if (r <= 0 && r != MBEDTLS_ERR_SSL_WANT_WRITE)
			goto done;
		if (r > 0)
			sent += r;
	}
	/* headers first (up to 16 KB), then the body straight into the file */
	char hdr[16384];
	size_t hl = 0;
	char* body = NULL;
	unsigned char buf[16384];
	for (;;)
	{
		r = mbedtls_ssl_read(&ssl, buf, sizeof buf);
		if (r == MBEDTLS_ERR_SSL_WANT_READ)
			continue;
		if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			break;
		if (r < 0)
			goto done;
		if (body)
		{
			if (fwrite(buf, 1, (size_t)r, out) != (size_t)r)
				goto done;
			continue;
		}
		const size_t take = (size_t)r < sizeof hdr - 1 - hl ? (size_t)r : sizeof hdr - 1 - hl;
		memcpy(hdr + hl, buf, take);
		hl += take;
		hdr[hl] = 0;
		char* end = strstr(hdr, "\r\n\r\n");
		if (!end)
		{
			if (hl >= sizeof hdr - 1)
				goto done;
			continue;
		}
		sscanf(hdr, "HTTP/1.%*d %d", status);
		body = end + 4;
		const size_t in_hdr = hl - (size_t)(body - hdr);
		if (fwrite(body, 1, in_hdr, out) != in_hdr)
			goto done;
		if (take < (size_t)r && fwrite(buf + take, 1, (size_t)r - take, out) != (size_t)r - take)
			goto done;
	}
	rc = body ? 0 : -1;
	r = 0;
done:
	if (r < 0)
	{
		char e[128];
		mbedtls_strerror(r, e, sizeof e);
		log_line("https: %s: %s (-0x%x)", host, e, -r);
	}
	mbedtls_ssl_close_notify(&ssl);
	mbedtls_ssl_free(&ssl);
	net_close(fd);
	return rc;
}
