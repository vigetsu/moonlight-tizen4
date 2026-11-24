#include "gamestream.h"
#include "http.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#define UNIQUE_ID "0123456789ABCDEF"
#define DEVICE_NAME "TizenTV"

static X509 *g_cert;
static EVP_PKEY *g_key;
static char *g_cert_hex;   /* hex of the PEM certificate, sent during pairing */
static SSL_CTX *g_tls;

#define SETERR(...) do { if (err && errlen) snprintf(err, errlen, __VA_ARGS__); } while (0)

/* ---------- helpers ---------- */

static void to_hex(const unsigned char *in, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 15];
    }
    out[n * 2] = 0;
}

static size_t from_hex(const char *in, unsigned char *out, size_t max) {
    size_t n = strlen(in) / 2;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) {
        unsigned int b;
        sscanf(in + i * 2, "%2x", &b);
        out[i] = (unsigned char)b;
    }
    return n;
}

static void new_uuid(char out[37]) {
    unsigned char r[16];
    RAND_bytes(r, sizeof(r));
    r[6] = (r[6] & 0x0f) | 0x40;
    r[8] = (r[8] & 0x3f) | 0x80;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13],
             r[14], r[15]);
}

static bool aes_ecb(const unsigned char *key, const unsigned char *in, int len, unsigned char *out,
                    bool enc) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n1 = 0, n2 = 0;
    bool ok = c && EVP_CipherInit_ex(c, EVP_aes_128_ecb(), NULL, key, NULL, enc ? 1 : 0) == 1 &&
              EVP_CIPHER_CTX_set_padding(c, 0) == 1 &&
              EVP_CipherUpdate(c, out, &n1, in, len) == 1 &&
              EVP_CipherFinal_ex(c, out + n1, &n2) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok;
}

static bool sign_sha256(const unsigned char *msg, size_t len, unsigned char **sig, size_t *siglen) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = false;
    *sig = NULL;
    if (ctx && EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, g_key) == 1 &&
        EVP_DigestSignUpdate(ctx, msg, len) == 1 && EVP_DigestSignFinal(ctx, NULL, siglen) == 1) {
        *sig = OPENSSL_malloc(*siglen);
        ok = *sig && EVP_DigestSignFinal(ctx, *sig, siglen) == 1;
    }
    EVP_MD_CTX_free(ctx);
    return ok;
}

static bool verify_sha256(X509 *cert, const unsigned char *msg, size_t len,
                          const unsigned char *sig, size_t siglen) {
    EVP_PKEY *pub = X509_get_pubkey(cert);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = pub && ctx && EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pub) == 1 &&
              EVP_DigestVerifyUpdate(ctx, msg, len) == 1 &&
              EVP_DigestVerifyFinal(ctx, sig, siglen) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pub);
    return ok;
}

static void hash(int major, const unsigned char *in, size_t len, unsigned char *out) {
    if (major >= 7) SHA256(in, len, out);
    else SHA1(in, len, out);
}

/* ---------- identity ---------- */

static bool generate_identity(const char *cert_path, const char *key_path) {
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!kctx || EVP_PKEY_keygen_init(kctx) != 1 || EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) != 1 ||
        EVP_PKEY_keygen(kctx, &g_key) != 1) {
        EVP_PKEY_CTX_free(kctx);
        return false;
    }
    EVP_PKEY_CTX_free(kctx);

    g_cert = X509_new();
    X509_set_version(g_cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(g_cert), 0);
    X509_gmtime_adj(X509_getm_notBefore(g_cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(g_cert), 60L * 60 * 24 * 365 * 20);
    X509_set_pubkey(g_cert, g_key);
    X509_NAME *name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"NVIDIA GameStream Client",
                               -1, -1, 0);
    X509_set_subject_name(g_cert, name);
    X509_set_issuer_name(g_cert, name);
    X509_NAME_free(name);
    if (!X509_sign(g_cert, g_key, EVP_sha256())) return false;

    FILE *f = fopen(cert_path, "w");
    if (!f) return false;
    PEM_write_X509(f, g_cert);
    fclose(f);
    f = fopen(key_path, "w");
    if (!f) return false;
    PEM_write_PrivateKey(f, g_key, NULL, NULL, 0, NULL, NULL);
    fclose(f);
    return true;
}

bool gs_init(void) {
    char cert_path[1100], key_path[1100];
    snprintf(cert_path, sizeof(cert_path), "%s/client.pem", config_dir());
    snprintf(key_path, sizeof(key_path), "%s/key.pem", config_dir());

    FILE *cf = fopen(cert_path, "r");
    FILE *kf = fopen(key_path, "r");
    if (cf && kf) {
        g_cert = PEM_read_X509(cf, NULL, NULL, NULL);
        g_key = PEM_read_PrivateKey(kf, NULL, NULL, NULL);
    }
    if (cf) fclose(cf);
    if (kf) fclose(kf);

    if (!g_cert || !g_key) {
        fprintf(stderr, "[gs] creating client certificate in %s\n", config_dir());
        if (!generate_identity(cert_path, key_path)) {
            fprintf(stderr, "[gs] failed to create client certificate\n");
            return false;
        }
    }

    BIO *mem = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(mem, g_cert);
    char *pem;
    long pem_len = BIO_get_mem_data(mem, &pem);
    g_cert_hex = malloc((size_t)pem_len * 2 + 1);
    to_hex((unsigned char *)pem, (size_t)pem_len, g_cert_hex);
    BIO_free(mem);

    g_tls = SSL_CTX_new(TLS_client_method());
    if (!g_tls) return false;
    SSL_CTX_set_verify(g_tls, SSL_VERIFY_NONE, NULL);
    SSL_CTX_use_certificate(g_tls, g_cert);
    SSL_CTX_use_PrivateKey(g_tls, g_key);
    return true;
}

/* ---------- requests ---------- */

/* Sends a request and checks the root status_code. Caller frees resp. */
static bool request(gs_server_t *s, bool https, const char *path_fmt_query, int timeout_ms,
                    http_resp_t *resp, char *err, size_t errlen) {
    char uuid[37], path[16384];
    new_uuid(uuid);
    snprintf(path, sizeof(path), "%s%cuniqueid=" UNIQUE_ID "&uuid=%s", path_fmt_query,
             strchr(path_fmt_query, '?') ? '&' : '?', uuid);
    if (!http_get(s->address, https ? s->https_port : s->http_port, path, https ? g_tls : NULL,
                  timeout_ms, resp)) {
        SETERR("%s bilgisayara bağlanılamadı (%s:%u)", https ? "HTTPS" : "HTTP", s->address,
               https ? s->https_port : s->http_port);
        return false;
    }
    char msg[256];
    int code = xml_status_code(resp->body, msg, sizeof(msg));
    if (resp->status != 200 || (code != -1 && code != 200)) {
        SETERR("Sunucu hatası %d: %s", code != -1 ? code : resp->status, msg[0] ? msg : "bilinmiyor");
        http_resp_free(resp);
        return false;
    }
    return true;
}

void gs_server_setup(gs_server_t *s, const char *address) {
    memset(s, 0, sizeof(*s));
    snprintf(s->address, sizeof(s->address), "%s", address);
    s->http_port = 47989;
    s->https_port = 47984;
}

static void copy_tag(const char *xml, const char *tag, char *out, size_t outlen) {
    char *v = xml_get(xml, tag);
    snprintf(out, outlen, "%s", v ? v : "");
    free(v);
}

static int int_tag(const char *xml, const char *tag, int def) {
    char *v = xml_get(xml, tag);
    int r = v && *v ? atoi(v) : def;
    free(v);
    return r;
}

bool gs_server_info(gs_server_t *s, char *err, size_t errlen) {
    http_resp_t r;
    /* HTTPS tells us reliably whether we're paired; fall back to HTTP if rejected. */
    bool ok = request(s, true, "/serverinfo", 5000, &r, NULL, 0);
    if (!ok && !request(s, false, "/serverinfo", 5000, &r, err, errlen)) return false;

    copy_tag(r.body, "hostname", s->hostname, sizeof(s->hostname));
    copy_tag(r.body, "appversion", s->app_version, sizeof(s->app_version));
    copy_tag(r.body, "GfeVersion", s->gfe_version, sizeof(s->gfe_version));
    s->major_version = atoi(s->app_version);
    s->codec_support = int_tag(r.body, "ServerCodecModeSupport", SCM_H264);
    s->current_game = int_tag(r.body, "currentgame", 0);
    s->paired = ok && int_tag(r.body, "PairStatus", 0) == 1;
    int https_port = int_tag(r.body, "HttpsPort", 0);
    if (https_port > 0) s->https_port = (uint16_t)https_port;
    /* GFE reports a stale currentgame while the host is idle. */
    char state[64];
    copy_tag(r.body, "state", state, sizeof(state));
    if (strstr(state, "_SERVER_BUSY") == NULL) s->current_game = 0;
    http_resp_free(&r);
    return true;
}

bool gs_unpair(gs_server_t *s) {
    http_resp_t r;
    if (!request(s, false, "/unpair", 5000, &r, NULL, 0)) return false;
    http_resp_free(&r);
    s->paired = false;
    return true;
}

static bool pair_step(gs_server_t *s, bool https, const char *query, int timeout_ms, char **out_tag,
                      const char *tag, char *err, size_t errlen) {
    http_resp_t r;
    if (!request(s, https, query, timeout_ms, &r, err, errlen)) return false;
    char *paired = xml_get(r.body, "paired");
    bool ok = paired && strcmp(paired, "1") == 0;
    free(paired);
    if (ok && tag) {
        *out_tag = xml_get(r.body, tag);
        ok = *out_tag != NULL;
    }
    http_resp_free(&r);
    if (!ok) SETERR("Eşleştirme reddedildi");
    return ok;
}

bool gs_pair(gs_server_t *s, const char *pin, char *err, size_t errlen) {
    if (s->paired) return true;
    if (!s->major_version && !gs_server_info(s, err, errlen)) return false;

    bool ok = false;
    char *q = malloc(16384);
    char *plaincert_hex = NULL, *chal_resp_hex = NULL, *pairing_secret_hex = NULL;
    X509 *server_cert = NULL;
    unsigned char *client_sig = NULL;
    const int hash_len = s->major_version >= 7 ? 32 : 20;

    /* 1. Exchange certificates. Sunshine holds this request until the PIN is typed in. */
    unsigned char salt[16];
    char salt_hex[33];
    RAND_bytes(salt, sizeof(salt));
    to_hex(salt, sizeof(salt), salt_hex);
    snprintf(q, 16384, "/pair?devicename=" DEVICE_NAME "&updateState=1&phrase=getservercert&salt=%s&clientcert=%s",
             salt_hex, g_cert_hex);
    if (!pair_step(s, false, q, 5 * 60 * 1000, &plaincert_hex, "plaincert", err, errlen)) goto out;

    {
        size_t pem_len = strlen(plaincert_hex) / 2;
        unsigned char *pem = malloc(pem_len + 1);
        from_hex(plaincert_hex, pem, pem_len);
        pem[pem_len] = 0;
        BIO *bio = BIO_new_mem_buf(pem, (int)pem_len);
        server_cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        BIO_free(bio);
        free(pem);
        if (!server_cert) {
            SETERR("Sunucu sertifikası okunamadı (başka bir eşleştirme sürüyor olabilir)");
            goto out;
        }
    }

    unsigned char salt_pin[20], aes_key[32];
    memcpy(salt_pin, salt, 16);
    memcpy(salt_pin + 16, pin, 4);
    hash(s->major_version, salt_pin, sizeof(salt_pin), aes_key);

    /* 2. Send our encrypted challenge. */
    unsigned char client_chal[16], enc[64];
    char hex[129];
    RAND_bytes(client_chal, sizeof(client_chal));
    aes_ecb(aes_key, client_chal, 16, enc, true);
    to_hex(enc, 16, hex);
    snprintf(q, 16384, "/pair?devicename=" DEVICE_NAME "&updateState=1&clientchallenge=%s", hex);
    if (!pair_step(s, false, q, 10000, &chal_resp_hex, "challengeresponse", err, errlen)) goto out;

    unsigned char chal_resp[64] = {0}, chal_resp_dec[64];
    size_t crl = from_hex(chal_resp_hex, chal_resp, sizeof(chal_resp));
    if (crl < (size_t)hash_len + 16 || crl % 16) {
        SETERR("Geçersiz sunucu yanıtı");
        goto out;
    }
    aes_ecb(aes_key, chal_resp, (int)crl, chal_resp_dec, false);
    unsigned char server_resp_hash[32];
    memcpy(server_resp_hash, chal_resp_dec, (size_t)hash_len);
    const unsigned char *server_chal = chal_resp_dec + hash_len;

    /* 3. Answer the server's challenge. */
    const ASN1_BIT_STRING *our_sig;
    X509_get0_signature(&our_sig, NULL, g_cert);
    unsigned char client_secret[16];
    RAND_bytes(client_secret, sizeof(client_secret));
    {
        size_t sl = (size_t)ASN1_STRING_length(our_sig);
        size_t n = 16 + sl + 16;
        unsigned char *buf = malloc(n);
        memcpy(buf, server_chal, 16);
        memcpy(buf + 16, ASN1_STRING_get0_data(our_sig), sl);
        memcpy(buf + 16 + sl, client_secret, 16);
        unsigned char h[32] = {0};
        hash(s->major_version, buf, n, h);
        free(buf);
        aes_ecb(aes_key, h, 32, enc, true);
        to_hex(enc, 32, hex);
    }
    snprintf(q, 16384, "/pair?devicename=" DEVICE_NAME "&updateState=1&serverchallengeresp=%s", hex);
    if (!pair_step(s, false, q, 10000, &pairing_secret_hex, "pairingsecret", err, errlen)) goto out;

    /* 4. Verify the server knew the PIN and owns its certificate. */
    {
        unsigned char ps[1024];
        size_t psl = from_hex(pairing_secret_hex, ps, sizeof(ps));
        if (psl <= 16) {
            SETERR("Geçersiz eşleştirme yanıtı");
            goto out;
        }
        if (!verify_sha256(server_cert, ps, 16, ps + 16, psl - 16)) {
            SETERR("Sunucu imzası doğrulanamadı (MITM?)");
            goto out;
        }
        const ASN1_BIT_STRING *srv_sig;
        X509_get0_signature(&srv_sig, NULL, server_cert);
        size_t sl = (size_t)ASN1_STRING_length(srv_sig);
        size_t n = 16 + sl + 16;
        unsigned char *buf = malloc(n);
        memcpy(buf, client_chal, 16);
        memcpy(buf + 16, ASN1_STRING_get0_data(srv_sig), sl);
        memcpy(buf + 16 + sl, ps, 16);
        unsigned char h[32];
        hash(s->major_version, buf, n, h);
        free(buf);
        if (memcmp(h, server_resp_hash, (size_t)hash_len) != 0) {
            SETERR("PIN yanlış girildi");
            goto out;
        }
    }

    /* 5. Prove we own our certificate. */
    {
        size_t sl;
        if (!sign_sha256(client_secret, 16, &client_sig, &sl)) {
            SETERR("İmza oluşturulamadı");
            goto out;
        }
        size_t n = 16 + sl;
        unsigned char *buf = malloc(n);
        char *bhex = malloc(n * 2 + 1);
        memcpy(buf, client_secret, 16);
        memcpy(buf + 16, client_sig, sl);
        to_hex(buf, n, bhex);
        snprintf(q, 16384, "/pair?devicename=" DEVICE_NAME "&updateState=1&clientpairingsecret=%s", bhex);
        free(buf);
        free(bhex);
    }
    if (!pair_step(s, false, q, 10000, NULL, NULL, err, errlen)) goto out;

    /* 6. Final check over HTTPS with our client certificate. */
    if (!pair_step(s, true, "/pair?devicename=" DEVICE_NAME "&updateState=1&phrase=pairchallenge", 10000,
                   NULL, NULL, err, errlen))
        goto out;

    s->paired = true;
    ok = true;

out:
    if (!ok) gs_unpair(s);
    free(q);
    free(plaincert_hex);
    free(chal_resp_hex);
    free(pairing_secret_hex);
    X509_free(server_cert);
    OPENSSL_free(client_sig);
    return ok;
}

bool gs_applist(gs_server_t *s, gs_app_t **apps, int *count, char *err, size_t errlen) {
    http_resp_t r;
    *apps = NULL;
    *count = 0;
    if (!request(s, true, "/applist", 10000, &r, err, errlen)) return false;

    int cap = 16;
    gs_app_t *list = calloc((size_t)cap, sizeof(gs_app_t));
    const char *p = r.body;
    while ((p = strstr(p, "<App>")) != NULL) {
        const char *end = strstr(p, "</App>");
        if (!end) break;
        size_t n = (size_t)(end - p);
        char *chunk = malloc(n + 1);
        memcpy(chunk, p, n);
        chunk[n] = 0;
        if (*count == cap) {
            cap *= 2;
            list = realloc(list, (size_t)cap * sizeof(gs_app_t));
        }
        gs_app_t *a = &list[*count];
        memset(a, 0, sizeof(*a));
        a->id = int_tag(chunk, "ID", 0);
        a->hdr = int_tag(chunk, "IsHdrSupported", 0) == 1;
        copy_tag(chunk, "AppTitle", a->title, sizeof(a->title));
        free(chunk);
        if (a->id) (*count)++;
        p = end;
    }
    http_resp_free(&r);
    *apps = list;
    return true;
}

bool gs_launch(gs_server_t *s, STREAM_CONFIGURATION *cfg, int appid, bool sops, bool host_audio,
               char *err, size_t errlen) {
    RAND_bytes((unsigned char *)cfg->remoteInputAesKey, sizeof(cfg->remoteInputAesKey));
    memset(cfg->remoteInputAesIv, 0, sizeof(cfg->remoteInputAesIv));
    unsigned char ivid[4];
    RAND_bytes(ivid, 4);
    memcpy(cfg->remoteInputAesIv, ivid, 4);
    /* rikeyid is the first 4 IV bytes read as a big-endian integer. */
    int rikeyid = (int)(((uint32_t)ivid[0] << 24) | ((uint32_t)ivid[1] << 16) | ((uint32_t)ivid[2] << 8) | ivid[3]);
    char rikey[33];
    to_hex((unsigned char *)cfg->remoteInputAesKey, 16, rikey);

    bool resume = s->current_game != 0;
    char q[4096];
    snprintf(q, sizeof(q),
             "/%s?appid=%d&mode=%dx%dx%d&additionalStates=1&sops=%d&rikey=%s&rikeyid=%d"
             "&localAudioPlayMode=%d&surroundAudioInfo=%d&remoteControllersBitmap=%d&gcmap=%d%s",
             resume ? "resume" : "launch", appid, cfg->width, cfg->height, cfg->fps, sops ? 1 : 0, rikey,
             rikeyid, host_audio ? 1 : 0, SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(cfg->audioConfiguration),
             0xF, 0xF, LiGetLaunchUrlQueryParameters());

    http_resp_t r;
    if (!request(s, true, q, 30000, &r, err, errlen)) return false;
    char *ok_tag = xml_get(r.body, resume ? "resume" : "gamesession");
    bool ok = ok_tag && strcmp(ok_tag, "0") != 0;
    free(ok_tag);
    char *url = xml_get(r.body, "sessionUrl0");
    snprintf(s->rtsp_url, sizeof(s->rtsp_url), "%s", url ? url : "");
    free(url);
    http_resp_free(&r);
    if (!ok) {
        SETERR("Uygulama başlatılamadı");
        return false;
    }
    s->current_game = appid;
    return true;
}

bool gs_quit(gs_server_t *s, char *err, size_t errlen) {
    http_resp_t r;
    if (!request(s, true, "/cancel", 30000, &r, err, errlen)) return false;
    char *c = xml_get(r.body, "cancel");
    bool ok = c && strcmp(c, "0") != 0;
    free(c);
    http_resp_free(&r);
    if (!ok) SETERR("Oyun kapatılamadı");
    else s->current_game = 0;
    return ok;
}
