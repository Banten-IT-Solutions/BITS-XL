#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include <mbedtls/version.h>

#include <curl/curl.h>

#ifdef BITSXL_HAVE_JSONC
#include <json-c/json.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define BITSXL_VERSION "1.2.0-r1"
#define DECOY_V2_PAYMENT_FOR "\xF0\x9F\xA4\xAB"
#define FAMILY_LOOP_PAYMENT_FOR "\xF0\x9F\xA4\x91"
#define TOKEN_CACHE_TTL_SEC 240
#define HTTP_RESPONSE_MAX (2U * 1024U * 1024U)
#define RESPONSE_CACHE_MAX (1U * 1024U * 1024U)
#ifndef RESPONSE_CACHE_DIR
#define RESPONSE_CACHE_DIR "/tmp/bitsxl-cache"
#endif
#define DASHBOARD_CACHE_TTL_SEC 15
#define TIERING_CACHE_TTL_SEC 30
#define STORE_CACHE_TTL_SEC 300

typedef struct {
	char base_api_url[256], base_ciam_url[256], basic_auth[512], ax_fp_key[128], ua[512], api_key[256];
	char encrypted_field_key[128], xdata_key[128], ax_api_sig_key[256], x_api_base_secret[256];
	char decoy_prepaid_family_code[256], decoy_prepaid_package_number[32];
	char decoy_prioritas_family_code[256], decoy_prioritas_package_number[32];
	char decoy_priohybrid_family_code[256], decoy_priohybrid_package_number[32];
	char home[PATH_MAX], env_path[PATH_MAX];
} Config;
typedef struct { char *number, *subscriber_id, *subscription_type, *refresh_token; int dirty, is_new; } Account;
typedef struct { Account *items; size_t len, loaded_len; char **deleted; size_t deleted_len, deleted_cap; } Accounts;
typedef struct { char *access_token, *id_token, *refresh_token; int cached; } Tokens;
typedef struct { char **v; size_t n, cap; } StrList;
typedef struct { char *name, *group, *code, *domain, *subtype; } QuotaPkg;
typedef struct { QuotaPkg *v; size_t n, cap; } QuotaList;
typedef struct {
	char *option_code,*family_code,*variant_code,*migration_type;
	int is_enterprise,package_number,resolved;
} PackageContext;
typedef struct {
	char *item_code,*item_name,*family,*validity,*payment_for,*token_confirmation;
	long long price,balance,timestamp;
	PackageContext context;
} PaymentQuote;
typedef struct { char code[256], name[256]; long long price; } CartItem;
typedef struct { int count,use_decoy,token_idx_raw,token_idx; unsigned int delay_seconds; } RepeatPurchaseOptions;
typedef struct { int use_decoy,pause_on_success,start_option; unsigned int delay_seconds; } FamilyPurchaseOptions;
typedef struct { const char *category,*family_code; int package_number,configured,prioritas_fallback; } DecoyConfigChoice;
typedef enum {
	DECOY_RESOLVE_OK=0,
	DECOY_RESOLVE_FAILED=-1,
	DECOY_RESOLVE_RETRY_151=-2,
	DECOY_RESOLVE_RETRY_AUTH=-3
} DecoyResolveResult;

static Config cfg; static int json_payment_confirmed;
static const char *config_missing(void);
static void json_str(const char *s);
static int arg_true(const char *s);
static int api_auth_failed(const char *resp);
static int api_success_response(const char *resp);
static int api_error_151(const char *response);
static char *unsubscribe_quota_once(Accounts *a,Account *acc,Tokens *t,const char *quota_code,const char *subtype,const char *domain);
static Account *accounts_find(Accounts *a,const char *number);
static void accounts_put(Accounts *a,const char *number,const char *sub,const char *typ,const char *rt);

static void die(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(1); }
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("oom"); return p; }
static char *xstrdup(const char *s) { char *p = strdup(s ? s : ""); if (!p) die("oom"); return p; }
static char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }
static char *xasprintf(const char *fmt, ...) { va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap); int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap); if (n < 0) die("vsnprintf"); char *s = xmalloc((size_t)n + 1); vsnprintf(s, (size_t)n + 1, fmt, ap2); va_end(ap2); return s; }
static void package_context_free(PackageContext *context){
	if(!context) return;
	free(context->option_code);
	free(context->family_code);
	free(context->variant_code);
	free(context->migration_type);
	memset(context,0,sizeof(*context));
}
static void package_context_set(PackageContext *context,const char *option_code,const char *family_code,const char *variant_code,int is_enterprise,const char *migration_type,int package_number,int resolved){
	if(!context) return;
	package_context_free(context);
	context->option_code=xstrdup(option_code?option_code:"");
	context->family_code=xstrdup(family_code?family_code:"");
	context->variant_code=xstrdup(variant_code?variant_code:"");
	context->migration_type=xstrdup(migration_type&&*migration_type?migration_type:"NONE");
	context->is_enterprise=is_enterprise?1:0;
	context->package_number=package_number;
	context->resolved=resolved?1:0;
}
static void package_context_copy(PackageContext *dst,const PackageContext *src){
	if(!dst) return;
	if(dst==src) return;
	if(!src){ package_context_free(dst); return; }
	package_context_set(dst,src->option_code,src->family_code,src->variant_code,src->is_enterprise,src->migration_type,src->package_number,src->resolved);
}
static void trim(char *s) { char *p = s; while (isspace((unsigned char)*p)) p++; if (p != s) memmove(s, p, strlen(p) + 1); size_t n = strlen(s); while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0; }
static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static const char *host_from_url(const char *url) { const char *p = strstr(url, "://"); return p ? p + 3 : url; }
static char *host_header(const char *url) { const char *h = host_from_url(url), *slash = strchr(h, '/'); return slash ? xstrndup(h, (size_t)(slash - h)) : xstrdup(h); }
static void path_join(char *out, size_t sz, const char *a, const char *b) { snprintf(out, sz, "%s/%s", a, b); }
static void ensure_dir(const char *p) { struct stat st; if(!mkdir(p,0700)) return; if(errno!=EEXIST) die("mkdir %s: %s",p,strerror(errno)); if(lstat(p,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()) die("unsafe state directory: %s",p); if((st.st_mode&0777)!=0700&&chmod(p,0700)) die("chmod %s: %s",p,strerror(errno)); }
static char *read_file(const char *path) { FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long n = ftell(f); if (n < 0) { fclose(f); return NULL; } rewind(f); char *b = xmalloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); fclose(f); b[r] = 0; return b; }
static int write_file(const char *path, const char *data) { char tmp[PATH_MAX]; snprintf(tmp,sizeof(tmp),"%s.%ld.XXXXXX",path,(long)getpid()); int fd=mkstemp(tmp); if(fd<0) return -1; size_t len=strlen(data),off=0; while(off<len){ ssize_t w=write(fd,data+off,len-off); if(w<=0){ close(fd); unlink(tmp); return -1; } off+=(size_t)w; } fsync(fd); close(fd); chmod(tmp,0600); if(rename(tmp,path)){ unlink(tmp); return -1; } return 0; }
static void random_bytes(unsigned char *buf, size_t n) { int fd = open("/dev/urandom", O_RDONLY); if (fd >= 0) { size_t off = 0; while (off < n) { ssize_t r = read(fd, buf + off, n - off); if (r <= 0) break; off += (size_t)r; } close(fd); if (off == n) return; } srand((unsigned)(time(NULL) ^ getpid())); for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(rand() & 255); }
static void hex_encode(const unsigned char *in, size_t n, char *out) { static const char h[] = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { out[i*2] = h[in[i] >> 4]; out[i*2+1] = h[in[i] & 15]; } out[n*2] = 0; }
static long long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL; }
static void uuid_v4(char out[37]) { unsigned char b[16]; random_bytes(b, 16); b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128; snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]); }
static void timestamp_java_local(char out[40]) { long long ms = now_ms(); time_t t = (time_t)(ms / 1000); struct tm lt; localtime_r(&t, &lt); char zraw[8] = "+0000", z[8] = "+00:00"; strftime(zraw, sizeof(zraw), "%z", &lt); if (strlen(zraw) == 5) snprintf(z, sizeof(z), "%.3s:%.2s", zraw, zraw + 3); strftime(out, 24, "%Y-%m-%dT%H:%M:%S", &lt); char tail[16]; snprintf(tail, sizeof(tail), ".%02lld%s", (ms % 1000) / 10, z); strcat(out, tail); }
static void timestamp_gmt7(char out[40], int minus_seconds) { long long ms = now_ms() - (long long)minus_seconds * 1000LL; time_t t = (time_t)(ms / 1000 + 7 * 3600); struct tm tm; gmtime_r(&t, &tm); strftime(out, 24, "%Y-%m-%dT%H:%M:%S", &tm); char tail[16]; snprintf(tail, sizeof(tail), ".%03lld+0700", ms % 1000); strcat(out, tail); }
static void timestamp_gmt7_colon(char out[40]) { long long ms = now_ms(); time_t t = (time_t)(ms / 1000 + 7 * 3600); struct tm tm; gmtime_r(&t, &tm); strftime(out, 24, "%Y-%m-%dT%H:%M:%S", &tm); char tail[16]; snprintf(tail, sizeof(tail), ".%02lld+07:00", (ms % 1000) / 10); strcat(out, tail); }

static int aes_cbc(int enc, const unsigned char *key, size_t key_len, unsigned char iv[16], const unsigned char *in, unsigned char *out, size_t len) { mbedtls_aes_context ctx; mbedtls_aes_init(&ctx); int rc = enc ? mbedtls_aes_setkey_enc(&ctx, key, (unsigned)(key_len * 8)) : mbedtls_aes_setkey_dec(&ctx, key, (unsigned)(key_len * 8)); if (!rc) rc = mbedtls_aes_crypt_cbc(&ctx, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, len, iv, in, out); mbedtls_aes_free(&ctx); return rc; }
static void sha256_bytes(const unsigned char *in, size_t n, unsigned char out[32]) {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
	if (mbedtls_sha256(in, n, out, 0)) die("sha256");
#else
	if (mbedtls_sha256_ret(in, n, out, 0)) die("sha256");
#endif
}
static void sha512_bytes(const unsigned char *in, size_t n, unsigned char out[64]) {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
	if (mbedtls_sha512(in, n, out, 0)) die("sha512");
#else
	if (mbedtls_sha512_ret(in, n, out, 0)) die("sha512");
#endif
}
static void md5_bytes(const unsigned char *in, size_t n, unsigned char out[16]) {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
	if (mbedtls_md5(in, n, out)) die("md5");
#else
	if (mbedtls_md5_ret(in, n, out)) die("md5");
#endif
}
static void hmac_hash(int sha512_mode, const unsigned char *key, size_t key_len, const unsigned char *msg, size_t msg_len, unsigned char *out) { size_t block = sha512_mode ? 128 : 64, out_len = sha512_mode ? 64 : 32; unsigned char k0[128], ipad[128], opad[128], inner[64]; memset(k0, 0, sizeof(k0)); if (key_len > block) { if (sha512_mode) sha512_bytes(key, key_len, k0); else sha256_bytes(key, key_len, k0); } else memcpy(k0, key, key_len); for (size_t i = 0; i < block; i++) { ipad[i] = k0[i] ^ 0x36; opad[i] = k0[i] ^ 0x5c; } unsigned char *buf = xmalloc(block + msg_len); memcpy(buf, ipad, block); memcpy(buf + block, msg, msg_len); if (sha512_mode) sha512_bytes(buf, block + msg_len, inner); else sha256_bytes(buf, block + msg_len, inner); free(buf); buf = xmalloc(block + out_len); memcpy(buf, opad, block); memcpy(buf + block, inner, out_len); if (sha512_mode) sha512_bytes(buf, block + out_len, out); else sha256_bytes(buf, block + out_len, out); free(buf); }

static char *b64_encode(const unsigned char *data, size_t len, int urlsafe) { const char *tab = urlsafe ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; char *out = xmalloc(((len + 2) / 3) * 4 + 1); size_t j = 0; for (size_t i = 0; i < len; i += 3) { unsigned v = (unsigned)data[i] << 16; int rem = (int)(len - i); if (rem > 1) v |= (unsigned)data[i+1] << 8; if (rem > 2) v |= data[i+2]; out[j++] = tab[(v >> 18) & 63]; out[j++] = tab[(v >> 12) & 63]; out[j++] = rem > 1 ? tab[(v >> 6) & 63] : '='; out[j++] = rem > 2 ? tab[v & 63] : '='; } out[j] = 0; return out; }
static int b64_val(int c) { if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26; if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+' || c == '-') return 62; if (c == '/' || c == '_') return 63; return -1; }
static unsigned char *b64_decode(const char *s, size_t *out_len) { size_t len = strlen(s), cap = (len / 4 + 2) * 3, j = 0; unsigned char *out = xmalloc(cap + 1); int val = 0, valb = -8; for (size_t i = 0; i < len; i++) { if (s[i] == '=') break; int d = b64_val((unsigned char)s[i]); if (d < 0) continue; val = (val << 6) | d; valb += 6; if (valb >= 0) { out[j++] = (unsigned char)((val >> valb) & 255); valb -= 8; } } out[j] = 0; *out_len = j; return out; }
static char *url_encode(const char *s) { size_t n = 0; for (const unsigned char *p = (const unsigned char *)s; *p; p++) n += (isalnum(*p) || strchr("-_.~", *p)) ? 1 : 3; char *out = xmalloc(n + 1), *q = out; for (const unsigned char *p = (const unsigned char *)s; *p; p++) { if (isalnum(*p) || strchr("-_.~", *p)) *q++ = (char)*p; else { sprintf(q, "%%%02X", *p); q += 3; } } *q = 0; return out; }
static char *json_escape(const char *s) { size_t n = 0; for (const unsigned char *p = (const unsigned char *)s; *p; p++) n += (*p == '"' || *p == '\\') ? 2 : (*p < 32 ? 6 : 1); char *out = xmalloc(n + 1), *q = out; for (const unsigned char *p = (const unsigned char *)s; *p; p++) { if (*p == '"' || *p == '\\') { *q++ = '\\'; *q++ = (char)*p; } else if (*p < 32) { sprintf(q, "\\u%04x", *p); q += 6; } else *q++ = (char)*p; } *q = 0; return out; }

static const char *json_find_key(const char *json, const char *key) { size_t klen = strlen(key); int in = 0, esc = 0; for (const char *p = json; *p; p++) { if (in) { if (esc) esc = 0; else if (*p == '\\') esc = 1; else if (*p == '"') in = 0; continue; } if (*p == '"') { if (!strncmp(p + 1, key, klen) && p[1 + klen] == '"') { const char *q = p + klen + 2; while (isspace((unsigned char)*q)) q++; if (*q == ':') return q + 1; } in = 1; } } return NULL; }
static char *json_get_string(const char *json, const char *key) { const char *p = json_find_key(json, key); if (!p) return NULL; while (isspace((unsigned char)*p)) p++; if (*p != '"') return NULL; p++; char *out = xmalloc(strlen(p) + 1), *q = out; int esc = 0; for (; *p; p++) { if (esc) { if (*p == 'n') *q++ = '\n'; else if (*p == 't') *q++ = '\t'; else if (*p == 'r') *q++ = '\r'; else if (*p == 'u') { *q++ = '?'; for (int i = 0; i < 4 && isxdigit((unsigned char)p[1]); i++) p++; } else *q++ = *p; esc = 0; } else if (*p == '\\') esc = 1; else if (*p == '"') break; else *q++ = *p; } *q = 0; return out; }
static long long json_get_ll(const char *json, const char *key, long long def) { const char *p = json_find_key(json, key); if (!p) return def; while (isspace((unsigned char)*p)) p++; char *e = NULL; long long v = strtoll(p, &e, 10); return e == p ? def : v; }
static int json_array_span(const char *json, const char *key, const char **start, const char **end) { const char *p = json_find_key(json, key); if (!p) return -1; while (isspace((unsigned char)*p)) p++; if (*p != '[') return -1; int depth = 0, in = 0, esc = 0; const char *s = p + 1; for (; *p; p++) { if (in) { if (esc) esc = 0; else if (*p == '\\') esc = 1; else if (*p == '"') in = 0; } else { if (*p == '"') in = 1; else if (*p == '[') depth++; else if (*p == ']') { depth--; if (!depth) { *start = s; *end = p; return 0; } } } } return -1; }
static int json_array_span_any(const char *json,const char **keys,size_t n,const char **start,const char **end){ for(size_t i=0;i<n;i++) if(!json_array_span(json,keys[i],start,end)) return 0; return -1; }
static char *json_object_dup(const char *json, const char *key) { const char *p = json_find_key(json, key); if (!p) return NULL; while (isspace((unsigned char)*p)) p++; if (*p != '{') return NULL; int depth = 0, in = 0, esc = 0; const char *s = p; for (; *p; p++) { if (in) { if (esc) esc = 0; else if (*p == '\\') esc = 1; else if (*p == '"') in = 0; } else { if (*p == '"') in = 1; else if (*p == '{') depth++; else if (*p == '}') { depth--; if (!depth) return xstrndup(s, (size_t)(p - s + 1)); } } } return NULL; }
static long long json_get_ll_any(const char *json, const char *key, long long def) { long long v = json_get_ll(json, key, LLONG_MIN); if (v != LLONG_MIN) return v; char *s = json_get_string(json, key); if (!s) return def; char *e = NULL; v = strtoll(s, &e, 10); if (e == s) v = def; free(s); return v; }
#ifndef BITSXL_HAVE_JSONC
static int json_value_span_at(const char *p,const char **start,const char **end){ while(p&&isspace((unsigned char)*p)) p++; if(!p||!*p) return -1; *start=p; if(*p=='"' ){ int esc=0; for(p++;*p;p++){ if(esc) esc=0; else if(*p=='\\') esc=1; else if(*p=='"'){ *end=p+1; return 0; } } return -1; } if(*p=='{'||*p=='['){ int depth=0,in=0,esc=0; for(;*p;p++){ if(in){ if(esc) esc=0; else if(*p=='\\') esc=1; else if(*p=='"') in=0; } else { if(*p=='"') in=1; else if(*p=='{'||*p=='[') depth++; else if(*p=='}'||*p==']'){ if(--depth==0){ *end=p+1; return 0; } } } } return -1; } while(*p&&!strchr(",}]\r\n\t ",*p)) p++; *end=p; return *end>*start?0:-1; }
#endif
static int json_document_ok(const char *s){
	if(!s) return 0;
#ifdef BITSXL_HAVE_JSONC
	struct json_tokener *tok=json_tokener_new();
	if(!tok) return 0;
	struct json_object *obj=json_tokener_parse_ex(tok,s,(int)strlen(s));
	enum json_tokener_error err=json_tokener_get_error(tok);
	enum json_type type=obj?json_object_get_type(obj):json_type_null;
	int ok=obj&&err==json_tokener_success&&(type==json_type_object||type==json_type_array);
	if(obj) json_object_put(obj);
	json_tokener_free(tok);
	return ok;
#else
	const char *a,*b;
	if(json_value_span_at(s,&a,&b)) return 0;
	while(isspace((unsigned char)*b)) b++;
	return *b==0&&(*a=='{'||*a=='[');
#endif
}
static char *next_object(const char **p, const char *end) { const char *s = *p; while (s < end && *s != '{') s++; if (s >= end) { *p = end; return NULL; } int depth = 0, in = 0, esc = 0; for (const char *q = s; q < end; q++) { if (in) { if (esc) esc = 0; else if (*q == '\\') esc = 1; else if (*q == '"') in = 0; } else { if (*q == '"') in = 1; else if (*q == '{') depth++; else if (*q == '}') { depth--; if (!depth) { size_t n = (size_t)(q - s + 1); char *out = xmalloc(n + 1); memcpy(out, s, n); out[n] = 0; *p = q + 1; return out; } } } } *p = end; return NULL; }

static char *pkcs7_pad(const unsigned char *in, size_t len, size_t *out_len) { size_t pad = 16 - (len % 16); *out_len = len + pad; char *out = xmalloc(*out_len); memcpy(out, in, len); memset(out + len, (int)pad, pad); return out; }
static int pkcs7_unpad(unsigned char *buf, size_t *len) { if (!*len) return -1; unsigned char pad = buf[*len - 1]; if (!pad || pad > 16 || pad > *len) return -1; for (size_t i = *len - pad; i < *len; i++) if (buf[i] != pad) return -1; *len -= pad; buf[*len] = 0; return 0; }
static char *encrypt_xdata(const char *plain, long long xtime) { char xt[32], hex[65]; snprintf(xt, sizeof(xt), "%lld", xtime); unsigned char digest[32]; sha256_bytes((unsigned char *)xt, strlen(xt), digest); hex_encode(digest, 32, hex); unsigned char iv[16]; memcpy(iv, hex, 16); size_t padded_len; char *padded = pkcs7_pad((unsigned char *)plain, strlen(plain), &padded_len); unsigned char *ct = xmalloc(padded_len); if (aes_cbc(1, (unsigned char *)cfg.xdata_key, strlen(cfg.xdata_key), iv, (unsigned char *)padded, ct, padded_len)) die("aes encrypt"); char *b64 = b64_encode(ct, padded_len, 1); free(padded); free(ct); return b64; }
static char *decrypt_xdata_payload(const char *json) { char *xdata = json_get_string(json, "xdata"); long long xtime = json_get_ll(json, "xtime", -1); if (!xdata || xtime < 0) { free(xdata); return NULL; } char xt[32], hex[65]; snprintf(xt, sizeof(xt), "%lld", xtime); unsigned char digest[32]; sha256_bytes((unsigned char *)xt, strlen(xt), digest); hex_encode(digest, 32, hex); unsigned char iv[16]; memcpy(iv, hex, 16); size_t ct_len; unsigned char *ct = b64_decode(xdata, &ct_len); if (ct_len % 16) { free(xdata); free(ct); return NULL; } unsigned char *pt = xmalloc(ct_len + 1); if (aes_cbc(0, (unsigned char *)cfg.xdata_key, strlen(cfg.xdata_key), iv, ct, pt, ct_len)) { free(xdata); free(ct); free(pt); return NULL; } size_t pt_len = ct_len; if (pkcs7_unpad(pt, &pt_len)) { free(xdata); free(ct); free(pt); return NULL; } char *out = xstrdup((char *)pt); free(xdata); free(ct); free(pt); return out; }
static char *make_x_signature(const char *id_token, const char *method, const char *path, long long sig_time) { char *key = xasprintf("%s;%s;%s;%s;%lld", cfg.x_api_base_secret, id_token, method, path, sig_time); char *msg = xasprintf("%s;%lld;", id_token, sig_time); unsigned char mac[64]; hmac_hash(1, (unsigned char *)key, strlen(key), (unsigned char *)msg, strlen(msg), mac); char *hex = xmalloc(129); hex_encode(mac, 64, hex); free(key); free(msg); return hex; }
static char *make_x_signature_payment(const char *access_token,long long payment_ts,const char *package_code,const char *token_payment,const char *payment_method,const char *payment_for,const char *path){ char *key=xasprintf("%s;%lld#ae-hei_9Tee6he+Ik3Gais5=;POST;%s;%lld",cfg.x_api_base_secret,payment_ts,path,payment_ts); char *msg=xasprintf("%s;%s;%lld;%s;%s;%s;",access_token,token_payment,payment_ts,payment_for,payment_method,package_code); unsigned char mac[64]; hmac_hash(1,(unsigned char *)key,strlen(key),(unsigned char *)msg,strlen(msg),mac); char *hex=xmalloc(129); hex_encode(mac,64,hex); free(key); free(msg); return hex; }
static char *make_x_signature_bounty(const char *access_token,long long sig_time,const char *package_code,const char *token){ const char *path="api/v8/personalization/bounties-exchange"; char *key=xasprintf("%s;%s;%lld#ae-hei_9Tee6he+Ik3Gais5=;POST;%s;%lld",cfg.x_api_base_secret,access_token,sig_time,path,sig_time); char *msg=xasprintf("%s;%s;%lld;%s;",access_token,token,sig_time,package_code); unsigned char mac[64]; hmac_hash(1,(unsigned char *)key,strlen(key),(unsigned char *)msg,strlen(msg),mac); char *hex=xmalloc(129); hex_encode(mac,64,hex); free(key); free(msg); return hex; }
static char *make_x_signature_loyalty(long long sig_time,const char *package_code,const char *token,const char *path){ char *key=xasprintf("%s;%lld#ae-hei_9Tee6he+Ik3Gais5=;POST;%s;%lld",cfg.x_api_base_secret,sig_time,path,sig_time); char *msg=xasprintf("%s;%lld;%s;",token,sig_time,package_code); unsigned char mac[64]; hmac_hash(1,(unsigned char *)key,strlen(key),(unsigned char *)msg,strlen(msg),mac); char *hex=xmalloc(129); hex_encode(mac,64,hex); free(key); free(msg); return hex; }
static char *make_x_signature_bounty_allotment(long long sig_time,const char *package_code,const char *token,const char *path,const char *dest){ char *key=xasprintf("%s;%lld#ae-hei_9Tee6he+Ik3Gais5=;%s;POST;%s;%lld",cfg.x_api_base_secret,sig_time,dest,path,sig_time); char *msg=xasprintf("%s;%lld;%s;%s;",token,sig_time,dest,package_code); unsigned char mac[64]; hmac_hash(1,(unsigned char *)key,strlen(key), (unsigned char *)msg,strlen(msg),mac); char *hex=xmalloc(129); hex_encode(mac,64,hex); free(key); free(msg); return hex; }
static char *encrypted_empty_field(void){ if(!cfg.encrypted_field_key[0]) return xstrdup(""); unsigned char rb[8]; random_bytes(rb,8); char iv_hex[17]; hex_encode(rb,8,iv_hex); unsigned char iv[16]; memcpy(iv,iv_hex,16); unsigned char pt[16],ct[16]; memset(pt,16,sizeof(pt)); if(aes_cbc(1,(unsigned char *)cfg.encrypted_field_key,strlen(cfg.encrypted_field_key),iv,pt,ct,sizeof(ct))) die("encrypted field aes"); char *b64=b64_encode(ct,sizeof(ct),1),*out=xasprintf("%s%s",b64,iv_hex); free(b64); return out; }
static char *make_ax_api_signature(const char *ts, const char *contact, const char *code, const char *contact_type) { char *pre = xasprintf("%spassword%s%s%sopenid", ts, contact_type, contact, code); unsigned char mac[32]; hmac_hash(0, (unsigned char *)cfg.ax_api_sig_key, strlen(cfg.ax_api_sig_key), (unsigned char *)pre, strlen(pre), mac); char *b64 = b64_encode(mac, 32, 0); free(pre); return b64; }
static char *ax_fingerprint(void) { char path[PATH_MAX]; path_join(path, sizeof(path), cfg.home, "ax.fp"); char *existing = read_file(path); if (existing) { trim(existing); if (*existing) return existing; free(existing); } unsigned char rb[4]; random_bytes(rb, 4); unsigned r1 = 1000 + (((unsigned)rb[0] << 8 | rb[1]) % 9000), r2 = 1000 + (((unsigned)rb[2] << 8 | rb[3]) % 9000); char plain[256]; snprintf(plain, sizeof(plain), "samsung%u|SM-N93%u|en|720x1540|GMT07:00|192.169.69.69|1.0|Android 13|6281398370564", r1, r2); unsigned char iv[16] = {0}; size_t padded_len; char *padded = pkcs7_pad((unsigned char *)plain, strlen(plain), &padded_len); unsigned char *ct = xmalloc(padded_len); if (aes_cbc(1, (unsigned char *)cfg.ax_fp_key, strlen(cfg.ax_fp_key), iv, (unsigned char *)padded, ct, padded_len)) die("fingerprint aes"); char *b64 = b64_encode(ct, padded_len, 0); write_file(path, b64); free(padded); free(ct); return b64; }
static char *ax_device_id(const char *fp) { unsigned char digest[16]; md5_bytes((unsigned char *)fp, strlen(fp), digest); char *hex = xmalloc(33); hex_encode(digest, 16, hex); return hex; }

static void sl_add(StrList *l, char *s) { if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->v = realloc(l->v, l->cap * sizeof(char *)); if (!l->v) die("oom"); } l->v[l->n++] = s; }
static void free_sl(StrList *l) { for (size_t i = 0; i < l->n; i++) free(l->v[i]); free(l->v); }
static char *hdr(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap); char *line = xmalloc((size_t)n + 10); strcpy(line, "--header="); va_start(ap, fmt); vsnprintf(line + 9, (size_t)n + 1, fmt, ap); va_end(ap); return line; }

typedef struct {
	char *data;
	size_t len, cap;
	int too_large;
} HttpResponseBuffer;

static int http_curl_global_ready;

static void http_curl_global_cleanup(void) {
	if (http_curl_global_ready) {
		curl_global_cleanup();
		http_curl_global_ready = 0;
	}
}

static CURLcode http_curl_global_init_once(void) {
	CURLcode code;
	if (http_curl_global_ready)
		return CURLE_OK;
	code = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (code != CURLE_OK)
		return code;
	if (atexit(http_curl_global_cleanup)) {
		curl_global_cleanup();
		return CURLE_FAILED_INIT;
	}
	http_curl_global_ready = 1;
	return CURLE_OK;
}

static size_t http_response_write(char *data, size_t size, size_t nmemb, void *opaque) {
	HttpResponseBuffer *response = opaque;
	size_t chunk, needed, next;
	if (!size || !nmemb)
		return 0;
	if (nmemb > SIZE_MAX / size) {
		response->too_large = 1;
		return 0;
	}
	chunk = size * nmemb;
	if (chunk > (size_t)HTTP_RESPONSE_MAX - response->len) {
		response->too_large = 1;
		return 0;
	}
	needed = response->len + chunk + 1;
	if (needed > response->cap) {
		next = response->cap ? response->cap : 4096;
		while (next < needed) {
			if (next >= (size_t)HTTP_RESPONSE_MAX + 1U) {
				next = (size_t)HTTP_RESPONSE_MAX + 1U;
				break;
			}
			next *= 2;
			if (next > (size_t)HTTP_RESPONSE_MAX + 1U)
				next = (size_t)HTTP_RESPONSE_MAX + 1U;
		}
		response->data = realloc(response->data, next);
		if (!response->data)
			die("oom");
		response->cap = next;
	}
	memcpy(response->data + response->len, data, chunk);
	response->len += chunk;
	response->data[response->len] = 0;
	return chunk;
}

static const char *http_header_line(const char *header) {
	return starts_with(header, "--header=") ? header + 9 : header;
}

static char *http_fetch_failed(CURLcode code) {
	return xasprintf("{\"status\":\"FAILED\",\"code\":\"HTTP_FETCH_FAILED\",\"exit\":%d,\"curl\":%d}", (int)code, (int)code);
}

static char *http_fetch(const char *url, const char *method, StrList *headers, const char *body) {
	CURL *curl;
	struct curl_slist *curl_headers = NULL;
	HttpResponseBuffer response = {0};
	CURLcode code;
	char *out;
	if (!url || !*url)
		return http_fetch_failed(CURLE_URL_MALFORMAT);
	code = http_curl_global_init_once();
	if (code != CURLE_OK)
		return http_fetch_failed(code);
	curl = curl_easy_init();
	if (!curl)
		return http_fetch_failed(CURLE_FAILED_INIT);
	for (size_t h = 0; headers && h < headers->n; h++) {
		struct curl_slist *next;
		if (!headers->v[h])
			continue;
		next = curl_slist_append(curl_headers, http_header_line(headers->v[h]));
		if (!next) {
			code = CURLE_OUT_OF_MEMORY;
			goto done;
		}
		curl_headers = next;
	}
#define HTTP_CURL_SET(option, value) do { code = curl_easy_setopt(curl, option, value); if (code != CURLE_OK) goto done; } while (0)
	HTTP_CURL_SET(CURLOPT_URL, url);
	HTTP_CURL_SET(CURLOPT_HTTPHEADER, curl_headers);
	HTTP_CURL_SET(CURLOPT_FOLLOWLOCATION, 1L);
	HTTP_CURL_SET(CURLOPT_CONNECTTIMEOUT, 10L);
	HTTP_CURL_SET(CURLOPT_TIMEOUT, 30L);
	HTTP_CURL_SET(CURLOPT_NOPROGRESS, 1L);
	HTTP_CURL_SET(CURLOPT_NOSIGNAL, 1L);
	HTTP_CURL_SET(CURLOPT_SSL_VERIFYPEER, 1L);
	HTTP_CURL_SET(CURLOPT_SSL_VERIFYHOST, 2L);
	HTTP_CURL_SET(CURLOPT_FAILONERROR, 0L);
	HTTP_CURL_SET(CURLOPT_WRITEFUNCTION, http_response_write);
	HTTP_CURL_SET(CURLOPT_WRITEDATA, &response);
	if (body) {
		HTTP_CURL_SET(CURLOPT_POSTFIELDS, body);
		HTTP_CURL_SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
		HTTP_CURL_SET(CURLOPT_CUSTOMREQUEST, method && *method ? method : "POST");
	} else {
		HTTP_CURL_SET(CURLOPT_HTTPGET, 1L);
	}
	code = curl_easy_perform(curl);
done:
#undef HTTP_CURL_SET
	if (response.too_large) {
		free(response.data);
		out = xasprintf("{\"status\":\"FAILED\",\"code\":\"HTTP_RESPONSE_TOO_LARGE\",\"limit\":%u}", HTTP_RESPONSE_MAX);
	} else if (code != CURLE_OK) {
		free(response.data);
		out = http_fetch_failed(code);
	} else {
		out = response.data ? response.data : xstrdup("");
	}
	curl_slist_free_all(curl_headers);
	curl_easy_cleanup(curl);
	return out;
}
static int response_cache_dir_ready(void){ struct stat st; if(!mkdir(RESPONSE_CACHE_DIR,0700)) return 1; if(errno!=EEXIST||lstat(RESPONSE_CACHE_DIR,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()) return 0; chmod(RESPONSE_CACHE_DIR,0700); return 1; }
static void response_cache_config_tag(char out[17]){ const char *v[]={cfg.base_api_url,cfg.base_ciam_url,cfg.basic_auth,cfg.ax_fp_key,cfg.ua,cfg.api_key,cfg.encrypted_field_key,cfg.xdata_key,cfg.ax_api_sig_key,cfg.x_api_base_secret,cfg.decoy_prepaid_family_code,cfg.decoy_prepaid_package_number,cfg.decoy_prioritas_family_code,cfg.decoy_prioritas_package_number,cfg.decoy_priohybrid_family_code,cfg.decoy_priohybrid_package_number}; uint64_t h=UINT64_C(1469598103934665603); for(size_t i=0;i<sizeof(v)/sizeof(v[0]);i++){ for(const unsigned char *p=(const unsigned char *)v[i];*p;p++){ h^=*p; h*=UINT64_C(1099511628211); } h^=0xffU; h*=UINT64_C(1099511628211); } snprintf(out,17,"%016llx",(unsigned long long)h); }
static void response_cache_path(char *out,size_t sz,Account *acc,const char *key){ char account[96],name[96],tag[17]; size_t ai=0,ni=0; const char *number=acc&&acc->number?acc->number:"active"; for(size_t i=0;number[i]&&ai+1<sizeof(account);i++) account[ai++]=(isalnum((unsigned char)number[i])||number[i]=='_'||number[i]=='-')?number[i]:'_'; account[ai]=0; for(size_t i=0;key&&key[i]&&ni+1<sizeof(name);i++) name[ni++]=(isalnum((unsigned char)key[i])||key[i]=='_'||key[i]=='-')?key[i]:'_'; name[ni]=0; response_cache_config_tag(tag); snprintf(out,sz,"%s/%s-%s-%s.json",RESPONSE_CACHE_DIR,account[0]?account:"active",tag,name[0]?name:"data"); }
static int response_cache_shape_valid(const char *key,const char *data){ const char *start,*end; if(!strcmp(key,"quota")) return !json_array_span(data,"quotas",&start,&end); if(!strcmp(key,"balance")) return json_find_key(data,"remaining")!=NULL; if(!strcmp(key,"tiering")) return json_find_key(data,"tier")||json_find_key(data,"current_point"); if(starts_with(key,"segments-")) return !json_array_span(data,"store_segments",&start,&end); if(starts_with(key,"shop-")){ const char *keys[]={"results_price_only","results","packages"}; return !json_array_span_any(data,keys,3,&start,&end); } if(starts_with(key,"families-")) return !json_array_span(data,"results",&start,&end); return 1; }
static char *response_cache_load(Account *acc,const char *key,int ttl){ if(ttl<=0||!response_cache_dir_ready()) return NULL; char path[PATH_MAX]; struct stat st; response_cache_path(path,sizeof(path),acc,key); if(lstat(path,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||(unsigned long long)st.st_size>RESPONSE_CACHE_MAX) return NULL; time_t now=time(NULL); if(st.st_mtime>now||now-st.st_mtime>=ttl){ unlink(path); return NULL; } char *data=read_file(path); if(!data||!json_document_ok(data)||!response_cache_shape_valid(key,data)){ free(data); unlink(path); return NULL; } return data; }
static void response_cache_save(Account *acc,const char *key,const char *data){ if(!data||strlen(data)>RESPONSE_CACHE_MAX||!json_document_ok(data)||api_auth_failed(data)||!api_success_response(data)||!response_cache_shape_valid(key,data)||!response_cache_dir_ready()) return; char path[PATH_MAX]; response_cache_path(path,sizeof(path),acc,key); write_file(path,data); }
static void response_cache_delete(Account *acc,const char *key){ if(!response_cache_dir_ready()) return; char path[PATH_MAX]; response_cache_path(path,sizeof(path),acc,key); unlink(path); }
static char *response_cache_generation(Account *acc){ if(!response_cache_dir_ready()) return xstrdup(""); char path[PATH_MAX]; response_cache_path(path,sizeof(path),acc,"_epoch"); char *value=read_file(path); return value?value:xstrdup(""); }
static void response_cache_bump(Account *acc){ if(!acc||!acc->number||!response_cache_dir_ready()) return; unsigned char random[8]; char hex[17],path[PATH_MAX],value[96]; random_bytes(random,sizeof(random)); hex_encode(random,sizeof(random),hex); response_cache_path(path,sizeof(path),acc,"_epoch"); snprintf(value,sizeof(value),"%lld-%ld-%s\n",now_ms(),(long)getpid(),hex); write_file(path,value); }
static void response_cache_save_if_current(Account *acc,const char *key,const char *data,const char *generation){ char *current=response_cache_generation(acc); if(!strcmp(current,generation?generation:"")) response_cache_save(acc,key,data); free(current); }
static void response_cache_invalidate_dynamic(Account *acc){ if(!acc||!acc->number) return; response_cache_delete(acc,"balance"); response_cache_delete(acc,"quota"); response_cache_delete(acc,"tiering"); }
static void response_cache_invalidate_account(Account *acc){ response_cache_bump(acc); response_cache_invalidate_dynamic(acc); response_cache_delete(acc,"segments-0"); response_cache_delete(acc,"segments-1"); response_cache_delete(acc,"shop-0"); response_cache_delete(acc,"shop-1"); response_cache_delete(acc,"families-0"); response_cache_delete(acc,"families-1"); }
static void ciam_headers(StrList *h, const char *content_type, const char *bearer_token, const char *ax_sig, const char *ax_req_at) { char *fp = ax_fingerprint(), *dev = ax_device_id(fp), *host = host_header(cfg.base_ciam_url); char uuid[37], at[40]; uuid_v4(uuid); if (!ax_req_at) { timestamp_gmt7_colon(at); ax_req_at = at; } sl_add(h, hdr("Accept-Encoding: identity")); sl_add(h, bearer_token ? hdr("Authorization: Bearer %s", bearer_token) : hdr("Authorization: Basic %s", cfg.basic_auth)); if (ax_sig) sl_add(h, hdr("Ax-Api-Signature: %s", ax_sig)); sl_add(h, hdr("Ax-Device-Id: %s", dev)); sl_add(h, hdr("Ax-Fingerprint: %s", fp)); sl_add(h, hdr("Ax-Request-At: %s", ax_req_at)); sl_add(h, hdr("Ax-Request-Device: samsung")); sl_add(h, hdr("Ax-Request-Device-Model: SM-N935F")); sl_add(h, hdr("Ax-Request-Id: %s", uuid)); sl_add(h, hdr("Ax-Substype: PREPAID")); sl_add(h, hdr("Content-Type: %s", content_type)); sl_add(h, hdr("Host: %s", host)); sl_add(h, hdr("User-Agent: %s", cfg.ua)); free(fp); free(dev); free(host); }
#ifdef BITSXL_TEST
static char *(*bitsxl_test_api_request_hook)(const char *,const char *,const char *);
#endif
static char *api_request(const char *path,const char *payload,const char *id_token){
#ifdef BITSXL_TEST
	if(bitsxl_test_api_request_hook) return bitsxl_test_api_request_hook(path,payload,id_token);
#endif
	if(!id_token||!*id_token) return xstrdup("{\"status\":\"FAILED\",\"code\":\"REQUEST_MISSING_BEARER\",\"message\":\"missing id_token\"}");
	long long xtime=now_ms(),sig_time=xtime/1000;
	char *xdata=encrypt_xdata(payload,xtime),*body=xasprintf("{\"xdata\":\"%s\",\"xtime\":%lld}",xdata,xtime),*sig=make_x_signature(id_token,"POST",path,sig_time),*host=host_header(cfg.base_api_url),*url=xasprintf("%s/%s",cfg.base_api_url,path);
	char uuid[37],at[40]; uuid_v4(uuid); timestamp_java_local(at);
	StrList h={0};
	sl_add(&h,hdr("host: %s",host));
	sl_add(&h,hdr("content-type: application/json; charset=utf-8"));
	sl_add(&h,hdr("user-agent: %s",cfg.ua));
	sl_add(&h,hdr("x-api-key: %s",cfg.api_key));
	sl_add(&h,hdr("Authorization: Bearer %s",id_token));
	sl_add(&h,hdr("x-hv: v3"));
	sl_add(&h,hdr("x-signature-time: %lld",sig_time));
	sl_add(&h,hdr("x-signature: %s",sig));
	sl_add(&h,hdr("x-request-id: %s",uuid));
	sl_add(&h,hdr("x-request-at: %s",at));
	sl_add(&h,hdr("x-version-app: 8.9.0"));
	char *resp=http_fetch(url,"POST",&h,body),*plain=decrypt_xdata_payload(resp);
	if(!plain) plain=xstrdup(resp);
	free(resp); free(xdata); free(body); free(sig); free(host); free(url); free_sl(&h);
	return plain;
}
static char *api_request_payment(const char *path,const char *payload,Tokens *t,const char *payment_targets,const char *token_payment,const char *payment_for,long long payment_ts){ if(!t||!t->id_token||!*t->id_token||!t->access_token||!*t->access_token) return xstrdup("{\"status\":\"FAILED\",\"code\":\"REQUEST_MISSING_BEARER\",\"message\":\"missing token\"}"); long long xtime=now_ms(),sig_time=xtime/1000; char *xdata=encrypt_xdata(payload,xtime),*body=xasprintf("{\"xdata\":\"%s\",\"xtime\":%lld}",xdata,xtime),*sig=make_x_signature_payment(t->access_token,payment_ts,payment_targets,token_payment,"BALANCE",payment_for,path),*host=host_header(cfg.base_api_url),*url=xasprintf("%s/%s",cfg.base_api_url,path); char uuid[37],at[40]; uuid_v4(uuid); timestamp_java_local(at); StrList h={0}; sl_add(&h,hdr("host: %s",host)); sl_add(&h,hdr("content-type: application/json; charset=utf-8")); sl_add(&h,hdr("user-agent: %s",cfg.ua)); sl_add(&h,hdr("x-api-key: %s",cfg.api_key)); sl_add(&h,hdr("Authorization: Bearer %s",t->id_token)); sl_add(&h,hdr("x-hv: v3")); sl_add(&h,hdr("x-signature-time: %lld",sig_time)); sl_add(&h,hdr("x-signature: %s",sig)); sl_add(&h,hdr("x-request-id: %s",uuid)); sl_add(&h,hdr("x-request-at: %s",at)); sl_add(&h,hdr("x-version-app: 8.9.0")); char *resp=http_fetch(url,"POST",&h,body),*plain=decrypt_xdata_payload(resp); if(!plain) plain=xstrdup(resp); free(resp); free(xdata); free(body); free(sig); free(host); free(url); free_sl(&h); return plain; }
static char *api_request_payment_method(const char *path,const char *payload,Tokens *t,const char *payment_targets,const char *token_payment,const char *payment_method,const char *payment_for,long long payment_ts){ if(!t||!t->id_token||!*t->id_token||!t->access_token||!*t->access_token) return xstrdup("{\"status\":\"FAILED\",\"code\":\"REQUEST_MISSING_BEARER\",\"message\":\"missing token\"}"); long long xtime=now_ms(),sig_time=xtime/1000; char *xdata=encrypt_xdata(payload,xtime),*body=xasprintf("{\"xdata\":\"%s\",\"xtime\":%lld}",xdata,xtime),*sig=make_x_signature_payment(t->access_token,payment_ts,payment_targets,token_payment,payment_method,payment_for,path),*host=host_header(cfg.base_api_url),*url=xasprintf("%s/%s",cfg.base_api_url,path); char uuid[37],at[40]; uuid_v4(uuid); timestamp_java_local(at); StrList h={0}; sl_add(&h,hdr("host: %s",host)); sl_add(&h,hdr("content-type: application/json; charset=utf-8")); sl_add(&h,hdr("user-agent: %s",cfg.ua)); sl_add(&h,hdr("x-api-key: %s",cfg.api_key)); sl_add(&h,hdr("Authorization: Bearer %s",t->id_token)); sl_add(&h,hdr("x-hv: v3")); sl_add(&h,hdr("x-signature-time: %lld",sig_time)); sl_add(&h,hdr("x-signature: %s",sig)); sl_add(&h,hdr("x-request-id: %s",uuid)); sl_add(&h,hdr("x-request-at: %s",at)); sl_add(&h,hdr("x-version-app: 8.9.0")); char *resp=http_fetch(url,"POST",&h,body),*plain=decrypt_xdata_payload(resp); if(!plain) plain=xstrdup(resp); free(resp); free(xdata); free(body); free(sig); free(host); free(url); free_sl(&h); return plain; }
static char *api_request_custom_sig(const char *path,const char *payload,Tokens *t,char *sig){ if(!t||!t->id_token||!*t->id_token){ free(sig); return xstrdup("{\"status\":\"FAILED\",\"code\":\"REQUEST_MISSING_BEARER\",\"message\":\"missing id_token\"}"); } long long xtime=now_ms(),sig_time=xtime/1000; char *xdata=encrypt_xdata(payload,xtime),*body=xasprintf("{\"xdata\":\"%s\",\"xtime\":%lld}",xdata,xtime),*host=host_header(cfg.base_api_url),*url=xasprintf("%s/%s",cfg.base_api_url,path); char uuid[37],at[40]; uuid_v4(uuid); timestamp_java_local(at); StrList h={0}; sl_add(&h,hdr("host: %s",host)); sl_add(&h,hdr("content-type: application/json; charset=utf-8")); sl_add(&h,hdr("user-agent: %s",cfg.ua)); sl_add(&h,hdr("x-api-key: %s",cfg.api_key)); sl_add(&h,hdr("Authorization: Bearer %s",t->id_token)); sl_add(&h,hdr("x-hv: v3")); sl_add(&h,hdr("x-signature-time: %lld",sig_time)); sl_add(&h,hdr("x-signature: %s",sig)); sl_add(&h,hdr("x-request-id: %s",uuid)); sl_add(&h,hdr("x-request-at: %s",at)); sl_add(&h,hdr("x-version-app: 8.9.0")); char *resp=http_fetch(url,"POST",&h,body),*plain=decrypt_xdata_payload(resp); if(!plain) plain=xstrdup(resp); free(resp); free(xdata); free(body); free(sig); free(host); free(url); free_sl(&h); return plain; }
static char *request_otp(const char *number) { char *num = url_encode(number), *url = xasprintf("%s/realms/xl-ciam/auth/otp?contact=%s&contactType=SMS&alternateContact=false", cfg.base_ciam_url, num); StrList h = {0}; ciam_headers(&h, "application/json", NULL, NULL, NULL); char *resp = http_fetch(url, "GET", &h, NULL); free(num); free(url); free_sl(&h); return resp; }
static char *submit_otp(const char *contact_type, const char *contact, const char *code) { char ts_sign[40], ts_head[40]; timestamp_gmt7(ts_sign, 0); timestamp_gmt7(ts_head, 300); char *final_contact = !strcmp(contact_type, "DEVICEID") ? b64_encode((unsigned char *)contact, strlen(contact), 0) : xstrdup(contact); char *sig = make_ax_api_signature(ts_sign, final_contact, code, contact_type), *ec = url_encode(final_contact), *ecode = url_encode(code), *ect = url_encode(contact_type), *body = xasprintf("contactType=%s&code=%s&grant_type=password&contact=%s&scope=openid", ect, ecode, ec), *url = xasprintf("%s/realms/xl-ciam/protocol/openid-connect/token", cfg.base_ciam_url); StrList h = {0}; ciam_headers(&h, "application/x-www-form-urlencoded", NULL, sig, ts_head); char *resp = http_fetch(url, "POST", &h, body); free(final_contact); free(sig); free(ec); free(ecode); free(ect); free(body); free(url); free_sl(&h); return resp; }
static char *extend_session(const char *subscriber_id) { char *b = b64_encode((unsigned char *)subscriber_id, strlen(subscriber_id), 0), *eb = url_encode(b), *url = xasprintf("%s/realms/xl-ciam/auth/extend-session?contact=%s&contactType=DEVICEID", cfg.base_ciam_url, eb); StrList h = {0}; ciam_headers(&h, "application/json", NULL, NULL, NULL); char *resp = http_fetch(url, "GET", &h, NULL); free(b); free(eb); free(url); free_sl(&h); return resp; }
static char *refresh_token_http(const char *refresh_token) { char *er = url_encode(refresh_token), *body = xasprintf("grant_type=refresh_token&refresh_token=%s", er), *url = xasprintf("%s/realms/xl-ciam/protocol/openid-connect/token", cfg.base_ciam_url); StrList h = {0}; ciam_headers(&h, "application/x-www-form-urlencoded", NULL, NULL, NULL); char *resp = http_fetch(url, "POST", &h, body); free(er); free(body); free(url); free_sl(&h); return resp; }

static void tokens_free(Tokens *t) { free(t->access_token); free(t->id_token); free(t->refresh_token); memset(t, 0, sizeof(*t)); }
static int parse_tokens(const char *json, Tokens *t) { t->access_token = json_get_string(json, "access_token"); t->id_token = json_get_string(json, "id_token"); t->refresh_token = json_get_string(json, "refresh_token"); return t->access_token && t->id_token && t->refresh_token ? 0 : -1; }
static void token_cache_path(char *out,size_t sz,Account *a){ char safe[96]; size_t j=0; const char *n=a&&a->number?a->number:"active"; for(size_t i=0;n[i]&&j+1<sizeof(safe);i++) safe[j++]=(isalnum((unsigned char)n[i])||n[i]=='_'||n[i]=='-')?n[i]:'_'; safe[j]=0; char file[128]; snprintf(file,sizeof(file),"token-%s.json",safe[0]?safe:"active"); path_join(out,sz,cfg.home,file); }
static int token_cache_load(Account *a,Tokens *t){ char path[PATH_MAX]; token_cache_path(path,sizeof(path),a); char *json=read_file(path); if(!json) return -1; long long exp=json_get_ll_any(json,"expires_at",0),now=(long long)time(NULL); if(exp<=now){ free(json); return -1; } t->access_token=json_get_string(json,"access_token"); t->id_token=json_get_string(json,"id_token"); t->refresh_token=json_get_string(json,"refresh_token"); if(!t->refresh_token&&a&&a->refresh_token) t->refresh_token=xstrdup(a->refresh_token); if(!t->access_token||!t->id_token||!t->refresh_token){ free(json); tokens_free(t); return -1; } t->cached=1; free(json); return 0; }
static void token_cache_save(Account *a,Tokens *t){ if(!a||!t||!t->access_token||!t->id_token||!t->refresh_token) return; char path[PATH_MAX],*at=json_escape(t->access_token),*id=json_escape(t->id_token),*rt=json_escape(t->refresh_token); token_cache_path(path,sizeof(path),a); char *data=xasprintf("{\"expires_at\":%lld,\"access_token\":\"%s\",\"id_token\":\"%s\",\"refresh_token\":\"%s\"}\n",(long long)time(NULL)+TOKEN_CACHE_TTL_SEC,at,id,rt); write_file(path,data); free(at); free(id); free(rt); free(data); }
static void token_cache_delete(Account *a){ char path[PATH_MAX]; token_cache_path(path,sizeof(path),a); unlink(path); }
static void mutation_prepare(Account *acc){ response_cache_invalidate_account(acc); }
static void mutation_result(Account *acc,Tokens *t,const char *resp){ response_cache_invalidate_account(acc); if(api_auth_failed(resp)){ token_cache_delete(acc); tokens_free(t); } }
static int token_cache_lock(Account *a){ char path[PATH_MAX]; token_cache_path(path,sizeof(path),a); strncat(path,".lock",sizeof(path)-strlen(path)-1); int fd=open(path,O_CREAT|O_RDWR,0600); if(fd>=0){ fchmod(fd,0600); flock(fd,LOCK_EX); } return fd; }
static void token_cache_unlock(int fd){ if(fd>=0){ flock(fd,LOCK_UN); close(fd); } }
static int refresh_account(Account *a, Tokens *t) { if(!a||!a->refresh_token||!*a->refresh_token){ fprintf(stderr,"refresh token missing\n"); return -1; } if(!token_cache_load(a,t)) return 0; int lock_fd=token_cache_lock(a); if(!token_cache_load(a,t)){ token_cache_unlock(lock_fd); return 0; } char *resp = refresh_token_http(a->refresh_token); if (strstr(resp, "Session not active")) { if (!a->subscriber_id || !*a->subscriber_id) { fprintf(stderr, "session inactive and subscriber_id missing\n"); free(resp); token_cache_delete(a); token_cache_unlock(lock_fd); return -1; } free(resp); char *ex = extend_session(a->subscriber_id), *code = json_get_string(ex, "exchange_code"); if (!code) { fprintf(stderr, "extend-session failed: %s\n", ex); free(ex); token_cache_delete(a); token_cache_unlock(lock_fd); return -1; } free(ex); resp = submit_otp("DEVICEID", a->subscriber_id, code); free(code); } int rc = parse_tokens(resp, t); if (rc) { fprintf(stderr, "refresh failed: %s\n", resp); token_cache_delete(a); } else token_cache_save(a,t); free(resp); token_cache_unlock(lock_fd); return rc; }
static char *get_profile(Tokens *t) { char *at = json_escape(t->access_token), *payload = xasprintf("{\"access_token\":\"%s\",\"app_version\":\"8.9.0\",\"is_enterprise\":false,\"lang\":\"en\"}", at), *resp = api_request("api/v8/profile", payload, t->id_token); free(at); free(payload); return resp; }
static int account_replace_if_changed(char **dst,const char *value){ if(!value||!*value||(*dst&&!strcmp(*dst,value))) return 0; free(*dst); *dst=xstrdup(value); return 1; }
static int account_update_profile(Account *a,Tokens *t){ int changed=account_replace_if_changed(&a->refresh_token,t->refresh_token); if(!(t->cached&&a->subscriber_id&&*a->subscriber_id&&a->subscription_type&&*a->subscription_type)){ char *profile=get_profile(t),*sub=json_get_string(profile,"subscriber_id"),*typ=json_get_string(profile,"subscription_type"); int type_changed=typ&&*typ&&(!a->subscription_type||strcmp(a->subscription_type,typ)); changed|=account_replace_if_changed(&a->subscriber_id,sub); changed|=account_replace_if_changed(&a->subscription_type,typ); if(type_changed) response_cache_invalidate_account(a); free(profile); free(sub); free(typ); } if(changed) a->dirty=1; return changed; }
static char *get_quota(Tokens *t) { return api_request("api/v8/packages/quota-details", "{\"is_enterprise\":false,\"lang\":\"en\",\"family_member_id\":\"\"}", t->id_token); }
static char *get_balance_api(Tokens *t) { return api_request("api/v8/packages/balance-and-credit", "{\"is_enterprise\":false,\"lang\":\"en\"}", t->id_token); }
static char *get_tiering_info_api(Tokens *t) { return api_request("gamification/api/v8/loyalties/tiering/info", "{\"is_enterprise\":false,\"lang\":\"en\"}", t->id_token); }
static char *get_transaction_history_api(Tokens *t) { return api_request("payments/api/v8/transaction-history", "{\"is_enterprise\":false,\"lang\":\"en\"}", t->id_token); }
static char *get_pending_transactions_api(Tokens *t) { return get_profile(t); }
static char *get_notifications_api(Tokens *t) { char *at=json_escape(t->access_token),*payload=xasprintf("{\"access_token\":\"%s\"}",at),*resp=api_request("dashboard/api/v8/segments",payload,t->id_token); free(at); free(payload); return resp; }
static char *get_notification_detail_api(Tokens *t,const char *notification_id){ char *id=json_escape(notification_id),*payload=xasprintf("{\"is_enterprise\":false,\"lang\":\"en\",\"notification_id\":\"%s\"}",id),*resp=api_request("api/v8/notification/detail",payload,t->id_token); free(id); free(payload); return resp; }
static char *get_transaction_status_api(Tokens *t,const char *transaction_id,const char *status_arg){ char *tx=json_escape(transaction_id),*st=json_escape(status_arg?status_arg:""); char *payload=xasprintf("{\"transaction_id\":\"%s\",\"is_enterprise\":false,\"lang\":\"en\",\"status\":\"%s\"}",tx,st); char *resp=api_request("payments/api/v8/pending-detail",payload,t->id_token); free(tx); free(st); free(payload); return resp; }
static char *get_redeemables_api(Tokens *t,int is_enterprise){ char *payload=xasprintf("{\"is_enterprise\":%s,\"lang\":\"en\"}",is_enterprise?"true":"false"); char *resp=api_request("api/v8/personalization/redeemables",payload,t->id_token); free(payload); return resp; }
static char *get_store_segments(Tokens *t,int is_enterprise){ char *payload=xasprintf("{\"is_enterprise\":%s,\"lang\":\"en\"}",is_enterprise?"true":"false"); char *resp=api_request("api/v8/configs/store/segments",payload,t->id_token); free(payload); return resp; }
static char *get_store_packages(Tokens *t,const char *subs_type,int is_enterprise){ char *st=json_escape(subs_type&&*subs_type?subs_type:"PREPAID"); char *payload=xasprintf("{\"is_enterprise\":%s,\"filters\":[{\"unit\":\"THOUSAND\",\"id\":\"FIL_SEL_P\",\"type\":\"PRICE\",\"items\":[]},{\"unit\":\"GB\",\"id\":\"FIL_SEL_MQ\",\"type\":\"DATA_TYPE\",\"items\":[]},{\"unit\":\"PACKAGE_NAME\",\"id\":\"FIL_PKG_N\",\"type\":\"PACKAGE_NAME\",\"items\":[{\"id\":\"\",\"label\":\"\"}]},{\"unit\":\"DAY\",\"id\":\"FIL_SEL_V\",\"type\":\"VALIDITY\",\"items\":[]}],\"substype\":\"%s\",\"text_search\":\"\",\"lang\":\"en\"}",is_enterprise?"true":"false",st); char *resp=api_request("api/v9/xl-stores/options/search",payload,t->id_token); free(st); free(payload); return resp; }
static char *get_store_family_list(Tokens *t,const char *subs_type,int is_enterprise){ char *st=json_escape(subs_type&&*subs_type?subs_type:"PREPAID"); char *payload=xasprintf("{\"is_enterprise\":%s,\"subs_type\":\"%s\",\"lang\":\"en\"}",is_enterprise?"true":"false",st); char *resp=api_request("api/v8/xl-stores/options/search/family-list",payload,t->id_token); free(st); free(payload); return resp; }
static char *get_store_family_packages_context_result(Tokens *t,const char *family_code,const char *enterprise_arg,const char *migration_arg,PackageContext *resolved_context,DecoyResolveResult *result_out){
	const char *migrations[]={"NONE","PRE_TO_PRIOH","PRIOH_TO_PRIO","PRIO_TO_PRIOH"};
	size_t migration_n=sizeof(migrations)/sizeof(migrations[0]);
	if(migration_arg&&*migration_arg){ migrations[0]=migration_arg; migration_n=1; }
	int enterprise_values[2]={0,1},enterprise_n=2;
	if(enterprise_arg&&*enterprise_arg){ enterprise_values[0]=arg_true(enterprise_arg); enterprise_n=1; }
	if(result_out) *result_out=DECOY_RESOLVE_FAILED;
	if(resolved_context) package_context_free(resolved_context);
	PackageContext error_151_context={0};
	char *fc=json_escape(family_code),*last=NULL,*error_151=NULL;
	for(size_t mi=0;mi<migration_n;mi++){
		for(int ei=0;ei<enterprise_n;ei++){
			char *payload=xasprintf("{\"is_show_tagging_tab\":true,\"is_dedicated_event\":true,\"is_transaction_routine\":false,\"migration_type\":\"%s\",\"package_family_code\":\"%s\",\"is_autobuy\":false,\"is_enterprise\":%s,\"is_pdlp\":true,\"referral_code\":\"\",\"is_migration\":false,\"lang\":\"en\"}",migrations[mi],fc,enterprise_values[ei]?"true":"false");
			char *resp=api_request("api/v8/xl-stores/options/list",payload,t->id_token);
			free(payload);
			char *status=json_get_string(resp,"status"),*data=json_object_dup(resp,"data"),*family=data?json_object_dup(data,"package_family"):NULL,*name=family?json_get_string(family,"name"):NULL;
			int ok=status&&!strcmp(status,"SUCCESS")&&name&&*name;
			free(status); free(data); free(family); free(name);
			if(ok){
				if(resolved_context) package_context_set(resolved_context,"",family_code,"",enterprise_values[ei],migrations[mi],0,0);
				if(result_out) *result_out=DECOY_RESOLVE_OK;
				package_context_free(&error_151_context);
				free(last); free(error_151); free(fc); return resp;
			}
			if(resolved_context) package_context_set(resolved_context,"",family_code,"",enterprise_values[ei],migrations[mi],0,0);
			if(api_auth_failed(resp)){
				if(result_out) *result_out=DECOY_RESOLVE_RETRY_AUTH;
				package_context_free(&error_151_context);
				free(last); free(error_151); free(fc);
				return resp;
			}
			if(!error_151&&api_error_151(resp)){
				error_151=xstrdup(resp);
				package_context_set(&error_151_context,"",family_code,"",enterprise_values[ei],migrations[mi],0,0);
			}
			free(last); last=resp;
		}
	}
	free(fc);
	if(error_151){
		if(resolved_context) package_context_copy(resolved_context,&error_151_context);
		if(result_out) *result_out=DECOY_RESOLVE_RETRY_151;
		package_context_free(&error_151_context);
		free(last);
		return error_151;
	}
	package_context_free(&error_151_context);
	return last?last:xstrdup("{\"status\":\"FAILED\",\"message\":\"family not found\"}");
}
static char *get_store_family_packages_context(Tokens *t,const char *family_code,const char *enterprise_arg,const char *migration_arg){ return get_store_family_packages_context_result(t,family_code,enterprise_arg,migration_arg,NULL,NULL); }
static char *get_store_family_packages(Tokens *t,const char *family_code,const char *enterprise_arg){ return get_store_family_packages_context(t,family_code,enterprise_arg,NULL); }
static char *unsubscribe_quota(Tokens *t,const char *quota_code,const char *subtype,const char *domain){ char *qc=json_escape(quota_code),*st=json_escape(subtype?subtype:""),*dm=json_escape(domain?domain:""); char *payload=xasprintf("{\"product_subscription_type\":\"%s\",\"quota_code\":\"%s\",\"product_domain\":\"%s\",\"is_enterprise\":false,\"unsubscribe_reason_code\":\"\",\"lang\":\"en\",\"family_member_id\":\"\"}",st,qc,dm); char *resp=api_request("api/v8/packages/unsubscribe",payload,t->id_token); free(qc); free(st); free(dm); free(payload); return resp; }

static const char *payment_config_missing(void){ const char *m=config_missing(); if(m) return m; if(!cfg.encrypted_field_key[0]) return "ENCRYPTED_FIELD_KEY"; return NULL; }
static long long balance_field(const char *json,const char *key,long long def){ char *data=json_object_dup(json,"data"),*balance=data?json_object_dup(data,"balance"):NULL,*credit=data?json_object_dup(data,"credit"):NULL; long long v=balance?json_get_ll_any(balance,key,LLONG_MIN):LLONG_MIN; if(v<=0&&credit){ long long c=json_get_ll_any(credit,key,LLONG_MIN); if(c!=LLONG_MIN) v=c; } if(v==LLONG_MIN) v=json_get_ll_any(json,key,def); free(data); free(balance); free(credit); return v; }
static long long balance_remaining(const char *json){ return balance_field(json,"remaining",-1); }
static long long balance_expired_at(const char *json){ return balance_field(json,"expired_at",0); }
static char *get_package_detail_api_ex(Tokens *t,const char *option_code,const char *family_code,const char *variant_code,int is_enterprise,const char *migration_type){
	char *oc=json_escape(option_code),*fc=json_escape(family_code&&strcmp(family_code,"-")?family_code:""),*vc=json_escape(variant_code&&strcmp(variant_code,"-")?variant_code:""),*mt=json_escape(migration_type&&*migration_type?migration_type:"NONE");
	char *payload=xasprintf("{\"is_transaction_routine\":false,\"migration_type\":\"%s\",\"package_family_code\":\"%s\",\"family_role_hub\":\"\",\"is_autobuy\":false,\"is_enterprise\":%s,\"is_shareable\":false,\"is_migration\":false,\"lang\":\"en\",\"package_option_code\":\"%s\",\"is_upsell_pdp\":false,\"package_variant_code\":\"%s\"}",mt,fc,is_enterprise?"true":"false",oc,vc);
	char *resp=api_request("api/v8/xl-stores/options/detail",payload,t->id_token);
	free(oc); free(fc); free(vc); free(mt); free(payload);
	return resp;
}
static char *get_package_detail_api(Tokens *t,const char *option_code){ return get_package_detail_api_ex(t,option_code,"","",0,"NONE"); }
static int package_context_enterprise(const PackageContext *context){ return context&&context->resolved&&context->is_enterprise; }
static const PackageContext *payment_cart_context(PaymentQuote *quotes,int count){
	for(int i=0;i<count;i++) if(quotes[i].context.resolved) return &quotes[i].context;
	return NULL;
}
static char *intercept_page_api_context(Tokens *t,const char *option_code,const PackageContext *context){
	char *oc=json_escape(option_code);
	char *payload=xasprintf("{\"is_enterprise\":%s,\"lang\":\"en\",\"package_option_code\":\"%s\"}",package_context_enterprise(context)?"true":"false",oc);
	char *resp=api_request("misc/api/v8/utility/intercept-page",payload,t->id_token);
	free(oc); free(payload);
	return resp;
}
static char *intercept_page_api(Tokens *t,const char *option_code){ return intercept_page_api_context(t,option_code,NULL); }
static char *payment_methods_api(Tokens *t,PaymentQuote *q){
	char *ic=json_escape(q->item_code),*tc=json_escape(q->token_confirmation);
	char *payload=xasprintf("{\"payment_type\":\"PURCHASE\",\"is_enterprise\":%s,\"payment_target\":\"%s\",\"lang\":\"en\",\"is_referral\":false,\"token_confirmation\":\"%s\"}",package_context_enterprise(&q->context)?"true":"false",ic,tc);
	char *resp=api_request("payments/api/v8/payment-methods-option",payload,t->id_token);
	free(ic); free(tc); free(payload);
	return resp;
}
static void payment_quote_free(PaymentQuote *q){
	free(q->item_code); free(q->item_name); free(q->family); free(q->validity); free(q->payment_for); free(q->token_confirmation);
	package_context_free(&q->context);
	memset(q,0,sizeof(*q));
}
static char *api_response_summary(const char *response){
	char *status=json_get_string(response,"status"),*code=json_get_string(response,"code");
	long long numeric_code=code?-1:json_get_ll_any(response,"code",-1);
	char *summary=xasprintf("status=%s code=%s",status&&*status?status:"UNKNOWN",code&&*code?code:(numeric_code>=0?"numeric":"UNKNOWN"));
	if(!code&&numeric_code>=0){ free(summary); summary=xasprintf("status=%s code=%lld",status&&*status?status:"UNKNOWN",numeric_code); }
	if(api_auth_failed(response)){ char *base=summary; summary=xasprintf("%s auth_error=true",base); free(base); }
	free(status); free(code);
	return summary;
}
static char *package_context_error(const char *stage,const PackageContext *context,const char *summary){
	int unresolved=context&&context->migration_type&&!strcmp(context->migration_type,"(unresolved)");
	const char *enterprise=!context||!context->migration_type?"(unknown)":(unresolved?"(unresolved)":(context->is_enterprise?"true":"false"));
	char package_number[32];
	if(context&&context->package_number>0) snprintf(package_number,sizeof(package_number),"%d",context->package_number);
	else snprintf(package_number,sizeof(package_number),"(unknown)");
	return xasprintf("%s: family=%s nomor_paket=%s variant=%s is_enterprise=%s migration_type=%s; %s",stage,context&&context->family_code&&*context->family_code?context->family_code:"(unknown)",package_number,context&&context->variant_code&&*context->variant_code?context->variant_code:"(unknown)",enterprise,context&&context->migration_type&&*context->migration_type?context->migration_type:"(unknown)",summary?summary:"request failed");
}
static char *payment_response_add_context(char *response,const PackageContext *context){
	if(!response||!context||!context->resolved) return response;
	char *status=json_get_string(response,"status");
	int success=status&&!strcmp(status,"SUCCESS");
	free(status);
	if(success) return response;
	char *option=json_escape(context->option_code),*family=json_escape(context->family_code),*variant=json_escape(context->variant_code),*migration=json_escape(context->migration_type);
	if(json_document_ok(response)){
		size_t length=strlen(response),close=length;
		while(close&&isspace((unsigned char)response[close-1])) close--;
		if(close&&response[close-1]=='}'){
			size_t open=0; while(open<close&&isspace((unsigned char)response[open])) open++;
			size_t content=open+1; while(content+1<close&&isspace((unsigned char)response[content])) content++;
			int has_fields=content+1<close;
			char *joined=xasprintf("%.*s%s\"package_context\":{\"option_code\":\"%s\",\"family_code\":\"%s\",\"package_number\":%d,\"variant_code\":\"%s\",\"is_enterprise\":%s,\"migration_type\":\"%s\"}}%s",(int)(close-1),response,has_fields?",":"",option,family,context->package_number,variant,context->is_enterprise?"true":"false",migration,response+close);
			free(option); free(family); free(variant); free(migration); free(response);
			return joined;
		}
	}
	char *message=package_context_error("settlement gagal",context,"response API tidak valid"),*escaped=json_escape(message),*joined=xasprintf("{\"status\":\"FAILED\",\"message\":\"%s\",\"package_context\":{\"option_code\":\"%s\",\"family_code\":\"%s\",\"package_number\":%d,\"variant_code\":\"%s\",\"is_enterprise\":%s,\"migration_type\":\"%s\"}}",escaped,option,family,context->package_number,variant,context->is_enterprise?"true":"false",migration);
	free(message); free(escaped); free(option); free(family); free(variant); free(migration); free(response);
	return joined;
}
static DecoyResolveResult payment_quote_load_context(Tokens *t,const PackageContext *context,PaymentQuote *q,char **err){
	if(!context||!context->resolved||!context->option_code||!*context->option_code||!context->family_code||!*context->family_code||!context->variant_code||!*context->variant_code||!context->migration_type||!*context->migration_type){
		if(err) *err=package_context_error("resolver decoy menghasilkan context tidak lengkap",context,"detail tidak dikirim");
		return DECOY_RESOLVE_FAILED;
	}
	char *detail=get_package_detail_api_ex(t,context->option_code,context->family_code,context->variant_code,context->is_enterprise,context->migration_type);
	int code_151=api_error_151(detail);
	int auth_failed=api_auth_failed(detail);
	char *detail_status=json_get_string(detail,"status"),*data=json_object_dup(detail,"data"),*option=data?json_object_dup(data,"package_option"):NULL;
	if(!detail_status||strcmp(detail_status,"SUCCESS")||!data||!option){
		if(err){ char *summary=api_response_summary(detail); *err=package_context_error("package detail decoy gagal",context,summary); free(summary); }
		free(detail_status); free(data); free(option); free(detail);
		return code_151?DECOY_RESOLVE_RETRY_151:(auth_failed?DECOY_RESOLVE_RETRY_AUTH:DECOY_RESOLVE_FAILED);
	}
	free(detail_status);
	char *family=json_object_dup(data,"package_family"); if(!family) family=xstrdup("{}");
	char *variant=json_object_dup(data,"package_detail_variant"); if(!variant) variant=xstrdup("{}");
	char *option_name=json_get_string(option,"name"),*variant_name=json_get_string(variant,"name"),*family_name=json_get_string(family,"name"),*validity=json_get_string(option,"validity"),*payment_for=json_get_string(family,"payment_for"),*token=json_get_string(data,"token_confirmation");
	char *returned_option=json_get_string(option,"package_option_code"),*returned_family=json_get_string(family,"package_family_code"),*returned_variant=json_get_string(variant,"package_variant_code");
	long long price=json_get_ll_any(option,"price",-1),timestamp=json_get_ll_any(data,"timestamp",-1);
	if((returned_family&&*returned_family&&strcmp(returned_family,context->family_code))||(returned_variant&&*returned_variant&&strcmp(returned_variant,context->variant_code))){
		if(err) *err=package_context_error("package detail decoy mengembalikan context berbeda",context,"response family/variant tidak cocok");
		free(detail); free(data); free(option); free(family); free(variant); free(option_name); free(variant_name); free(family_name); free(validity); free(payment_for); free(token); free(returned_option); free(returned_family); free(returned_variant);
		return DECOY_RESOLVE_FAILED;
	}
	if(!returned_option||!*returned_option||!token||price<0){
		if(err) *err=package_context_error("package detail decoy tidak memiliki field quote wajib",context,"package_option_code/token_confirmation/price tidak tersedia");
		free(detail); free(data); free(option); free(family); free(variant); free(option_name); free(variant_name); free(family_name); free(validity); free(payment_for); free(token); free(returned_option); free(returned_family); free(returned_variant);
		return DECOY_RESOLVE_FAILED;
	}
	char *item_name=NULL;
	if(variant_name&&*variant_name&&option_name&&*option_name) item_name=xasprintf("%s %s",variant_name,option_name);
	else item_name=xstrdup(option_name&&*option_name?option_name:(variant_name&&*variant_name?variant_name:returned_option));
	if(!payment_for||!*payment_for){ free(payment_for); payment_for=xstrdup("BUY_PACKAGE"); }
	/* Upstream uses the option code returned by package detail for PaymentItem;
	 * the family-list option code is only the input used to request that detail. */
	q->item_code=xstrdup(returned_option); q->item_name=item_name; q->family=xstrdup(family_name?family_name:""); q->validity=xstrdup(validity?validity:""); q->payment_for=xstrdup(payment_for); q->token_confirmation=xstrdup(token); q->price=price; q->balance=-1; q->timestamp=timestamp;
	package_context_copy(&q->context,context);
	free(detail); free(data); free(option); free(family); free(variant); free(option_name); free(variant_name); free(family_name); free(validity); free(payment_for); free(token); free(returned_option); free(returned_family); free(returned_variant);
	return DECOY_RESOLVE_OK;
}
static int payment_quote_load(Tokens *t,const char *option_code,PaymentQuote *q,char **err){
	PackageContext context={0};
	package_context_set(&context,option_code,"","",0,"NONE",0,0);
	char *detail=get_package_detail_api(t,option_code);
	if(!strstr(detail,"\"data\"")){ if(err){ char *summary=api_response_summary(detail); *err=xasprintf("package detail failed: %s",summary); free(summary); } free(detail); package_context_free(&context); return -1; }
	char *data=json_object_dup(detail,"data"); if(!data) data=xstrdup(detail);
	char *option=json_object_dup(data,"package_option"); if(!option) option=xstrdup("{}");
	char *family=json_object_dup(data,"package_family"); if(!family) family=xstrdup("{}");
	char *variant=json_object_dup(data,"package_detail_variant"); if(!variant) variant=xstrdup("{}");
	char *option_name=json_get_string(option,"name"),*variant_name=json_get_string(variant,"name"),*family_name=json_get_string(family,"name"),*validity=json_get_string(option,"validity"),*payment_for=json_get_string(family,"payment_for"),*token=json_get_string(data,"token_confirmation");
	long long price=json_get_ll_any(option,"price",-1),timestamp=json_get_ll_any(data,"timestamp",-1);
	if(!token||price<0){ if(err) *err=xstrdup("package fields missing: token_confirmation/price tidak tersedia"); free(detail); free(data); free(option); free(family); free(variant); free(option_name); free(variant_name); free(family_name); free(validity); free(payment_for); free(token); package_context_free(&context); return -1; }
	char *item_name=NULL;
	if(variant_name&&*variant_name&&option_name&&*option_name) item_name=xasprintf("%s %s",variant_name,option_name);
	else item_name=xstrdup(option_name&&*option_name?option_name:(variant_name&&*variant_name?variant_name:option_code));
	if(!payment_for||!*payment_for){ free(payment_for); payment_for=xstrdup("BUY_PACKAGE"); }
	q->item_code=xstrdup(option_code); q->item_name=item_name; q->family=xstrdup(family_name?family_name:""); q->validity=xstrdup(validity?validity:""); q->payment_for=xstrdup(payment_for); q->token_confirmation=xstrdup(token); q->price=price; q->balance=-1; q->timestamp=timestamp;
	free(detail); free(data); free(option); free(family); free(variant); free(option_name); free(variant_name); free(family_name); free(validity); free(payment_for); free(token); package_context_free(&context);
	return 0;
}
static void payment_quote_print_json(PaymentQuote *q){
	printf("{\"product\":{\"code\":"); json_str(q->item_code); printf(",\"name\":"); json_str(q->item_name); printf(",\"family\":"); json_str(q->family); printf(",\"validity\":"); json_str(q->validity); printf(",\"payment_for\":"); json_str(q->payment_for); printf(",\"price\":%lld,\"timestamp\":%lld},\"pulsa\":{\"balance\":%lld}",q->price,q->timestamp,q->balance);
	if(q->context.resolved){ printf(",\"package_context\":{\"option_code\":"); json_str(q->context.option_code); printf(",\"family_code\":"); json_str(q->context.family_code); printf(",\"variant_code\":"); json_str(q->context.variant_code); printf(",\"is_enterprise\":%s,\"migration_type\":",q->context.is_enterprise?"true":"false"); json_str(q->context.migration_type); printf(",\"package_number\":%d}",q->context.package_number); }
	putchar('}');
}
static int payment_token_index(int n,int idx){ if(idx<0) idx=n+idx; return (idx>=0&&idx<n)?idx:0; }
#ifdef BITSXL_TEST
static char *(*bitsxl_test_balance_settlement_hook)(const char *,const char *,Tokens *);
#endif
static char *send_balance_settlement(const char *path,const char *payload,Tokens *tokens,const char *targets,const char *token_payment,const char *payment_for,long long payment_timestamp){
#ifdef BITSXL_TEST
	if(bitsxl_test_balance_settlement_hook) return bitsxl_test_balance_settlement_hook(path,payload,tokens);
#endif
	return api_request_payment(path,payload,tokens,targets,token_payment,payment_for,payment_timestamp);
}
static char *payment_pulsa_settle_many_raw(Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,int token_idx,const char *payment_for_override){
	if(n<1) return xstrdup("{\"status\":\"FAILED\",\"message\":\"empty cart\"}");
	int ti=payment_token_index(n,token_idx);
	const char *payment_for=payment_for_override?payment_for_override:q[0].payment_for;
	const PackageContext *settlement_context=payment_cart_context(q,n);
	const char *settlement_migration=settlement_context&&settlement_context->migration_type?settlement_context->migration_type:"";
	/* Upstream binds intercept to item 0 and payment-method to token_idx; settlement context is cart-level. */
	char *intercept=intercept_page_api_context(t,q[0].item_code,&q[0].context);
	free(intercept);
	char *methods=payment_methods_api(t,&q[ti]);
	if(api_auth_failed(methods)) mutation_result(acc,t,methods);
	char *status=json_get_string(methods,"status"),*token_payment=json_get_string(methods,"token_payment");
	long long payment_ts=json_get_ll_any(methods,"timestamp",-1);
	if(!status||strcmp(status,"SUCCESS")||!token_payment||payment_ts<0){
		char *summary=api_response_summary(methods),*message=settlement_context?package_context_error("payment methods gagal",settlement_context,summary):xasprintf("payment methods failed: %s",summary),*escaped=json_escape(message),*error=xasprintf("{\"status\":\"FAILED\",\"message\":\"%s\"}",escaped);
		free(summary); free(message); free(escaped); free(status); free(token_payment); free(methods);
		return error;
	}
	char *targets=xstrdup(""),*items=xstrdup("");
	for(int i=0;i<n;i++){
		char *old=targets;
		targets=xasprintf("%s%s%s",old,i?";":"",q[i].item_code);
		free(old);
		char *code=json_escape(q[i].item_code),*name=json_escape(q[i].item_name),*tc=json_escape(q[i].token_confirmation);
		old=items;
		items=xasprintf("%s%s{\"item_code\":\"%s\",\"product_type\":\"\",\"item_price\":%lld,\"item_name\":\"%s\",\"tax\":0,\"token_confirmation\":\"%s\"}",old,i?",":"",code,q[i].price,name,tc);
		free(old); free(code); free(name); free(tc);
	}
	char *ept=encrypted_empty_field(),*eauth=encrypted_empty_field(),*pf=json_escape(payment_for?payment_for:""),*access=json_escape(t->access_token),*tp=json_escape(token_payment),*migration=json_escape(settlement_migration);
	long long now=(long long)time(NULL),original=q[n-1].price;
	char *payload=xasprintf("{\"total_discount\":0,\"is_enterprise\":%s,\"payment_token\":\"\",\"token_payment\":\"%s\",\"activated_autobuy_code\":\"\",\"cc_payment_type\":\"\",\"is_myxl_wallet\":false,\"pin\":\"\",\"ewallet_promo_id\":\"\",\"members\":[],\"total_fee\":0,\"fingerprint\":\"\",\"autobuy_threshold_setting\":{\"label\":\"\",\"type\":\"\",\"value\":0},\"is_use_point\":false,\"lang\":\"en\",\"payment_method\":\"BALANCE\",\"timestamp\":%lld,\"points_gained\":0,\"can_trigger_rating\":false,\"akrab_members\":[],\"akrab_parent_alias\":\"\",\"referral_unique_code\":\"\",\"coupon\":\"\",\"payment_for\":\"%s\",\"with_upsell\":false,\"topup_number\":\"\",\"stage_token\":\"\",\"authentication_id\":\"\",\"encrypted_payment_token\":\"%s\",\"token\":\"\",\"token_confirmation\":\"\",\"access_token\":\"%s\",\"wallet_number\":\"\",\"encrypted_authentication_id\":\"%s\",\"additional_data\":{\"original_price\":%lld,\"is_spend_limit_temporary\":false,\"migration_type\":\"%s\",\"akrab_m2m_group_id\":\"false\",\"spend_limit_amount\":0,\"is_spend_limit\":false,\"mission_id\":\"\",\"tax\":0,\"quota_bonus\":0,\"cashtag\":\"\",\"is_family_plan\":false,\"combo_details\":[],\"is_switch_plan\":false,\"discount_recurring\":0,\"is_akrab_m2m\":false,\"balance_type\":\"PREPAID_BALANCE\",\"has_bonus\":false,\"discount_promo\":0},\"total_amount\":%lld,\"is_using_autobuy\":false,\"items\":[%s]}",settlement_context&&settlement_context->is_enterprise?"true":"false",tp,now,pf,ept,access,eauth,original,migration,total,items);
	mutation_prepare(acc);
	char *resp=send_balance_settlement("payments/api/v8/settlement-multipayment",payload,t,targets,token_payment,payment_for,payment_ts);
	mutation_result(acc,t,resp);
	free(status); free(token_payment); free(methods); free(targets); free(items); free(ept); free(eauth); free(pf); free(access); free(tp); free(migration); free(payload);
	return resp;
}
static char *payment_pulsa_settle_many_ex(Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,int token_idx,const char *payment_for_override){
	char *response=payment_pulsa_settle_many_raw(acc,t,q,n,total,token_idx,payment_for_override);
	return payment_response_add_context(response,payment_cart_context(q,n));
}
static char *payment_pulsa_settle_many(Account *acc,Tokens *t,PaymentQuote *q,int n,long long total){ return payment_pulsa_settle_many_ex(acc,t,q,n,total,0,NULL); }
static long long payment_bizz_amount(const char *resp){
	if(!resp||!json_document_ok(resp)) return -1;
	char *status=json_get_string(resp,"status"),*message=json_get_string(resp,"message");
	int failed=status&&strcmp(status,"SUCCESS");
	free(status);
	if(!failed||!message){ free(message); return -1; }
	const char *p=strstr(message,"Bizz-err.Amount.Total");
	if(!p||(p=strchr(p,'='))==NULL){ free(message); return -1; }
	p++;
	while(isspace((unsigned char)*p)) p++;
	errno=0;
	char *end=NULL;
	long long value=strtoll(p,&end,10);
	int ok=!errno&&end&&end!=p&&value>=0;
	free(message);
	return ok?value:-1;
}
static void subscription_upper(Account *acc,char *b,size_t sz){ snprintf(b,sz,"%s",acc&&acc->subscription_type?acc->subscription_type:""); for(size_t i=0;b[i];i++) b[i]=(char)toupper((unsigned char)b[i]); }
static int subscription_priohybrid(Account *acc){ char b[128]; subscription_upper(acc,b,sizeof(b)); return !strcmp(b,"PRIOHYBRID"); }
static int subscription_prio(Account *acc){ char b[128]; subscription_upper(acc,b,sizeof(b)); return !strcmp(b,"PRIORITAS")||!strcmp(b,"PRIOHYBRID")||!strcmp(b,"GO"); }
static char *payment_items_json(PaymentQuote *q,int n,char **targets_out){ char *targets=xstrdup(""),*items=xstrdup(""); for(int i=0;i<n;i++){ char *old=targets; targets=xasprintf("%s%s%s",old,i?";":"",q[i].item_code); free(old); char *code=json_escape(q[i].item_code),*name=json_escape(q[i].item_name),*tc=json_escape(q[i].token_confirmation); old=items; items=xasprintf("%s%s{\"item_code\":\"%s\",\"product_type\":\"\",\"item_price\":%lld,\"item_name\":\"%s\",\"tax\":0,\"token_confirmation\":\"%s\"}",old,i?",":"",code,q[i].price,name,tc); free(old); free(code); free(name); free(tc); } *targets_out=targets; return items; }
static void package_context_finish(PackageContext *context,const char *option_code,const char *variant_code,int package_number){
	char *family=xstrdup(context&&context->family_code?context->family_code:""),*migration=xstrdup(context&&context->migration_type?context->migration_type:"NONE");
	int enterprise=context&&context->is_enterprise;
	package_context_set(context,option_code,family,variant_code,enterprise,migration,package_number,1);
	free(family); free(migration);
}
static void package_context_note_variant(PackageContext *context,const char *variant_code){
	if(!context) return;
	char *family=xstrdup(context->family_code?context->family_code:""),*migration=xstrdup(context->migration_type?context->migration_type:"NONE"),*option=xstrdup(context->option_code?context->option_code:"");
	int enterprise=context->is_enterprise,package_number=context->package_number,resolved=context->resolved;
	package_context_set(context,option,family,variant_code,enterprise,migration,package_number,resolved);
	free(family); free(migration); free(option);
}
static DecoyResolveResult family_variant_order_context(Tokens *t,const char *family_code,const char *variant_code,int order,const char *enterprise_arg,const char *migration_arg,PackageContext *context,char **err){
	if(err) *err=NULL;
	DecoyResolveResult lookup_result=DECOY_RESOLVE_FAILED;
	char *shop=get_store_family_packages_context_result(t,family_code,enterprise_arg,migration_arg,context,&lookup_result);
	package_context_note_variant(context,variant_code);
	if(lookup_result!=DECOY_RESOLVE_OK){
		if(err){ char *summary=api_response_summary(shop); *err=package_context_error("family lookup gagal",context,summary); free(summary); }
		free(shop);
		return lookup_result;
	}
	char *data=json_object_dup(shop,"data");
	if(!data) data=xstrdup(shop);
	const char *vs,*ve;
	if(json_array_span(data,"package_variants",&vs,&ve)){
		if(err){ char *summary=api_response_summary(shop); *err=package_context_error("family lookup gagal",context,summary); free(summary); }
		free(data); free(shop);
		return DECOY_RESOLVE_FAILED;
	}
	char *found=NULL; int flattened_number=0;
	const char *p=vs;
	while(p<ve&&!found){
		char *variant=next_object(&p,ve); if(!variant) break;
		char *vc=json_get_string(variant,"package_variant_code");
		const char *os,*oe;
		if(!json_array_span(variant,"package_options",&os,&oe)){
			const char *op=os;
			while(op<oe&&!found){
				char *option=next_object(&op,oe); if(!option) break;
				flattened_number++;
				if(vc&&!strcmp(vc,variant_code)&&json_get_ll_any(option,"order",-1)==order) found=json_get_string(option,"package_option_code");
				free(option);
			}
		}
		free(vc); free(variant);
	}
	if(found&&!*found){ free(found); found=NULL; }
	if(found) package_context_finish(context,found,variant_code,flattened_number);
	else if(err){ char *summary=xasprintf("variant/order target tidak tersedia (order=%d)",order); *err=package_context_error("package tidak ditemukan",context,summary); free(summary); }
	free(found); free(data); free(shop);
	return context&&context->resolved?DECOY_RESOLVE_OK:DECOY_RESOLVE_FAILED;
}
static char *find_family_option_code_context(Tokens *t,const char *family_code,const char *variant_code,int order,const char *enterprise_arg,const char *migration_arg,char **err){
	PackageContext context={0};
	if(family_variant_order_context(t,family_code,variant_code,order,enterprise_arg,migration_arg,&context,err)){ package_context_free(&context); return NULL; }
	char *code=xstrdup(context.option_code);
	package_context_free(&context);
	return code;
}
static char *find_family_option_code(Tokens *t,const char *family_code,const char *variant_code,int order,const char *enterprise_arg,char **err){ return find_family_option_code_context(t,family_code,variant_code,order,enterprise_arg,NULL,err); }
static int decoy_family_code_valid(const char *s){
	if(!s) return 0;
	size_t n=strlen(s);
	if(n<1||n>128) return 0;
	for(size_t i=0;i<n;i++) if(isspace((unsigned char)s[i])||iscntrl((unsigned char)s[i])) return 0;
	return 1;
}
static int decoy_package_number_parse(const char *s,int *out){
	if(!s||!*s) return -1;
	errno=0; char *end=NULL; long value=strtol(s,&end,10);
	if(errno||end==s||*end||value<1||value>INT_MAX) return -1;
	*out=(int)value;
	return 0;
}
static int decoy_pair_choice(const char *category,const char *family,const char *number,int fallback,DecoyConfigChoice *choice,char **err){
	int has_family=family&&*family,has_number=number&&*number;
	if(has_family!=has_number){
		if(err) *err=xasprintf("konfigurasi decoy %s tidak lengkap: Family Code dan Nomor Paket harus sama-sama diisi atau dikosongkan",category);
		return -1;
	}
	memset(choice,0,sizeof(*choice));
	choice->category=category;
	choice->prioritas_fallback=fallback;
	if(!has_family) return 0;
	if(!decoy_family_code_valid(family)){
		if(err) *err=xasprintf("konfigurasi decoy %s tidak valid: Family Code harus 1-128 karakter tanpa spasi",category);
		return -1;
	}
	if(decoy_package_number_parse(number,&choice->package_number)){
		if(err) *err=xasprintf("konfigurasi decoy %s tidak valid: Nomor Paket harus bilangan bulat >= 1",category);
		return -1;
	}
	choice->family_code=family;
	choice->configured=1;
	return 1;
}
static int decoy_config_choice(Account *acc,DecoyConfigChoice *choice,char **err){
	if(err) *err=NULL;
	if(subscription_priohybrid(acc)){
		int result=decoy_pair_choice("PRIOHYBRID",cfg.decoy_priohybrid_family_code,cfg.decoy_priohybrid_package_number,0,choice,err);
		if(result) return result;
		return decoy_pair_choice("PRIORITAS",cfg.decoy_prioritas_family_code,cfg.decoy_prioritas_package_number,1,choice,err);
	}
	if(subscription_prio(acc)) return decoy_pair_choice("PRIORITAS",cfg.decoy_prioritas_family_code,cfg.decoy_prioritas_package_number,0,choice,err);
	return decoy_pair_choice("PREPAID",cfg.decoy_prepaid_family_code,cfg.decoy_prepaid_package_number,0,choice,err);
}
static int flattened_family_package_context(const char *shop,int package_number,PackageContext *context,int *total_out,char **err){
	if(err) *err=NULL;
	if(context) context->package_number=package_number;
	char *data=json_object_dup(shop,"data"); if(!data) data=xstrdup(shop);
	const char *vs,*ve;
	if(json_array_span(data,"package_variants",&vs,&ve)){
		if(err){ char *summary=api_response_summary(shop); *err=package_context_error("family lookup gagal",context,summary); free(summary); }
		free(data);
		return -1;
	}
	char *found=NULL,*found_variant=NULL; int index=0;
	const char *vp=vs;
	while(vp<ve){
		char *variant=next_object(&vp,ve); if(!variant) break;
		char *variant_code=json_get_string(variant,"package_variant_code");
		const char *os,*oe;
		if(!json_array_span(variant,"package_options",&os,&oe)){
			const char *op=os;
			while(op<oe){
				char *option=next_object(&op,oe); if(!option) break;
				index++;
				if(index==package_number){
					found=json_get_string(option,"package_option_code");
					if(found&&!*found){ free(found); found=NULL; }
					if(found&&variant_code&&*variant_code) found_variant=xstrdup(variant_code);
				}
				free(option);
			}
		}
		free(variant_code); free(variant);
	}
	if(total_out) *total_out=index;
	if(found&&!found_variant){ free(found); found=NULL; }
	if(found) package_context_finish(context,found,found_variant,package_number);
	else if(err){ char *summary=xasprintf("Nomor Paket %d tidak tersedia/valid; family memiliki %d package option",package_number,index); *err=package_context_error("resolver decoy gagal",context,summary); free(summary); }
	free(found); free(found_variant);
	free(data);
	return context&&context->resolved?0:-1;
}
#ifdef BITSXL_TEST
static char *flattened_family_option_code(const char *shop,int package_number,int *total_out,char **err){
	PackageContext context={0};
	package_context_set(&context,"","","",0,"NONE",0,0);
	if(flattened_family_package_context(shop,package_number,&context,total_out,err)){ package_context_free(&context); return NULL; }
	char *code=xstrdup(context.option_code);
	package_context_free(&context);
	return code;
}
#endif
static DecoyResolveResult decoy_package_context_once(Tokens *t,Account *acc,PackageContext *context,char **err){
	DecoyConfigChoice choice;
	int configured=decoy_config_choice(acc,&choice,err);
	if(configured<0) return DECOY_RESOLVE_FAILED;
	if(configured){
		DecoyResolveResult lookup_result=DECOY_RESOLVE_FAILED;
		char *shop=get_store_family_packages_context_result(t,choice.family_code,"","",context,&lookup_result);
		if(context) context->package_number=choice.package_number;
		DecoyResolveResult result=lookup_result;
		if(lookup_result==DECOY_RESOLVE_OK){
			int total=0;
			result=flattened_family_package_context(shop,choice.package_number,context,&total,err)?DECOY_RESOLVE_FAILED:DECOY_RESOLVE_OK;
		} else if(err){
			char *summary=api_response_summary(shop);
			*err=package_context_error("family lookup gagal",context,summary);
			free(summary);
		}
		if(result&&err&&*err){
			char *detail=*err;
			*err=xasprintf("resolver decoy %s gagal: %s",choice.category,detail);
			free(detail);
		}
		free(shop);
		return result;
	}
	if(subscription_prio(acc)) return family_variant_order_context(t,"2512b72a-a3cd-4c70-a736-132cf2c1f0c0","cff298bd-8ec8-4696-b689-12407d36be15",1,"","",context,err);
	return family_variant_order_context(t,"b0a20d74-0c54-4e3b-8f3f-01e7482e50bf","719d093f-6f8d-46a4-8390-6a0003a172ea",1,"true","NONE",context,err);
}
static int api_error_151(const char *response){
	if(!response) return 0;
	char *code=json_get_string(response,"code");
	int matched=(code&&!strcmp(code,"151"))||json_get_ll_any(response,"code",-1)==151;
	free(code);
	return matched;
}
static char *payment_qris_settle_many_ex(Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,int token_idx,const char *payment_for_override){
	if(n<1) return xstrdup("{\"status\":\"FAILED\",\"message\":\"empty cart\"}");
	int ti=payment_token_index(n,token_idx);
	const char *payment_for=(payment_for_override&&*payment_for_override)?payment_for_override:q[0].payment_for;
	const PackageContext *settlement_context=payment_cart_context(q,n);
	const char *settlement_migration=settlement_context&&settlement_context->migration_type?settlement_context->migration_type:"";
	/* Keep upstream item selection while carrying the resolved context on supported fields. */
	char *intercept=intercept_page_api_context(t,q[0].item_code,&q[0].context);
	free(intercept);
	char *methods=payment_methods_api(t,&q[ti]);
	if(api_auth_failed(methods)) mutation_result(acc,t,methods);
	char *status=json_get_string(methods,"status"),*token_payment=json_get_string(methods,"token_payment");
	long long payment_ts=json_get_ll_any(methods,"timestamp",-1);
	if(!status||strcmp(status,"SUCCESS")||!token_payment||payment_ts<0){
		char *summary=api_response_summary(methods),*message=settlement_context?package_context_error("payment methods QRIS gagal",settlement_context,summary):xasprintf("payment methods failed: %s",summary),*escaped=json_escape(message),*error=xasprintf("{\"status\":\"FAILED\",\"message\":\"%s\"}",escaped);
		free(summary); free(message); free(escaped); free(status); free(token_payment); free(methods);
		return error;
	}
	char *targets=NULL,*items=payment_items_json(q,n,&targets),*pf=json_escape(payment_for?payment_for:""),*access=json_escape(t->access_token),*tp=json_escape(token_payment),*migration=json_escape(settlement_migration);
	long long now=(long long)time(NULL),original=q[0].price;
	char *payload=xasprintf("{\"akrab\":{\"akrab_members\":[],\"akrab_parent_alias\":\"\",\"members\":[]},\"can_trigger_rating\":false,\"total_discount\":0,\"coupon\":\"\",\"payment_for\":\"%s\",\"topup_number\":\"\",\"stage_token\":\"\",\"is_enterprise\":%s,\"autobuy\":{\"is_using_autobuy\":false,\"activated_autobuy_code\":\"\",\"autobuy_threshold_setting\":{\"label\":\"\",\"type\":\"\",\"value\":0}},\"access_token\":\"%s\",\"is_myxl_wallet\":false,\"additional_data\":{\"original_price\":%lld,\"is_spend_limit_temporary\":false,\"migration_type\":\"%s\",\"spend_limit_amount\":0,\"is_spend_limit\":false,\"tax\":0,\"benefit_type\":\"\",\"quota_bonus\":0,\"cashtag\":\"\",\"is_family_plan\":false,\"combo_details\":[],\"is_switch_plan\":false,\"discount_recurring\":0,\"has_bonus\":false,\"discount_promo\":0},\"total_amount\":%lld,\"total_fee\":0,\"is_use_point\":false,\"lang\":\"en\",\"items\":[%s],\"verification_token\":\"%s\",\"payment_method\":\"QRIS\",\"timestamp\":%lld}",pf,settlement_context&&settlement_context->is_enterprise?"true":"false",access,original,migration,total,items,tp,now);
	mutation_prepare(acc);
	char *resp=api_request_payment_method("payments/api/v8/settlement-multipayment/qris",payload,t,targets,token_payment,"QRIS",payment_for,payment_ts);
	mutation_result(acc,t,resp);
	resp=payment_response_add_context(resp,settlement_context);
	free(status); free(token_payment); free(methods); free(targets); free(items); free(pf); free(access); free(tp); free(migration); free(payload);
	return resp;
}
static char *payment_ewallet_settle_many(Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,const char *method,const char *wallet){ if(n<1) return xstrdup("{\"status\":\"FAILED\",\"message\":\"empty cart\"}"); char *intercept=intercept_page_api(t,q[0].item_code); free(intercept); char *methods=payment_methods_api(t,&q[0]); if(api_auth_failed(methods)) mutation_result(acc,t,methods); char *status=json_get_string(methods,"status"),*token_payment=json_get_string(methods,"token_payment"); long long payment_ts=json_get_ll_any(methods,"timestamp",-1); if(!status||strcmp(status,"SUCCESS")||!token_payment||payment_ts<0){ char *err=xasprintf("{\"status\":\"FAILED\",\"message\":\"payment methods failed\",\"response\":%s}",methods?methods:"null"); free(status); free(token_payment); free(methods); return err; } char *targets=NULL,*items=payment_items_json(q,n,&targets),*pf=json_escape(q[0].payment_for),*access=json_escape(t->access_token),*tp=json_escape(token_payment),*pm=json_escape(method),*wn=json_escape(wallet?wallet:""); long long now=(long long)time(NULL); char *payload=xasprintf("{\"akrab\":{\"akrab_members\":[],\"akrab_parent_alias\":\"\",\"members\":[]},\"can_trigger_rating\":false,\"total_discount\":0,\"coupon\":\"\",\"payment_for\":\"%s\",\"topup_number\":\"\",\"is_enterprise\":false,\"autobuy\":{\"is_using_autobuy\":false,\"activated_autobuy_code\":\"\",\"autobuy_threshold_setting\":{\"label\":\"\",\"type\":\"\",\"value\":0}},\"cc_payment_type\":\"\",\"access_token\":\"%s\",\"is_myxl_wallet\":false,\"wallet_number\":\"%s\",\"additional_data\":{},\"total_amount\":%lld,\"total_fee\":0,\"is_use_point\":false,\"lang\":\"en\",\"items\":[%s],\"verification_token\":\"%s\",\"payment_method\":\"%s\",\"timestamp\":%lld}",pf,access,wn,total,items,tp,pm,now); mutation_prepare(acc); char *resp=api_request_payment_method("payments/api/v8/settlement-multipayment/ewallet",payload,t,targets,token_payment,method,q[0].payment_for,payment_ts); mutation_result(acc,t,resp); free(status); free(token_payment); free(methods); free(targets); free(items); free(pf); free(access); free(tp); free(pm); free(wn); free(payload); return resp; }
static char *payment_voucher_settle(Account *acc,Tokens *t,PaymentQuote *q){ if(q->timestamp<0) return xstrdup("{\"status\":\"FAILED\",\"message\":\"missing package timestamp\"}"); char *ept=encrypted_empty_field(),*eauth=encrypted_empty_field(),*access=json_escape(t->access_token),*code=json_escape(q->item_code),*name=json_escape(q->item_name),*tc=json_escape(q->token_confirmation); char *payload=xasprintf("{\"total_discount\":0,\"is_enterprise\":false,\"payment_token\":\"\",\"token_payment\":\"\",\"activated_autobuy_code\":\"\",\"cc_payment_type\":\"\",\"is_myxl_wallet\":false,\"pin\":\"\",\"ewallet_promo_id\":\"\",\"members\":[],\"total_fee\":0,\"fingerprint\":\"\",\"autobuy_threshold_setting\":{\"label\":\"\",\"type\":\"\",\"value\":0},\"is_use_point\":false,\"lang\":\"en\",\"payment_method\":\"BALANCE\",\"timestamp\":%lld,\"points_gained\":0,\"can_trigger_rating\":false,\"akrab_members\":[],\"akrab_parent_alias\":\"\",\"referral_unique_code\":\"\",\"coupon\":\"\",\"payment_for\":\"REDEEM_VOUCHER\",\"with_upsell\":false,\"topup_number\":\"\",\"stage_token\":\"\",\"authentication_id\":\"\",\"encrypted_payment_token\":\"%s\",\"token\":\"\",\"token_confirmation\":\"%s\",\"access_token\":\"%s\",\"wallet_number\":\"\",\"encrypted_authentication_id\":\"%s\",\"additional_data\":{\"original_price\":0,\"is_spend_limit_temporary\":false,\"migration_type\":\"\",\"akrab_m2m_group_id\":\"\",\"spend_limit_amount\":0,\"is_spend_limit\":false,\"mission_id\":\"\",\"tax\":0,\"benefit_type\":\"\",\"quota_bonus\":0,\"cashtag\":\"\",\"is_family_plan\":false,\"combo_details\":[],\"is_switch_plan\":false,\"discount_recurring\":0,\"is_akrab_m2m\":false,\"balance_type\":\"\",\"has_bonus\":false,\"discount_promo\":0},\"total_amount\":0,\"is_using_autobuy\":false,\"items\":[{\"item_code\":\"%s\",\"product_type\":\"\",\"item_price\":%lld,\"item_name\":\"%s\",\"tax\":0}]}",q->timestamp,ept,tc,access,eauth,code,q->price,name); char *sig=make_x_signature_bounty(t->access_token,q->timestamp,q->item_code,q->token_confirmation),*resp; mutation_prepare(acc); resp=api_request_custom_sig("api/v8/personalization/bounties-exchange",payload,t,sig); mutation_result(acc,t,resp); free(ept); free(eauth); free(access); free(code); free(name); free(tc); free(payload); return resp; }
static char *payment_point_settle(Account *acc,Tokens *t,PaymentQuote *q){ if(q->timestamp<0) return xstrdup("{\"status\":\"FAILED\",\"message\":\"missing package timestamp\"}"); const char *path="gamification/api/v8/loyalties/tiering/exchange"; char *code=json_escape(q->item_code),*tc=json_escape(q->token_confirmation); char *payload=xasprintf("{\"item_code\":\"%s\",\"amount\":0,\"partner\":\"\",\"is_enterprise\":false,\"item_name\":\"\",\"lang\":\"en\",\"points\":%lld,\"timestamp\":%lld,\"token_confirmation\":\"%s\"}",code,q->price,q->timestamp,tc); char *sig=make_x_signature_loyalty(q->timestamp,q->item_code,q->token_confirmation,path),*resp; mutation_prepare(acc); resp=api_request_custom_sig(path,payload,t,sig); mutation_result(acc,t,resp); free(code); free(tc); free(payload); return resp; }
static char *payment_gift_settle(Account *acc,Tokens *t,PaymentQuote *q,const char *dest){ if(q->timestamp<0) return xstrdup("{\"status\":\"FAILED\",\"message\":\"missing package timestamp\"}"); const char *path="gamification/api/v8/loyalties/tiering/bounties-allotment"; char *d=json_escape(dest),*code=json_escape(q->item_code),*name=json_escape(q->item_name),*tc=json_escape(q->token_confirmation); long long now=(long long)time(NULL); char *payload=xasprintf("{\"destination_msisdn\":\"%s\",\"item_code\":\"%s\",\"is_enterprise\":false,\"item_name\":\"%s\",\"lang\":\"en\",\"timestamp\":%lld,\"token_confirmation\":\"%s\"}",d,code,name,now,tc); char *sig=make_x_signature_bounty_allotment(q->timestamp,q->item_code,q->token_confirmation,path,dest),*resp; mutation_prepare(acc); resp=api_request_custom_sig(path,payload,t,sig); mutation_result(acc,t,resp); free(d); free(code); free(name); free(tc); free(payload); return resp; }

static int load_env_file(const char *path) {
	FILE *f=fopen(path,"r"); if(!f) return -1;
	char line[2048];
	while(fgets(line,sizeof(line),f)){
		trim(line); if(!*line||*line=='#') continue;
		char *eq=strchr(line,'='); if(!eq) continue;
		*eq++=0; trim(line); trim(eq);
		size_t len=strlen(eq);
		if(len>=2&&((*eq=='"'&&eq[len-1]=='"')||(*eq=='\''&&eq[len-1]=='\''))){ eq[len-1]=0; eq++; }
		char *dst=NULL; size_t size=0;
		if(!strcmp(line,"BASE_API_URL")) dst=cfg.base_api_url,size=sizeof(cfg.base_api_url);
		else if(!strcmp(line,"BASE_CIAM_URL")) dst=cfg.base_ciam_url,size=sizeof(cfg.base_ciam_url);
		else if(!strcmp(line,"BASIC_AUTH")) dst=cfg.basic_auth,size=sizeof(cfg.basic_auth);
		else if(!strcmp(line,"AX_FP_KEY")) dst=cfg.ax_fp_key,size=sizeof(cfg.ax_fp_key);
		else if(!strcmp(line,"UA")) dst=cfg.ua,size=sizeof(cfg.ua);
		else if(!strcmp(line,"API_KEY")) dst=cfg.api_key,size=sizeof(cfg.api_key);
		else if(!strcmp(line,"ENCRYPTED_FIELD_KEY")) dst=cfg.encrypted_field_key,size=sizeof(cfg.encrypted_field_key);
		else if(!strcmp(line,"XDATA_KEY")) dst=cfg.xdata_key,size=sizeof(cfg.xdata_key);
		else if(!strcmp(line,"AX_API_SIG_KEY")) dst=cfg.ax_api_sig_key,size=sizeof(cfg.ax_api_sig_key);
		else if(!strcmp(line,"X_API_BASE_SECRET")) dst=cfg.x_api_base_secret,size=sizeof(cfg.x_api_base_secret);
		else if(!strcmp(line,"DECOY_PREPAID_FAMILY_CODE")) dst=cfg.decoy_prepaid_family_code,size=sizeof(cfg.decoy_prepaid_family_code);
		else if(!strcmp(line,"DECOY_PREPAID_PACKAGE_NUMBER")) dst=cfg.decoy_prepaid_package_number,size=sizeof(cfg.decoy_prepaid_package_number);
		else if(!strcmp(line,"DECOY_PRIORITAS_FAMILY_CODE")) dst=cfg.decoy_prioritas_family_code,size=sizeof(cfg.decoy_prioritas_family_code);
		else if(!strcmp(line,"DECOY_PRIORITAS_PACKAGE_NUMBER")) dst=cfg.decoy_prioritas_package_number,size=sizeof(cfg.decoy_prioritas_package_number);
		else if(!strcmp(line,"DECOY_PRIOHYBRID_FAMILY_CODE")) dst=cfg.decoy_priohybrid_family_code,size=sizeof(cfg.decoy_priohybrid_family_code);
		else if(!strcmp(line,"DECOY_PRIOHYBRID_PACKAGE_NUMBER")) dst=cfg.decoy_priohybrid_package_number,size=sizeof(cfg.decoy_priohybrid_package_number);
		if(dst) snprintf(dst,size,"%s",eq);
	}
	fclose(f);
	return 0;
}
static const char *config_missing(void){ const char *names[]={"BASE_API_URL","BASE_CIAM_URL","BASIC_AUTH","AX_FP_KEY","UA","API_KEY","XDATA_KEY","AX_API_SIG_KEY","X_API_BASE_SECRET"}; const char *vals[]={cfg.base_api_url,cfg.base_ciam_url,cfg.basic_auth,cfg.ax_fp_key,cfg.ua,cfg.api_key,cfg.xdata_key,cfg.ax_api_sig_key,cfg.x_api_base_secret}; for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) if(!vals[i][0]) return names[i]; return NULL; }
static void load_config(void) { const char *home=getenv("BITSXL_HOME"),*user_home=getenv("HOME"); if(home&&*home) snprintf(cfg.home,sizeof(cfg.home),"%s",home); else if(user_home&&*user_home) snprintf(cfg.home,sizeof(cfg.home),"%s/.bitsxl",user_home); else snprintf(cfg.home,sizeof(cfg.home),"/root/.bitsxl"); const char *env=getenv("BITSXL_ENV"); if(env){ if(!load_env_file(env)) snprintf(cfg.env_path,sizeof(cfg.env_path),"%s",env); } else if(!load_env_file("/etc/bitsxl/.env")) snprintf(cfg.env_path,sizeof(cfg.env_path),"/etc/bitsxl/.env"); else if(!load_env_file("/etc/bitsxl.env")) snprintf(cfg.env_path,sizeof(cfg.env_path),"/etc/bitsxl.env"); else if(!load_env_file(".env")) snprintf(cfg.env_path,sizeof(cfg.env_path),".env"); ensure_dir(cfg.home); }
static void require_config(void){ const char *missing=config_missing(); if(missing) die("missing env %s in /etc/bitsxl/.env or /etc/bitsxl.env",missing); }
static void account_free(Account *a){ free(a->number); free(a->subscriber_id); free(a->subscription_type); free(a->refresh_token); memset(a,0,sizeof(*a)); }
static void accounts_free(Accounts *a) { for(size_t i=0;i<a->len;i++) account_free(&a->items[i]); for(size_t i=0;i<a->deleted_len;i++) free(a->deleted[i]); free(a->deleted); free(a->items); memset(a,0,sizeof(*a)); }
static Accounts accounts_load(void) { Accounts a={0}; char path[PATH_MAX]; path_join(path,sizeof(path),cfg.home,"accounts.tsv"); FILE *f=fopen(path,"r"); if(!f) return a; char line[8192]; while(fgets(line,sizeof(line),f)){ trim(line); if(!*line) continue; char *cursor=line,*p1=strsep(&cursor,"\t"),*p2=strsep(&cursor,"\t"),*p3=strsep(&cursor,"\t"),*p4=strsep(&cursor,"\t"); if(!p1||!*p1||!p4||!*p4) continue; a.items=realloc(a.items,sizeof(Account)*(a.len+1)); if(!a.items) die("oom"); a.items[a.len++]=(Account){xstrdup(p1),xstrdup(p2?p2:""),xstrdup(p3?p3:""),xstrdup(p4),0,0}; } fclose(f); a.loaded_len=a.len; return a; }
static int accounts_lock(void){ char path[PATH_MAX]; path_join(path,sizeof(path),cfg.home,"accounts.tsv.lock"); int fd=open(path,O_CREAT|O_RDWR,0600); if(fd>=0){ fchmod(fd,0600); flock(fd,LOCK_EX); } return fd; }
static void accounts_unlock(int fd){ if(fd>=0){ flock(fd,LOCK_UN); close(fd); } }
static void accounts_mark_deleted(Accounts *a,const char *number){ for(size_t i=0;i<a->deleted_len;i++) if(!strcmp(a->deleted[i],number)) return; if(a->deleted_len==a->deleted_cap){ a->deleted_cap=a->deleted_cap?a->deleted_cap*2:4; a->deleted=realloc(a->deleted,a->deleted_cap*sizeof(char *)); if(!a->deleted) die("oom"); } a->deleted[a->deleted_len++]=xstrdup(number); }
static int accounts_remove(Accounts *a,const char *number){ for(size_t i=0;i<a->len;i++) if(!strcmp(a->items[i].number,number)){ account_free(&a->items[i]); if(i+1<a->len) memmove(&a->items[i],&a->items[i+1],(a->len-i-1)*sizeof(Account)); a->len--; return 1; } return 0; }
static void account_copy(Account *dst,const Account *src){ account_free(dst); dst->number=xstrdup(src->number); dst->subscriber_id=xstrdup(src->subscriber_id); dst->subscription_type=xstrdup(src->subscription_type); dst->refresh_token=xstrdup(src->refresh_token); dst->dirty=1; dst->is_new=src->is_new; }
static void accounts_merge_record(Accounts *dst,const Account *src){ Account *item=accounts_find(dst,src->number); if(!item){ dst->items=realloc(dst->items,sizeof(Account)*(dst->len+1)); if(!dst->items) die("oom"); item=&dst->items[dst->len++]; memset(item,0,sizeof(*item)); } account_copy(item,src); }
static void accounts_write_locked(Accounts *a){ char path[PATH_MAX],tmp[PATH_MAX+32]; path_join(path,sizeof(path),cfg.home,"accounts.tsv"); int tmp_len=snprintf(tmp,sizeof(tmp),"%s.%ld.XXXXXX",path,(long)getpid()); if(tmp_len<0||(size_t)tmp_len>=sizeof(tmp)) die("temporary account path too long"); int fd=mkstemp(tmp); if(fd<0) die("write %s: %s",path,strerror(errno)); FILE *f=fdopen(fd,"w"); if(!f){ close(fd); unlink(tmp); die("fdopen %s: %s",tmp,strerror(errno)); } chmod(tmp,0600); int failed=0; for(size_t i=0;i<a->len;i++) if(fprintf(f,"%s\t%s\t%s\t%s\n",a->items[i].number,a->items[i].subscriber_id,a->items[i].subscription_type,a->items[i].refresh_token)<0){ failed=1; break; } if(fflush(f)||fsync(fd)) failed=1; if(fclose(f)) failed=1; if(failed){ unlink(tmp); die("write %s: %s",path,strerror(errno)); } if(rename(tmp,path)){ unlink(tmp); die("rename %s: %s",path,strerror(errno)); } }
static void accounts_save(Accounts *a) { int changed=a->deleted_len>0; for(size_t i=0;!changed&&i<a->len;i++) changed=a->items[i].dirty; if(!changed) return; int lock_fd=accounts_lock(); if(lock_fd<0) die("lock accounts: %s",strerror(errno)); Accounts disk=accounts_load(); for(size_t i=0;i<a->deleted_len;i++) accounts_remove(&disk,a->deleted[i]); for(size_t i=0;i<a->len;i++) if(a->items[i].dirty&&(a->items[i].is_new||accounts_find(&disk,a->items[i].number))) accounts_merge_record(&disk,&a->items[i]); accounts_write_locked(&disk); accounts_free(&disk); accounts_unlock(lock_fd); for(size_t i=0;i<a->deleted_len;i++) free(a->deleted[i]); a->deleted_len=0; a->loaded_len=a->len; for(size_t i=0;i<a->len;i++){ a->items[i].dirty=0; a->items[i].is_new=0; } }
static void account_sync(Accounts *accounts,Account *account,Tokens *tokens){ if(account_update_profile(account,tokens)) accounts_save(accounts); }
static Account *accounts_find(Accounts *a,const char *number){ for(size_t i=0;i<a->len;i++) if(!strcmp(a->items[i].number,number)) return &a->items[i]; return NULL; }
static void accounts_put(Accounts *a,const char *number,const char *sub,const char *typ,const char *rt){ Account *e=accounts_find(a,number); if(!e){ a->items=realloc(a->items,sizeof(Account)*(a->len+1)); if(!a->items) die("oom"); e=&a->items[a->len++]; memset(e,0,sizeof(*e)); e->number=xstrdup(number); e->subscriber_id=xstrdup(""); e->subscription_type=xstrdup(""); e->refresh_token=xstrdup(""); e->dirty=1; e->is_new=1; } int type_changed=typ&&*typ&&(!e->subscription_type||strcmp(e->subscription_type,typ)); if(account_replace_if_changed(&e->subscriber_id,sub)|account_replace_if_changed(&e->subscription_type,typ)|account_replace_if_changed(&e->refresh_token,rt)) e->dirty=1; if(type_changed) response_cache_invalidate_account(e); }
static char *active_get(void){ char path[PATH_MAX]; path_join(path,sizeof(path),cfg.home,"active"); char *s=read_file(path); if(s) trim(s); return s; }
static void active_set(const char *number){ char path[PATH_MAX]; path_join(path,sizeof(path),cfg.home,"active"); write_file(path,number); }

static const char *C0="\033[0m",*CB="\033[1;37m",*CC="\033[1;36m",*CL="\033[36m",*CG="\033[32m",*CY="\033[33m",*CR="\033[31m",*CD="\033[90m",*CW="\033[37m";
static int msisdn_ok(const char *s);
static char *login_msisdn(const char *input);
static int otp_ok(const char *s);
static void money_id(long long v,char *out,size_t sz){ char tmp[48]; snprintf(tmp,sizeof(tmp),"%lld",v); size_t n=strlen(tmp), dots=(n?n-1:0)/3, len=n+dots; if(len+1>sz){ snprintf(out,sz,"%lld",v); return; } out[len]=0; int c=0; for(long i=(long)n-1,j=(long)len-1;i>=0;i--){ out[j--]=tmp[i]; if(++c==3&&i>0){ out[j--]='.'; c=0; } } }
static void date_str(long long ts,char *out,size_t sz){ if(ts<=0){ snprintf(out,sz,"N/A"); return; } time_t t=(time_t)ts; struct tm tm; localtime_r(&t,&tm); strftime(out,sz,"%Y-%m-%d",&tm); }
static void quota_value_str(const char *type,long long v,char *out,size_t sz){ if(!strcmp(type,"DATA")){ double d=(double)v; if(v>=1073741824LL) snprintf(out,sz,"%.2f GB",d/1073741824.0); else if(v>=1048576LL) snprintf(out,sz,"%.2f MB",d/1048576.0); else if(v>=1024) snprintf(out,sz,"%.2f KB",d/1024.0); else snprintf(out,sz,"%lld B",v); } else if(!strcmp(type,"VOICE")) snprintf(out,sz,"%.2f menit",v/60.0); else if(!strcmp(type,"TEXT")) snprintf(out,sz,"%lld SMS",v); else snprintf(out,sz,"%lld",v); }
static const char *pct_color(int pct){ return pct>=60?CG:(pct>=25?CY:CR); }
static void print_bar(int pct){ int fill=(pct+5)/10; if(fill<0) fill=0; if(fill>10) fill=10; const char *col=pct_color(pct); printf("%s",col); for(int i=0;i<fill;i++) printf("█"); printf("%s",CD); for(int i=fill;i<10;i++) printf("░"); printf("%s",C0); }
static void ql_add(QuotaList *l,char *name,char *group,char *code,char *domain,char *subtype){ if(!code){ free(name); free(group); free(domain); free(subtype); return; } if(l->n==l->cap){ l->cap=l->cap?l->cap*2:8; l->v=realloc(l->v,l->cap*sizeof(QuotaPkg)); if(!l->v) die("oom"); } l->v[l->n++]=(QuotaPkg){name?name:xstrdup("(no name)"),group?group:xstrdup(""),code,domain?domain:xstrdup(""),subtype?subtype:xstrdup("")}; }
static void ql_free(QuotaList *l){ for(size_t i=0;i<l->n;i++){ free(l->v[i].name); free(l->v[i].group); free(l->v[i].code); free(l->v[i].domain); free(l->v[i].subtype); } free(l->v); memset(l,0,sizeof(*l)); }
static int show_quota_pretty(const char *json,const char *number,const char *subtype,QuotaList *ql){ const char *start,*end; if(json_array_span(json,"quotas",&start,&end)){ printf("%sGagal membaca data kuota.%s\n%s\n",CR,C0,json); return 0; } printf("%s=======================================================%s\n",CC,C0); printf("%s             MyXL Quota - %s%s\n",CB,number,C0); printf("%s                    %s%s\n",CW,subtype?subtype:"",C0); printf("%s=======================================================%s\n",CC,C0); const char *p=start; int idx=1; while(p<end){ char *obj=next_object(&p,end); if(!obj) break; char *name=json_get_string(obj,"name"),*group=json_get_string(obj,"group_name"),*code=json_get_string(obj,"quota_code"),*domain=json_get_string(obj,"product_domain"),*pst=json_get_string(obj,"product_subscription_type"); ql_add(ql,xstrdup(name?name:"(no name)"),xstrdup(group?group:""),xstrdup(code?code:""),xstrdup(domain?domain:""),xstrdup(pst?pst:"")); printf("\n%s[%s%d%s]%s %s%s%s\n",CB,CC,idx,CB,C0,CC,name?name:"(no name)",C0); if(group&&*group) printf("    %s%s%s\n",CW,group,C0); printf("    %s---------------------------------------------------%s\n",CL,C0); const char *bs,*be; if(!json_array_span(obj,"benefits",&bs,&be)){ const char *bp=bs; int first=1; while(bp<be){ char *b=next_object(&bp,be); if(!b) break; char *bn=json_get_string(b,"name"),*dt=json_get_string(b,"data_type"); long long rem=json_get_ll(b,"remaining",0),tot=json_get_ll(b,"total",0); int pct=tot>0?(int)((rem*100+tot/2)/tot):0; if(pct<0)pct=0; if(pct>100)pct=100; char rs[32],ts[32]; quota_value_str(dt?dt:"",rem,rs,sizeof(rs)); quota_value_str(dt?dt:"",tot,ts,sizeof(ts)); if(!first) putchar('\n'); first=0; printf("    %s%s%s\n    ",CB,bn?bn:"benefit",C0); print_bar(pct); printf("  %s%3d%%%s   %s%s%s / %s%s%s\n",pct_color(pct),pct,C0,pct_color(pct),rs,C0,CW,ts,C0); free(bn); free(dt); free(b); } } free(name); free(group); free(code); free(domain); free(pst); free(obj); idx++; } printf("\n%s=======================================================%s\n",CC,C0); printf("%sTotal Paket:%s %s%zu%s\n",CW,C0,CB,ql->n,C0); return (int)ql->n; }
static void cmd_login(const char *input){ char *number=login_msisdn(input); if(!number) die("number must use 08 or 628 format and contain 8-14 normalized digits"); char *resp=request_otp(number); if(strstr(resp,"subscriber_id")) printf("OTP requested for %s. Next: bitsxl otp %s <code>\n",number,number); else printf("OTP response: %s\n",resp); free(resp); free(number); }
static void cmd_otp(const char *input,const char *code){ char *number=login_msisdn(input); if(!number) die("number must use 08 or 628 format and contain 8-14 normalized digits"); if(!otp_ok(code)){ free(number); die("otp must be 6 digits"); } char *resp=submit_otp("SMS",number,code); Tokens t={0}; if(parse_tokens(resp,&t)){ free(number); die("login failed: %s",resp); } free(resp); char *profile=get_profile(&t),*sub=json_get_string(profile,"subscriber_id"),*typ=json_get_string(profile,"subscription_type"); Accounts a=accounts_load(); accounts_put(&a,number,sub?sub:"",typ?typ:"",t.refresh_token); Account *saved=accounts_find(&a,number); if(saved) token_cache_save(saved,&t); accounts_save(&a); active_set(number); printf("saved %s [%s]\n",number,typ?typ:"unknown"); free(sub); free(typ); free(profile); accounts_free(&a); tokens_free(&t); free(number); }
static void cmd_accounts(void){ Accounts a=accounts_load(); char *act=active_get(); if(!a.len) puts("no accounts"); for(size_t i=0;i<a.len;i++) printf("%c %s [%s]\n",(act&&!strcmp(act,a.items[i].number))?'*':' ',a.items[i].number,a.items[i].subscription_type); free(act); accounts_free(&a); }
static void cmd_use(const char *number){ Accounts a=accounts_load(); if(!accounts_find(&a,number)) die("account not found: %s",number); active_set(number); printf("active: %s\n",number); accounts_free(&a); }
static void delete_account_number(const char *number){ Accounts a=accounts_load(); Account tmp={.number=(char *)number}; token_cache_delete(&tmp); response_cache_invalidate_account(&tmp); size_t w=0; for(size_t i=0;i<a.len;i++){ if(!strcmp(a.items[i].number,number)){ accounts_mark_deleted(&a,number); free(a.items[i].number); free(a.items[i].subscriber_id); free(a.items[i].subscription_type); free(a.items[i].refresh_token); continue; } a.items[w++]=a.items[i]; } a.len=w; accounts_save(&a); char *act=active_get(); if(act&&!strcmp(act,number)) active_set(""); free(act); accounts_free(&a); }
static void cmd_del(const char *number){ delete_account_number(number); printf("deleted: %s\n",number); }
static void cmd_logout(void){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(acc){ token_cache_delete(acc); response_cache_invalidate_account(acc); } active_set(""); printf("session reset%s%s\n",act&&*act?": ":"",act&&*act?act:""); free(act); accounts_free(&a); }
static void cmd_quota(const char *number_arg){ Accounts a=accounts_load(); char *act=NULL; const char *number=number_arg; if(!number){ act=active_get(); number=act; } if(!number||!*number) die("no active account. run: bitsxl use <number>"); Account *acc=accounts_find(&a,number); if(!acc) die("account not found: %s",number); Tokens t={0}; if(refresh_account(acc,&t)) exit(1); account_sync(&a,acc,&t); char *q=get_quota(&t); QuotaList ql={0}; show_quota_pretty(q,acc->number,acc->subscription_type,&ql); ql_free(&ql); free(q); tokens_free(&t); accounts_free(&a); free(act); }
static void cmd_env(void){
	int number=0;
	printf("home: %s\n",cfg.home); printf("env: %s\n",cfg.env_path[0]?cfg.env_path:"(none)");
	printf("BASE_API_URL: %s\n",cfg.base_api_url[0]?"ok":"missing"); printf("BASE_CIAM_URL: %s\n",cfg.base_ciam_url[0]?"ok":"missing");
	printf("BASIC_AUTH: %s\n",cfg.basic_auth[0]?"ok":"missing"); printf("AX_FP_KEY: %s\n",cfg.ax_fp_key[0]?"ok":"missing");
	printf("UA: %s\n",cfg.ua[0]?"ok":"missing"); printf("API_KEY: %s\n",cfg.api_key[0]?"ok":"missing");
	printf("ENCRYPTED_FIELD_KEY: %s\n",cfg.encrypted_field_key[0]?"ok":"missing"); printf("XDATA_KEY: %s\n",cfg.xdata_key[0]?"ok":"missing");
	printf("AX_API_SIG_KEY: %s\n",cfg.ax_api_sig_key[0]?"ok":"missing"); printf("X_API_BASE_SECRET: %s\n",cfg.x_api_base_secret[0]?"ok":"missing");
	printf("DECOY_PREPAID_FAMILY_CODE: %s\n",cfg.decoy_prepaid_family_code[0]?"set":"default");
	printf("DECOY_PREPAID_PACKAGE_NUMBER: %s\n",!decoy_package_number_parse(cfg.decoy_prepaid_package_number,&number)?"set":(cfg.decoy_prepaid_package_number[0]?"invalid":"default"));
	printf("DECOY_PRIORITAS_FAMILY_CODE: %s\n",cfg.decoy_prioritas_family_code[0]?"set":"default");
	printf("DECOY_PRIORITAS_PACKAGE_NUMBER: %s\n",!decoy_package_number_parse(cfg.decoy_prioritas_package_number,&number)?"set":(cfg.decoy_prioritas_package_number[0]?"invalid":"default"));
	printf("DECOY_PRIOHYBRID_FAMILY_CODE: %s\n",cfg.decoy_priohybrid_family_code[0]?"set":"fallback PRIORITAS");
	printf("DECOY_PRIOHYBRID_PACKAGE_NUMBER: %s\n",!decoy_package_number_parse(cfg.decoy_priohybrid_package_number,&number)?"set":(cfg.decoy_priohybrid_package_number[0]?"invalid":"fallback PRIORITAS"));
	printf("libmbedcrypto: linked\n");
}
static void json_str(const char *s){ char *e=json_escape(s?s:""); printf("\"%s\"",e); free(e); }
static int json_require_config(void){ const char *missing=config_missing(); if(!missing) return 0; printf("{\"ok\":false,\"error\":\"missing env\",\"missing\":"); json_str(missing); printf("}\n"); return -1; }
static int jsonish(const char *s){ return json_document_ok(s); }
static void json_val(const char *s){ if(jsonish(s)) fputs(s,stdout); else json_str(s?s:""); }
static void json_accounts_fields(Accounts *a,const char *act){ printf("\"active\":"); json_str(act?act:""); printf(",\"accounts\":["); for(size_t i=0;i<a->len;i++){ if(i) putchar(','); printf("{\"number\":"); json_str(a->items[i].number); printf(",\"subscription_type\":"); json_str(a->items[i].subscription_type); printf(",\"active\":%s}",(act&&strcmp(act,a->items[i].number)==0)?"true":"false"); } putchar(']'); }
static void cmd_json_env(void){
	int prepaid_number=0,prioritas_number=0,hybrid_number=0;
	int prepaid_valid=!decoy_package_number_parse(cfg.decoy_prepaid_package_number,&prepaid_number);
	int prioritas_valid=!decoy_package_number_parse(cfg.decoy_prioritas_package_number,&prioritas_number);
	int hybrid_valid=!decoy_package_number_parse(cfg.decoy_priohybrid_package_number,&hybrid_number);
	printf("{\"ok\":true,\"home\":"); json_str(cfg.home); printf(",\"env_path\":"); json_str(cfg.env_path); printf(",\"env\":{");
	printf("\"BASE_API_URL\":%s,\"BASE_CIAM_URL\":%s,\"BASIC_AUTH\":%s,\"AX_FP_KEY\":%s,\"UA\":%s,\"API_KEY\":%s,\"ENCRYPTED_FIELD_KEY\":%s,\"XDATA_KEY\":%s,\"AX_API_SIG_KEY\":%s,\"X_API_BASE_SECRET\":%s,\"DECOY_PREPAID_FAMILY_CODE\":%s,\"DECOY_PREPAID_PACKAGE_NUMBER\":%s,\"DECOY_PRIORITAS_FAMILY_CODE\":%s,\"DECOY_PRIORITAS_PACKAGE_NUMBER\":%s,\"DECOY_PRIOHYBRID_FAMILY_CODE\":%s,\"DECOY_PRIOHYBRID_PACKAGE_NUMBER\":%s",
		cfg.base_api_url[0]?"true":"false",cfg.base_ciam_url[0]?"true":"false",cfg.basic_auth[0]?"true":"false",cfg.ax_fp_key[0]?"true":"false",cfg.ua[0]?"true":"false",cfg.api_key[0]?"true":"false",cfg.encrypted_field_key[0]?"true":"false",cfg.xdata_key[0]?"true":"false",cfg.ax_api_sig_key[0]?"true":"false",cfg.x_api_base_secret[0]?"true":"false",cfg.decoy_prepaid_family_code[0]?"true":"false",prepaid_valid?"true":"false",cfg.decoy_prioritas_family_code[0]?"true":"false",prioritas_valid?"true":"false",cfg.decoy_priohybrid_family_code[0]?"true":"false",hybrid_valid?"true":"false");
	printf("},\"libmbedcrypto\":true}\n");
}
static void cmd_json_accounts(void){ Accounts a=accounts_load(); char *act=active_get(); printf("{\"ok\":true,"); json_accounts_fields(&a,act); printf("}\n"); free(act); accounts_free(&a); }
static void cmd_json_use(const char *number){ Accounts a=accounts_load(); Account *acc=accounts_find(&a,number); if(!acc){ printf("{\"ok\":false,\"error\":\"account not found\"}\n"); accounts_free(&a); return; } active_set(number); printf("{\"ok\":true,\"active\":"); json_str(number); printf("}\n"); accounts_free(&a); }
static void cmd_json_del(const char *number){ Accounts a=accounts_load(); if(!accounts_find(&a,number)){ printf("{\"ok\":false,\"error\":\"account not found\"}\n"); accounts_free(&a); return; } accounts_free(&a); delete_account_number(number); printf("{\"ok\":true}\n"); }
static void cmd_json_logout(void){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(acc){ token_cache_delete(acc); response_cache_invalidate_account(acc); } active_set(""); printf("{\"ok\":true,\"previous_active\":"); json_str(act?act:""); printf("}\n"); free(act); accounts_free(&a); }
static void cmd_json_login(const char *input){ char *number=login_msisdn(input); if(!number){ printf("{\"ok\":false,\"error\":\"number must use 08 or 628 format\"}\n"); return; } char *resp=request_otp(number); int ok=strstr(resp,"subscriber_id")!=NULL; printf("{\"ok\":%s,\"response\":",ok?"true":"false"); json_val(resp); printf("}\n"); free(resp); free(number); }
static void cmd_json_otp(const char *input,const char *code){ char *number=login_msisdn(input); if(!number){ printf("{\"ok\":false,\"error\":\"number must use 08 or 628 format\"}\n"); return; } if(!otp_ok(code)){ printf("{\"ok\":false,\"error\":\"otp must be 6 digits\"}\n"); free(number); return; } char *resp=submit_otp("SMS",number,code); Tokens t={0}; if(parse_tokens(resp,&t)){ printf("{\"ok\":false,\"response\":"); json_val(resp); printf("}\n"); free(resp); free(number); return; } free(resp); char *profile=get_profile(&t),*sub=json_get_string(profile,"subscriber_id"),*typ=json_get_string(profile,"subscription_type"); Accounts a=accounts_load(); accounts_put(&a,number,sub?sub:"",typ?typ:"",t.refresh_token); Account *saved=accounts_find(&a,number); if(saved) token_cache_save(saved,&t); accounts_save(&a); active_set(number); printf("{\"ok\":true,\"number\":"); json_str(number); printf(",\"subscription_type\":"); json_str(typ?typ:"unknown"); printf("}\n"); free(sub); free(typ); free(profile); accounts_free(&a); tokens_free(&t); free(number); }
static void cmd_json_unsub(const char *quota_code,const char *subtype,const char *domain){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } if(!quota_code||!*quota_code){ printf("{\"ok\":false,\"error\":\"missing quota code\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *res=unsubscribe_quota_once(&a,acc,&t,quota_code,subtype?subtype:"",domain?domain:""); int ok=!api_auth_failed(res)&&api_success_response(res); printf("{\"ok\":%s,\"response\":",ok?"true":"false"); json_val(res); printf("}\n"); free(res); tokens_free(&t); free(act); accounts_free(&a); }
static int arg_true(const char *s){ return s&&(*s=='1'||*s=='y'||*s=='Y'||!strcmp(s,"true")||!strcmp(s,"TRUE")||!strcmp(s,"yes")||!strcmp(s,"YES")); }
static int family_code_ok(const char *s){ if(!s) return 0; size_t n=strlen(s); if(n<1||n>128) return 0; for(size_t i=0;i<n;i++) if(iscntrl((unsigned char)s[i])||isspace((unsigned char)s[i])) return 0; return 1; }
static int purchase_code_ok(const char *s){ if(!s) return 0; size_t n=strlen(s); if(n<1||n>255) return 0; for(size_t i=0;i<n;i++) if(iscntrl((unsigned char)s[i])||isspace((unsigned char)s[i])) return 0; return 1; }
static int notification_id_ok(const char *s){ if(!s) return 0; size_t n=strlen(s); if(n<1||n>256) return 0; for(size_t i=0;i<n;i++) if(iscntrl((unsigned char)s[i])||isspace((unsigned char)s[i])) return 0; return 1; }
static int api_auth_failed(const char *resp){ return resp&&(strstr(resp,"REQUEST_MISSING_BEARER")||strstr(resp,"MISSING_BEARER")||strstr(resp,"Invalid token")||strstr(resp,"invalid_token")||strstr(resp,"Token expired")||strstr(resp,"token expired")||strstr(resp,"Unauthorized")||strstr(resp,"auth_error=true")||strstr(resp,"code=132")||strstr(resp,"\"code\":\"132\"")||strstr(resp,"\"code\":132")||strstr(resp,"\"status\":401")||strstr(resp,"\"status\":\"401\"")); }
static int api_success_response(const char *resp){ char *status=json_get_string(resp,"status"),*code=json_get_string(resp,"code"); int ok=(status&&!strcmp(status,"SUCCESS"))||(code&&!strcmp(code,"000")); free(status); free(code); return ok; }
#ifdef BITSXL_TEST
static int (*bitsxl_test_api_retry_auth_hook)(Account *,Accounts *,Tokens *);
#endif
static int api_retry_auth(Account *acc,Accounts *a,Tokens *t){
#ifdef BITSXL_TEST
	if(bitsxl_test_api_retry_auth_hook) return bitsxl_test_api_retry_auth_hook(acc,a,t);
#endif
	token_cache_delete(acc); tokens_free(t); memset(t,0,sizeof(*t));
	if(refresh_account(acc,t)) return -1;
	account_sync(a,acc,t);
	return 0;
}
static char *get_quota_retry(Accounts *a,Account *acc,Tokens *t){ char *r=response_cache_load(acc,"quota",DASHBOARD_CACHE_TTL_SEC); if(r) return r; char *generation=response_cache_generation(acc); r=get_quota(t); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_quota(t); } response_cache_save_if_current(acc,"quota",r,generation); free(generation); return r; }
static char *get_balance_retry(Accounts *a,Account *acc,Tokens *t){ char *r=response_cache_load(acc,"balance",DASHBOARD_CACHE_TTL_SEC); if(r) return r; char *generation=response_cache_generation(acc); r=get_balance_api(t); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_balance_api(t); } response_cache_save_if_current(acc,"balance",r,generation); free(generation); return r; }
static char *get_tiering_info_retry(Accounts *a,Account *acc,Tokens *t){ char *r=response_cache_load(acc,"tiering",TIERING_CACHE_TTL_SEC); if(r) return r; char *generation=response_cache_generation(acc); r=get_tiering_info_api(t); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_tiering_info_api(t); } response_cache_save_if_current(acc,"tiering",r,generation); free(generation); return r; }
static char *get_transaction_history_retry(Accounts *a,Account *acc,Tokens *t){ char *r=get_transaction_history_api(t); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_transaction_history_api(t); } return r; }
static char *get_pending_transactions_retry(Accounts *a,Account *acc,Tokens *t){ char *r=get_pending_transactions_api(t); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_pending_transactions_api(t); } return r; }
static char *get_transaction_status_retry(Accounts *a,Account *acc,Tokens *t,const char *tx,const char *st){ char *r=get_transaction_status_api(t,tx,st); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_transaction_status_api(t,tx,st); } return r; }
static char *get_notifications_retry(Accounts *a,Account *acc,Tokens *t){ char *r=get_notifications_api(t); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_notifications_api(t); } return r; }
static char *get_notification_detail_once(Account *acc,Tokens *t,const char *notification_id){ char *r=get_notification_detail_api(t,notification_id); if(api_auth_failed(r)){ token_cache_delete(acc); tokens_free(t); } return r; }
static char *unsubscribe_quota_once(Accounts *a,Account *acc,Tokens *t,const char *quota_code,const char *subtype,const char *domain){ (void)a; mutation_prepare(acc); char *r=unsubscribe_quota(t,quota_code,subtype,domain); mutation_result(acc,t,r); return r; }
static char *get_store_segments_retry(Accounts *a,Account *acc,Tokens *t,int ent){ char key[24]; snprintf(key,sizeof(key),"segments-%d",ent?1:0); char *r=response_cache_load(acc,key,STORE_CACHE_TTL_SEC); if(r) return r; char *generation=response_cache_generation(acc); r=get_store_segments(t,ent); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_store_segments(t,ent); } response_cache_save_if_current(acc,key,r,generation); free(generation); return r; }
static char *get_store_packages_retry(Accounts *a,Account *acc,Tokens *t,int ent){ char key[24]; snprintf(key,sizeof(key),"shop-%d",ent?1:0); char *r=response_cache_load(acc,key,STORE_CACHE_TTL_SEC); if(r) return r; char *generation=response_cache_generation(acc); r=get_store_packages(t,acc->subscription_type,ent); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_store_packages(t,acc->subscription_type,ent); } response_cache_save_if_current(acc,key,r,generation); free(generation); return r; }
static char *get_store_family_list_retry(Accounts *a,Account *acc,Tokens *t,int ent){ char key[24]; snprintf(key,sizeof(key),"families-%d",ent?1:0); char *r=response_cache_load(acc,key,STORE_CACHE_TTL_SEC); if(r) return r; char *generation=response_cache_generation(acc); r=get_store_family_list(t,acc->subscription_type,ent); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_store_family_list(t,acc->subscription_type,ent); } response_cache_save_if_current(acc,key,r,generation); free(generation); return r; }
static char *get_store_family_retry(Accounts *a,Account *acc,Tokens *t,const char *code,const char *ent){ char *r=get_store_family_packages(t,code,ent); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_store_family_packages(t,code,ent); } return r; }
static char *get_redeemables_retry(Accounts *a,Account *acc,Tokens *t,int ent){ char *r=get_redeemables_api(t,ent); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_redeemables_api(t,ent); } return r; }
static char *get_package_detail_retry(Accounts *a,Account *acc,Tokens *t,const char *code){ char *r=get_package_detail_api(t,code); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_package_detail_api(t,code); } return r; }
static char *get_package_detail_retry_ex(Accounts *a,Account *acc,Tokens *t,const char *code,const char *family,const char *variant,int ent){ char *r=get_package_detail_api_ex(t,code,family,variant,ent,"NONE"); if(api_auth_failed(r)&&!api_retry_auth(acc,a,t)){ free(r); r=get_package_detail_api_ex(t,code,family,variant,ent,"NONE"); } return r; }
static int package_detail_valid(const char *detail){ char *status=json_get_string(detail,"status"),*data=json_object_dup(detail,"data"),*option=data?json_object_dup(data,"package_option"):NULL; int ok=status&&!strcmp(status,"SUCCESS")&&option; free(status); free(data); free(option); return ok; }
static int redeemables_valid(const char *resp){ const char *s,*e; return api_success_response(resp)&&!json_array_span(resp,"categories",&s,&e); }
static int transaction_history_valid(const char *resp){ const char *s,*e,*keys[]={"list","transaction_history","history","transactions"}; return api_success_response(resp)&&!json_array_span_any(resp,keys,4,&s,&e); }
static int pending_payment_present(const char *resp){ const char *s,*e; if(json_array_span(resp,"pending_payment",&s,&e)) return 0; while(s<e&&isspace((unsigned char)*s)) s++; return s<e; }
static int payment_quote_load_retry_track(Accounts *a,Account *acc,Tokens *t,const char *code,PaymentQuote *q,char **err,int *token_refreshed){
	if(token_refreshed) *token_refreshed=0;
	if(!payment_quote_load(t,code,q,err)) return 0;
	if(err&&*err&&api_auth_failed(*err)){
		free(*err); *err=NULL;
		if(!api_retry_auth(acc,a,t)){
			if(token_refreshed) *token_refreshed=1;
			return payment_quote_load(t,code,q,err);
		}
	}
	return -1;
}
static int payment_quote_load_retry(Accounts *a,Account *acc,Tokens *t,const char *code,PaymentQuote *q,char **err){ return payment_quote_load_retry_track(a,acc,t,code,q,err,NULL); }
static DecoyResolveResult decoy_quote_resolve_once(Account *acc,Tokens *t,PaymentQuote *quote,char **option_code,char **err){
	PackageContext context={0};
	DecoyResolveResult result=decoy_package_context_once(t,acc,&context,err);
	if(result){ package_context_free(&context); return result; }
	result=payment_quote_load_context(t,&context,quote,err);
	if(!result&&option_code) *option_code=xstrdup(quote->item_code);
	package_context_free(&context);
	return result;
}
static int decoy_quote_resolve(Accounts *accounts,Account *acc,Tokens *tokens,PaymentQuote *quote,char **option_code,char **err,int *token_refreshed){
	if(err) *err=NULL;
	if(option_code) *option_code=NULL;
	if(token_refreshed) *token_refreshed=0;
	DecoyResolveResult first_result=decoy_quote_resolve_once(acc,tokens,quote,option_code,err);
	if(!first_result) return 0;
	int retry_151=first_result==DECOY_RESOLVE_RETRY_151;
	int retry_auth=first_result==DECOY_RESOLVE_RETRY_AUTH;
	if(!retry_151&&!retry_auth) return -1;
	char *first_error=err?*err:NULL;
	if(err) *err=NULL;
	payment_quote_free(quote);
	if(option_code&&*option_code){ free(*option_code); *option_code=NULL; }
	if(api_retry_auth(acc,accounts,tokens)){
		if(err) *err=xasprintf("resolver decoy gagal memperbarui token akun aktif setelah %s: %s",retry_151?"error 151":"auth error",first_error?first_error:"unknown error");
		free(first_error);
		return -1;
	}
	if(token_refreshed) *token_refreshed=1;
	free(first_error);
	DecoyResolveResult retry_result=decoy_quote_resolve_once(acc,tokens,quote,option_code,err);
	if(!retry_result) return 0;
	if(retry_151&&err&&*err){
		char *retry_error=*err;
		*err=xasprintf("resolver decoy tetap gagal setelah error 151 di-resolve ulang satu kali dengan token terbaru: %s",retry_error);
		free(retry_error);
	}
	return -1;
}
static int payment_quotes_reload_current(Tokens *tokens,PaymentQuote *quotes,int count,long long *total,char **err){
	char *codes[3]={0};
	if(count<1||count>3){ if(err) *err=xstrdup("target quote count invalid"); return -1; }
	for(int i=0;i<count;i++) codes[i]=xstrdup(quotes[i].item_code);
	for(int i=0;i<count;i++) payment_quote_free(&quotes[i]);
	long long sum=0; int loaded=0;
	for(int i=0;i<count;i++){
		if(payment_quote_load(tokens,codes[i],&quotes[i],err)){
			for(int j=0;j<loaded;j++) payment_quote_free(&quotes[j]);
			for(int j=0;j<count;j++) free(codes[j]);
			return -1;
		}
		if(quotes[i].price>LLONG_MAX-sum){
			if(err) *err=xstrdup("target quote total overflow");
			for(int j=0;j<=i;j++) payment_quote_free(&quotes[j]);
			for(int j=0;j<count;j++) free(codes[j]);
			return -1;
		}
		sum+=quotes[i].price; loaded++;
	}
	for(int i=0;i<count;i++) free(codes[i]);
	if(total) *total=sum;
	return 0;
}
static char *payment_pulsa_settle_many_once(Accounts *a,Account *acc,Tokens *t,PaymentQuote *q,int n,long long total){ (void)a; (void)acc; return payment_pulsa_settle_many(acc,t,q,n,total); }
static void json_auth_error(const char *resp){ printf("{\"ok\":false,\"error\":\"auth failed\",\"response\":"); json_val(resp); printf("}\n"); }
static int load_payment_quotes_json(Accounts *a,Account *acc,Tokens *t,int n,char **codes,PaymentQuote *q,int max,long long *total,int *loaded);
static long long payment_amount(long long total,long long custom_amount);
static void print_amount_json(long long total,long long amount);
static int strip_custom_amount_args(int *argc,char **argv,long long *custom_amount);
static int strip_json_confirm_args(int *argc,char **argv);
static int require_json_payment_confirmed(void);
static void cmd_json_store_segments(const char *enterprise_arg){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *segments=get_store_segments_retry(&a,acc,&t,arg_true(enterprise_arg)); if(api_auth_failed(segments)){ json_auth_error(segments); free(segments); tokens_free(&t); free(act); accounts_free(&a); return; } printf("{\"ok\":true,\"number\":"); json_str(acc->number); printf(",\"store_segments\":"); json_val(segments); printf("}\n"); free(segments); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_redeemables(const char *enterprise_arg){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *redeemables=get_redeemables_retry(&a,acc,&t,arg_true(enterprise_arg)); if(api_auth_failed(redeemables)){ json_auth_error(redeemables); free(redeemables); tokens_free(&t); free(act); accounts_free(&a); return; } int ok=redeemables_valid(redeemables); printf("{\"ok\":%s",ok?"true":"false"); if(!ok) printf(",\"error\":\"redeemables failed\""); printf(",\"number\":"); json_str(acc->number); printf(",\"subscription_type\":"); json_str(acc->subscription_type); printf(",\"redeemables\":"); json_val(redeemables); printf("}\n"); free(redeemables); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_shop(const char *enterprise_arg){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *shop=get_store_packages_retry(&a,acc,&t,arg_true(enterprise_arg)); if(api_auth_failed(shop)){ json_auth_error(shop); free(shop); tokens_free(&t); free(act); accounts_free(&a); return; } printf("{\"ok\":true,\"number\":"); json_str(acc->number); printf(",\"subscription_type\":"); json_str(acc->subscription_type); printf(",\"shop\":"); json_val(shop); printf("}\n"); free(shop); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_shop_family_list(const char *enterprise_arg){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *families=get_store_family_list_retry(&a,acc,&t,arg_true(enterprise_arg)); if(api_auth_failed(families)){ json_auth_error(families); free(families); tokens_free(&t); free(act); accounts_free(&a); return; } printf("{\"ok\":true,\"number\":"); json_str(acc->number); printf(",\"subscription_type\":"); json_str(acc->subscription_type); printf(",\"family_list\":"); json_val(families); printf("}\n"); free(families); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_shop_family(const char *family_code,const char *enterprise_arg){ if(!family_code_ok(family_code)){ printf("{\"ok\":false,\"error\":\"invalid family code\"}\n"); return; } Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *shop=get_store_family_retry(&a,acc,&t,family_code,enterprise_arg); if(api_auth_failed(shop)){ json_auth_error(shop); free(shop); tokens_free(&t); free(act); accounts_free(&a); return; } printf("{\"ok\":true,\"number\":"); json_str(acc->number); printf(",\"subscription_type\":"); json_str(acc->subscription_type); printf(",\"family_code\":"); json_str(family_code); printf(",\"shop\":"); json_val(shop); printf("}\n"); free(shop); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_package_detail(const char *option_code,const char *family_code,const char *variant_code,const char *enterprise_arg){ if(!option_code||!*option_code){ printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); return; } Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *detail=get_package_detail_retry_ex(&a,acc,&t,option_code,family_code?family_code:"",variant_code?variant_code:"",arg_true(enterprise_arg)); if(api_auth_failed(detail)){ json_auth_error(detail); free(detail); tokens_free(&t); free(act); accounts_free(&a); return; } if(!package_detail_valid(detail)){ printf("{\"ok\":false,\"error\":\"package detail failed\",\"action_param\":"); json_str(option_code); printf(",\"response\":"); json_val(detail); printf("}\n"); free(detail); tokens_free(&t); free(act); accounts_free(&a); return; } printf("{\"ok\":true,\"action_param\":"); json_str(option_code); printf(",\"detail\":"); json_val(detail); printf("}\n"); free(detail); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_payment_quote(const char *option_code){ if(!option_code||!*option_code){ printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); return; } Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); PaymentQuote q={0}; char *err=NULL; if(payment_quote_load_retry(&a,acc,&t,option_code,&q,&err)){ printf("{\"ok\":false,\"error\":"); json_str(err?err:"quote failed"); printf("}\n"); free(err); tokens_free(&t); free(act); accounts_free(&a); return; } printf("{\"ok\":true,\"action_param\":"); json_str(option_code); printf(",\"quote\":"); payment_quote_print_json(&q); printf("}\n"); payment_quote_free(&q); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_payment_probe(const char *option_code){ if(!option_code||!*option_code){ printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); return; } Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); PaymentQuote q={0}; char *err=NULL; if(payment_quote_load_retry(&a,acc,&t,option_code,&q,&err)){ printf("{\"ok\":false,\"error\":"); json_str(err?err:"quote failed"); printf("}\n"); free(err); tokens_free(&t); free(act); accounts_free(&a); return; } char *methods=payment_methods_api(&t,&q); if(api_auth_failed(methods)&&!api_retry_auth(acc,&a,&t)){ free(methods); methods=payment_methods_api(&t,&q); } if(api_auth_failed(methods)){ json_auth_error(methods); free(methods); payment_quote_free(&q); tokens_free(&t); free(act); accounts_free(&a); return; } int ok=api_success_response(methods); printf("{\"ok\":%s",ok?"true":"false"); if(!ok) printf(",\"error\":\"payment methods failed\""); printf(",\"action_param\":"); json_str(option_code); printf(",\"quote\":"); payment_quote_print_json(&q); printf(",\"payment_methods\":"); json_val(methods); printf("}\n"); free(methods); payment_quote_free(&q); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_payment_pulsa(const char *option_code,const char *confirm_arg,long long custom_amount){ const char *missing=payment_config_missing(); if(missing){ printf("{\"ok\":false,\"error\":\"missing env\",\"missing\":"); json_str(missing); printf("}\n"); return; } if(!option_code||!*option_code){ printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); return; } Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); PaymentQuote q={0}; char *err=NULL; if(payment_quote_load_retry(&a,acc,&t,option_code,&q,&err)){ printf("{\"ok\":false,\"error\":"); json_str(err?err:"quote failed"); printf("}\n"); free(err); tokens_free(&t); free(act); accounts_free(&a); return; } long long amount=payment_amount(q.price,custom_amount); char expect[80]; snprintf(expect,sizeof(expect),"BAYAR-%lld",amount); if(!confirm_arg||strcmp(confirm_arg,expect)){ printf("{\"ok\":false,\"error\":\"confirm required\",\"confirm\":"); json_str(expect); printf(",\"quote\":"); payment_quote_print_json(&q); print_amount_json(q.price,amount); printf("}\n"); payment_quote_free(&q); tokens_free(&t); free(act); accounts_free(&a); return; } char *resp=payment_pulsa_settle_many_once(&a,acc,&t,&q,1,amount),*status=json_get_string(resp,"status"),*msg=json_get_string(resp,"message"); int ok=status&&!strcmp(status,"SUCCESS"); printf("{\"ok\":%s,\"payment\":\"pulsa\",\"message\":",ok?"true":"false"); json_str(msg?msg:(status?status:"unknown")); printf(",\"quote\":"); payment_quote_print_json(&q); print_amount_json(q.price,amount); printf(",\"response\":"); json_val(resp); printf("}\n"); free(status); free(msg); free(resp); payment_quote_free(&q); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_payment_pulsa_cart(int n,char **codes,long long custom_amount){ const char *missing=payment_config_missing(); if(missing){ printf("{\"ok\":false,\"error\":\"missing env\",\"missing\":"); json_str(missing); printf("}\n"); return; } if(require_json_payment_confirmed()) return; if(n<1){ printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); return; } if(n>3){ printf("{\"ok\":false,\"error\":\"cart max 3 packages\"}\n"); return; } Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); PaymentQuote q[3]; memset(q,0,sizeof(q)); long long total=0; int loaded=0; if(load_payment_quotes_json(&a,acc,&t,n,codes,q,3,&total,&loaded)){ tokens_free(&t); free(act); accounts_free(&a); return; } long long amount=payment_amount(total,custom_amount); char *resp=payment_pulsa_settle_many_once(&a,acc,&t,q,n,amount),*status=json_get_string(resp,"status"),*msg=json_get_string(resp,"message"); int ok=status&&!strcmp(status,"SUCCESS"); printf("{\"ok\":%s,\"payment\":\"pulsa\",\"message\":",ok?"true":"false"); json_str(msg?msg:(status?status:"unknown")); printf(",\"quotes\":["); for(int i=0;i<n;i++){ if(i) putchar(','); payment_quote_print_json(&q[i]); } putchar(']'); print_amount_json(total,amount); printf(",\"response\":"); json_val(resp); printf("}\n"); free(status); free(msg); free(resp); for(int i=0;i<n;i++) payment_quote_free(&q[i]); tokens_free(&t); free(act); accounts_free(&a); }
static int json_payment_begin(Accounts *a,char **act,Account **acc,Tokens *t){ *a=accounts_load(); *act=active_get(); *acc=(*act&&**act)?accounts_find(a,*act):NULL; if(!*acc){ printf("{\"ok\":false,\"error\":\"no active account\"}\n"); free(*act); accounts_free(a); return -1; } if(refresh_account(*acc,t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str((*acc)->number); printf("}\n"); free(*act); accounts_free(a); return -1; } account_sync(a,*acc,t); return 0; }
static int load_payment_quotes_json(Accounts *a,Account *acc,Tokens *t,int n,char **codes,PaymentQuote *q,int max,long long *total,int *loaded){
	if(n<1){ printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); return -1; }
	if(n>max){ printf("{\"ok\":false,\"error\":\"cart max %d packages\"}\n",max); return -1; }
	*total=0; *loaded=0;
	for(int i=0;i<n;i++){
		char *err=NULL; int refreshed=0;
		if(payment_quote_load_retry_track(a,acc,t,codes[i],&q[i],&err,&refreshed)){
			printf("{\"ok\":false,\"error\":"); json_str(err?err:"quote failed"); printf(",\"index\":%d,\"code\":",i); json_str(codes[i]); printf("}\n");
			free(err); for(int j=0;j<*loaded;j++) payment_quote_free(&q[j]); return -1;
		}
		if(refreshed&&*loaded>0){
			long long reloaded_total=0;
			if(payment_quotes_reload_current(t,q,*loaded,&reloaded_total,&err)){
				printf("{\"ok\":false,\"error\":"); json_str(err?err:"prior target quote reload failed after token refresh"); printf(",\"index\":%d}\n",i);
				free(err); payment_quote_free(&q[i]); *loaded=0; return -1;
			}
			*total=reloaded_total;
		}
		if(q[i].price>LLONG_MAX-*total){
			printf("{\"ok\":false,\"error\":\"target quote total overflow\",\"index\":%d}\n",i);
			payment_quote_free(&q[i]); for(int j=0;j<*loaded;j++) payment_quote_free(&q[j]); *loaded=0; return -1;
		}
		*total+=q[i].price; (*loaded)++;
	}
	return 0;
}
static void print_quotes_json(PaymentQuote *q,int n){ printf("\"quotes\":["); for(int i=0;i<n;i++){ if(i) putchar(','); payment_quote_print_json(&q[i]); } putchar(']'); }
static int payment_ok_status(const char *status){ return status&&!strcmp(status,"SUCCESS"); }
static long long payment_amount(long long total,long long custom_amount){ return custom_amount>=0?custom_amount:total; }
static void print_amount_json(long long total,long long amount){ printf(",\"total_amount\":%lld",amount); if(amount!=total) printf(",\"custom_price\":%lld",amount); }
static char *payment_pulsa_settle_retry(Account *acc,Tokens *t,PaymentQuote *q,int n,long long *amount,int token_idx,const char *payment_for_override,char **first_out){
	if(first_out) *first_out=NULL;
	const PackageContext *context=payment_cart_context(q,n);
	char *resp=payment_pulsa_settle_many_raw(acc,t,q,n,*amount,token_idx,payment_for_override);
	char *status=json_get_string(resp,"status");
	long long valid_amount=payment_ok_status(status)?-1:payment_bizz_amount(resp);
	free(status);
	if(valid_amount<0) return payment_response_add_context(resp,context);
	if(first_out) *first_out=payment_response_add_context(resp,context); else free(resp);
	*amount=valid_amount;
	resp=payment_pulsa_settle_many_raw(acc,t,q,n,*amount,token_idx,payment_for_override);
	return payment_response_add_context(resp,context);
}
static int parse_amount_arg(const char *s,long long *out){ if(!s||!*s) return -1; char *e=NULL; errno=0; long long v=strtoll(s,&e,10); if(errno||e==s||*e||v<=0) return -1; *out=v; return 0; }
static int parse_bounded_ll(const char *s,long long min,long long max,long long *out){ if(!s||!*s) return -1; char *e=NULL; errno=0; long long v=strtoll(s,&e,10); if(errno||e==s||*e||v<min||v>max) return -1; *out=v; return 0; }
static int parse_bool_strict(const char *s,int *out){
	if(!s) return -1;
	if(!strcmp(s,"1")||!strcmp(s,"y")||!strcmp(s,"Y")||!strcmp(s,"yes")||!strcmp(s,"YES")||!strcmp(s,"true")||!strcmp(s,"TRUE")){ *out=1; return 0; }
	if(!strcmp(s,"0")||!strcmp(s,"n")||!strcmp(s,"N")||!strcmp(s,"no")||!strcmp(s,"NO")||!strcmp(s,"false")||!strcmp(s,"FALSE")){ *out=0; return 0; }
	return -1;
}
static int repeat_token_index_strict(int raw,int item_n,int *out){ long long idx=raw; if(idx<0) idx+=item_n; if(idx<0||idx>=item_n) return -1; *out=(int)idx; return 0; }
static void parse_repeat_purchase_options(int argc,char **argv,int count,RepeatPurchaseOptions *opts){
	memset(opts,0,sizeof(*opts)); opts->count=count;
	int seen_delay=0,seen_decoy=0,seen_token_idx=0;
	for(int i=0;i<argc;i++){
		const char *arg=argv[i],*value=NULL;
		if(!strcmp(arg,"--delay")||!strcmp(arg,"--delay-seconds")){
			if(seen_delay++) die("duplicate repeat option: %s",arg);
			if(++i>=argc) die("missing value for %s",arg);
			value=argv[i]; long long parsed=0;
			if(parse_bounded_ll(value,0,UINT_MAX,&parsed)) die("invalid delay: %s",value);
			opts->delay_seconds=(unsigned int)parsed;
		} else if(starts_with(arg,"--delay=")||starts_with(arg,"--delay-seconds=")){
			if(seen_delay++) die("duplicate repeat option: %s",arg);
			value=strchr(arg,'=')+1; long long parsed=0;
			if(parse_bounded_ll(value,0,UINT_MAX,&parsed)) die("invalid delay: %s",value);
			opts->delay_seconds=(unsigned int)parsed;
		} else if(!strcmp(arg,"--use-decoy")){
			if(seen_decoy++) die("duplicate repeat option: %s",arg);
			if(i+1<argc&&parse_bool_strict(argv[i+1],&opts->use_decoy)==0) i++;
			else opts->use_decoy=1;
		} else if(!strcmp(arg,"--no-decoy")){
			if(seen_decoy++) die("duplicate repeat option: %s",arg);
			opts->use_decoy=0;
		} else if(starts_with(arg,"--use-decoy=")){
			if(seen_decoy++) die("duplicate repeat option: %s",arg);
			value=strchr(arg,'=')+1;
			if(parse_bool_strict(value,&opts->use_decoy)) die("invalid use_decoy: %s",value);
		} else if(!strcmp(arg,"--token-confirmation-idx")||!strcmp(arg,"--token_confirmation_idx")){
			if(seen_token_idx++) die("duplicate repeat option: %s",arg);
			if(++i>=argc) die("missing value for %s",arg);
			value=argv[i]; long long parsed=0;
			if(parse_bounded_ll(value,INT_MIN,INT_MAX,&parsed)) die("invalid token_confirmation_idx: %s",value);
			opts->token_idx_raw=(int)parsed;
		} else if(starts_with(arg,"--token-confirmation-idx=")||starts_with(arg,"--token_confirmation_idx=")){
			if(seen_token_idx++) die("duplicate repeat option: %s",arg);
			value=strchr(arg,'=')+1; long long parsed=0;
			if(parse_bounded_ll(value,INT_MIN,INT_MAX,&parsed)) die("invalid token_confirmation_idx: %s",value);
			opts->token_idx_raw=(int)parsed;
		} else die("unknown repeat option: %s",arg);
	}
	if(repeat_token_index_strict(opts->token_idx_raw,opts->use_decoy?2:1,&opts->token_idx))
		die("token_confirmation_idx %d invalid when use_decoy=%s",opts->token_idx_raw,opts->use_decoy?"true":"false");
}
static int strip_custom_amount_args(int *argc,char **argv,long long *custom_amount){ int w=0; *custom_amount=-1; for(int i=0;i<*argc;i++){ const char *v=NULL; if(!strcmp(argv[i],"--amount")||!strcmp(argv[i],"--price")||!strcmp(argv[i],"--custom-price")){ if(i+1>=*argc) return -1; v=argv[++i]; } else if(starts_with(argv[i],"amount=")) v=argv[i]+7; else if(starts_with(argv[i],"price=")) v=argv[i]+6; else if(starts_with(argv[i],"custom_price=")) v=argv[i]+13; else if(starts_with(argv[i],"custom-price=")) v=argv[i]+13; else { argv[w++]=argv[i]; continue; } if(parse_amount_arg(v,custom_amount)) return -1; } *argc=w; return 0; }
static int confirm_arg_ok(const char *s){ return s&&(!strcmp(s,"1")||!strcmp(s,"true")||!strcmp(s,"yes")||!strcmp(s,"CONFIRM")||!strcmp(s,"confirm")); }
static int strip_json_confirm_args(int *argc,char **argv){ int w=0,ok=0; for(int i=0;i<*argc;i++){ const char *v=NULL; if(!strcmp(argv[i],"--confirm")||!strcmp(argv[i],"confirm")){ ok=1; continue; } if(starts_with(argv[i],"confirm=")) v=argv[i]+8; else if(starts_with(argv[i],"confirmed=")) v=argv[i]+10; if(v){ if(confirm_arg_ok(v)) ok=1; else return -1; continue; } argv[w++]=argv[i]; } *argc=w; return ok; }
static int require_json_payment_confirmed(void){ if(json_payment_confirmed) return 0; printf("{\"ok\":false,\"error\":\"confirm required\",\"confirm\":\"confirm=1\"}\n"); return -1; }
static void cmd_json_payment_balance_mode(const char *mode,int n,char **codes,long long custom_amount){ const char *missing=payment_config_missing(); if(missing){ printf("{\"ok\":false,\"error\":\"missing env\",\"missing\":"); json_str(missing); printf("}\n"); return; } if(require_json_payment_confirmed()) return; Accounts a={0}; char *act=NULL; Account *acc=NULL; Tokens t={0}; if(json_payment_begin(&a,&act,&acc,&t)) return; PaymentQuote q[3]; memset(q,0,sizeof(q)); long long total=0; int loaded=0; if(load_payment_quotes_json(&a,acc,&t,n,codes,q,3,&total,&loaded)){ tokens_free(&t); free(act); accounts_free(&a); return; } long long amount=payment_amount(total,custom_amount); char *resp=payment_pulsa_settle_many_once(&a,acc,&t,q,loaded,amount),*status=json_get_string(resp,"status"),*msg=json_get_string(resp,"message"); int ok=payment_ok_status(status); printf("{\"ok\":%s,\"payment\":\"balance\",\"mode\":",ok?"true":"false"); json_str(mode); printf(",\"message\":"); json_str(msg?msg:(status?status:"unknown")); printf(","); print_quotes_json(q,loaded); print_amount_json(total,amount); printf(",\"response\":"); json_val(resp); printf("}\n"); free(status); free(msg); free(resp); for(int i=0;i<loaded;i++) payment_quote_free(&q[i]); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_payment_decoy_mode(const char *mode,int standard,int n,char **codes,long long custom_amount){
	const char *missing=payment_config_missing();
	if(missing){ printf("{\"ok\":false,\"error\":\"missing env\",\"missing\":"); json_str(missing); printf("}\n"); return; }
	if(require_json_payment_confirmed()) return;
	Accounts a={0}; char *act=NULL; Account *acc=NULL; Tokens t={0};
	if(json_payment_begin(&a,&act,&acc,&t)) return;
	PaymentQuote q[4]; memset(q,0,sizeof(q));
	long long total=0; int loaded=0;
	if(load_payment_quotes_json(&a,acc,&t,n,codes,q,3,&total,&loaded)){ tokens_free(&t); free(act); accounts_free(&a); return; }
	char *err=NULL,*decoy=NULL; int decoy_refreshed=0;
	if(decoy_quote_resolve(&a,acc,&t,&q[loaded],&decoy,&err,&decoy_refreshed)){
		printf("{\"ok\":false,\"error\":"); json_str(err?err:"decoy resolve/validation failed before settlement"); printf("}\n");
		free(err); free(decoy); for(int i=0;i<loaded;i++) payment_quote_free(&q[i]); tokens_free(&t); free(act); accounts_free(&a); return;
	}
	free(decoy);
	if(decoy_refreshed&&payment_quotes_reload_current(&t,q,loaded,&total,&err)){
		printf("{\"ok\":false,\"error\":"); json_str(err?err:"target quote reload failed after decoy refresh"); printf("}\n");
		free(err); payment_quote_free(&q[loaded]); for(int i=0;i<loaded;i++) payment_quote_free(&q[i]); tokens_free(&t); free(act); accounts_free(&a); return;
	}
	if(q[loaded].price>LLONG_MAX-total){
		printf("{\"ok\":false,\"error\":\"payment amount overflow\"}\n");
		payment_quote_free(&q[loaded]); for(int i=0;i<loaded;i++) payment_quote_free(&q[i]); tokens_free(&t); free(act); accounts_free(&a); return;
	}
	total+=q[loaded].price; loaded++;
	long long amount=payment_amount(total,custom_amount);
	int token_idx=standard?0:loaded-1;
	const char *payment_for_override=standard?NULL:DECOY_V2_PAYMENT_FOR;
	char *first=NULL,*resp=payment_pulsa_settle_retry(acc,&t,q,loaded,&amount,token_idx,payment_for_override,&first);
	char *status=json_get_string(resp,"status"),*msg=json_get_string(resp,"message");
	int ok=payment_ok_status(status);
	printf("{\"ok\":%s,\"payment\":\"balance\",\"mode\":",ok?"true":"false"); json_str(mode);
	printf(",\"token_confirmation_idx\":%d,\"payment_for\":",token_idx); json_str(standard?q[0].payment_for:DECOY_V2_PAYMENT_FOR);
	printf(",\"message\":"); json_str(msg?msg:(status?status:"unknown")); printf(","); print_quotes_json(q,loaded); print_amount_json(total,amount);
	printf(",\"response\":"); json_val(resp); if(first){ printf(",\"first_response\":"); json_val(first); } printf("}\n");
	free(first); free(status); free(msg); free(resp); for(int i=0;i<loaded;i++) payment_quote_free(&q[i]); tokens_free(&t); free(act); accounts_free(&a);
}
static void cmd_json_payment_qris_mode(const char *mode,const char *decoy_kind,int n,char **codes,long long custom_amount){
	const char *missing=payment_config_missing();
	if(missing){ printf("{\"ok\":false,\"error\":\"missing env\",\"missing\":"); json_str(missing); printf("}\n"); return; }
	if(require_json_payment_confirmed()) return;
	Accounts accounts={0}; char *active=NULL; Account *acc=NULL; Tokens tokens={0};
	if(json_payment_begin(&accounts,&active,&acc,&tokens)) return;
	PaymentQuote quotes[4]; memset(quotes,0,sizeof(quotes));
	long long total=0; int loaded=0;
	if(load_payment_quotes_json(&accounts,acc,&tokens,n,codes,quotes,3,&total,&loaded)){ tokens_free(&tokens); free(active); accounts_free(&accounts); return; }
	if(decoy_kind){
		char *err=NULL,*decoy=NULL; int refreshed=0;
		if(decoy_quote_resolve(&accounts,acc,&tokens,&quotes[loaded],&decoy,&err,&refreshed)){
			printf("{\"ok\":false,\"error\":"); json_str(err?err:"decoy resolve/validation failed before settlement"); printf("}\n");
			free(err); free(decoy); for(int i=0;i<loaded;i++) payment_quote_free(&quotes[i]); tokens_free(&tokens); free(active); accounts_free(&accounts); return;
		}
		free(decoy);
		if(refreshed&&payment_quotes_reload_current(&tokens,quotes,loaded,&total,&err)){
			printf("{\"ok\":false,\"error\":"); json_str(err?err:"target quote reload failed after decoy refresh"); printf("}\n");
			free(err); payment_quote_free(&quotes[loaded]); for(int i=0;i<loaded;i++) payment_quote_free(&quotes[i]); tokens_free(&tokens); free(active); accounts_free(&accounts); return;
		}
		if(quotes[loaded].price>LLONG_MAX-total){
			printf("{\"ok\":false,\"error\":\"payment amount overflow\"}\n");
			payment_quote_free(&quotes[loaded]); for(int i=0;i<loaded;i++) payment_quote_free(&quotes[i]); tokens_free(&tokens); free(active); accounts_free(&accounts); return;
		}
		total+=quotes[loaded].price; loaded++;
	}
	long long amount=payment_amount(total,custom_amount);
	int token_idx=decoy_kind?loaded-1:0;
	const char *payment_for=decoy_kind?"SHARE_PACKAGE":NULL;
	char *response=payment_qris_settle_many_ex(acc,&tokens,quotes,loaded,amount,token_idx,payment_for);
	char *status=json_get_string(response,"status"),*message=json_get_string(response,"message"),*transaction=json_get_string(response,"transaction_code"),*pending=NULL,*qr=NULL,*qr_url=NULL;
	int ok=payment_ok_status(status);
	if(ok&&transaction&&*transaction){
		pending=get_transaction_status_retry(&accounts,acc,&tokens,transaction,""); qr=json_get_string(pending,"qr_code");
		if(qr&&*qr){ char *encoded=b64_encode((unsigned char *)qr,strlen(qr),1); qr_url=xasprintf("http://192.168.1.1/qrcode.php?data=%s",encoded); free(encoded); }
	}
	printf("{\"ok\":%s,\"payment\":\"qris\",\"mode\":",ok?"true":"false"); json_str(mode);
	printf(",\"message\":"); json_str(message?message:(status?status:"unknown")); printf(","); print_quotes_json(quotes,loaded); print_amount_json(total,amount);
	printf(",\"response\":"); json_val(response); if(transaction){ printf(",\"transaction_code\":"); json_str(transaction); } if(pending){ printf(",\"pending_detail\":"); json_val(pending); } if(qr){ printf(",\"qris_code\":"); json_str(qr); } if(qr_url){ printf(",\"qris_url\":"); json_str(qr_url); } printf("}\n");
	free(status); free(message); free(transaction); free(pending); free(qr); free(qr_url); free(response); for(int i=0;i<loaded;i++) payment_quote_free(&quotes[i]); tokens_free(&tokens); free(active); accounts_free(&accounts);
}
static char *wallet_api_number(const char *in){ char b[32]; size_t j=0; for(size_t i=0;in&&in[i]&&j+1<sizeof(b);i++) if(isdigit((unsigned char)in[i])) b[j++]=in[i]; b[j]=0; if(starts_with(b,"628")) return xasprintf("0%s",b+2); if(b[0]=='8') return xasprintf("0%s",b); return xstrdup(b); }
static const char *ewallet_method_arg(const char *s){ if(!s) return ""; if(!strcmp(s,"DANA")||!strcmp(s,"dana")) return "DANA"; if(!strcmp(s,"SHOPEEPAY")||!strcmp(s,"shopeepay")) return "SHOPEEPAY"; if(!strcmp(s,"GOPAY")||!strcmp(s,"gopay")) return "GOPAY"; return ""; }
static void cmd_json_payment_ewallet_mode(const char *method,int n,char **codes,const char *wallet_arg,long long custom_amount){ const char *missing=payment_config_missing(); if(missing){ printf("{\"ok\":false,\"error\":\"missing env\",\"missing\":"); json_str(missing); printf("}\n"); return; } if(require_json_payment_confirmed()) return; char *wallet=wallet_api_number(wallet_arg); if(!strcmp(method,"DANA")&&(!starts_with(wallet,"08")||strlen(wallet)<10||strlen(wallet)>13)){ printf("{\"ok\":false,\"error\":\"wallet number must start with 08 and contain 10-13 digits\"}\n"); free(wallet); return; } Accounts a={0}; char *act=NULL; Account *acc=NULL; Tokens t={0}; if(json_payment_begin(&a,&act,&acc,&t)){ free(wallet); return; } PaymentQuote q[3]; memset(q,0,sizeof(q)); long long total=0; int loaded=0; if(load_payment_quotes_json(&a,acc,&t,n,codes,q,3,&total,&loaded)){ free(wallet); tokens_free(&t); free(act); accounts_free(&a); return; } long long amount=payment_amount(total,custom_amount); char *resp=payment_ewallet_settle_many(acc,&t,q,loaded,amount,method,wallet); char *status=json_get_string(resp,"status"),*msg=json_get_string(resp,"message"),*deeplink=json_get_string(resp,"deeplink"); int ok=payment_ok_status(status); printf("{\"ok\":%s,\"payment\":\"ewallet\",\"method\":",ok?"true":"false"); json_str(method); printf(",\"message\":"); json_str(msg?msg:(status?status:"unknown")); printf(","); print_quotes_json(q,loaded); print_amount_json(total,amount); printf(",\"response\":"); json_val(resp); if(deeplink){ printf(",\"deeplink\":"); json_str(deeplink); } printf("}\n"); free(wallet); free(status); free(msg); free(deeplink); free(resp); for(int i=0;i<loaded;i++) payment_quote_free(&q[i]); tokens_free(&t); free(act); accounts_free(&a); }
static char *dest_msisdn(const char *in){ char b[32]; size_t j=0; for(size_t i=0;in&&in[i]&&j+1<sizeof(b);i++) if(isdigit((unsigned char)in[i])) b[j++]=in[i]; b[j]=0; if(starts_with(b,"08")) return xasprintf("62%s",b+1); if(b[0]=='8') return xasprintf("62%s",b); return xstrdup(b); }
static void cmd_json_payment_single_special(const char *mode,int n,char **codes,const char *extra){
	const char *missing=payment_config_missing();
	if(missing){ printf("{\"ok\":false,\"error\":\"missing env\",\"missing\":"); json_str(missing); printf("}\n"); return; }
	if(require_json_payment_confirmed()) return;
	if(n!=1){ printf("{\"ok\":false,\"error\":\"payment mode supports one package only\"}\n"); return; }
	char *dest=NULL;
	if(!strcmp(mode,"gift")){
		dest=dest_msisdn(extra);
		if(!msisdn_ok(dest)){ printf("{\"ok\":false,\"error\":\"destination msisdn must start with 628\"}\n"); free(dest); return; }
	}
	Accounts a={0}; char *act=NULL; Account *acc=NULL; Tokens t={0};
	if(json_payment_begin(&a,&act,&acc,&t)){ free(dest); return; }
	PaymentQuote q={0}; char *err=NULL;
	if(payment_quote_load_retry(&a,acc,&t,codes[0],&q,&err)){
		printf("{\"ok\":false,\"error\":"); json_str(err?err:"quote failed"); printf("}\n");
		free(err); free(dest); tokens_free(&t); free(act); accounts_free(&a); return;
	}
	if(strcmp(q.payment_for?q.payment_for:"","REDEEM_VOUCHER")){
		printf("{\"ok\":false,\"error\":\"payment mode requires REDEEM_VOUCHER\",\"payment\":");
		json_str(mode); printf(",\"payment_for\":"); json_str(q.payment_for?q.payment_for:"");
		printf(",\"quote\":"); payment_quote_print_json(&q); printf("}\n");
		free(dest); payment_quote_free(&q); tokens_free(&t); free(act); accounts_free(&a); return;
	}
	char *resp=!strcmp(mode,"point")?payment_point_settle(acc,&t,&q):(!strcmp(mode,"voucher")?payment_voucher_settle(acc,&t,&q):payment_gift_settle(acc,&t,&q,dest));
	char *status=json_get_string(resp,"status"),*msg=json_get_string(resp,"message");
	int ok=payment_ok_status(status);
	printf("{\"ok\":%s,\"payment\":",ok?"true":"false"); json_str(mode); printf(",\"message\":"); json_str(msg?msg:(status?status:"unknown")); printf(",\"quote\":"); payment_quote_print_json(&q); printf(",\"response\":"); json_val(resp); printf("}\n");
	free(dest); free(status); free(msg); free(resp); payment_quote_free(&q); tokens_free(&t); free(act); accounts_free(&a);
}
static void cmd_json_quota(const char *number_arg,int fresh){ Accounts a=accounts_load(); char *act=NULL; const char *number=number_arg; if(!number){ act=active_get(); number=act; } Account *acc=(number&&*number)?accounts_find(&a,number):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"account not found\",\"active\":"); json_str(number?number:""); printf("}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); if(fresh) response_cache_delete(acc,"quota"); char *quota=get_quota_retry(&a,acc,&t); if(api_auth_failed(quota)){ json_auth_error(quota); } else { json_val(quota); putchar('\n'); } free(quota); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_tiering(void){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\",\"active\":"); json_str(act?act:""); printf("}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *tier=get_tiering_info_retry(&a,acc,&t); printf("{\"ok\":%s,\"number\":",api_auth_failed(tier)?"false":"true"); json_str(acc->number); printf(",\"subscription_type\":"); json_str(acc->subscription_type); printf(",\"tiering\":"); json_val(tier); printf("}\n"); free(tier); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_transaction_history(void){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\",\"active\":"); json_str(act?act:""); printf("}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *history=get_transaction_history_retry(&a,acc,&t); int ok=!api_auth_failed(history)&&transaction_history_valid(history); printf("{\"ok\":%s",ok?"true":"false"); if(!ok) printf(",\"error\":\"transaction history failed\""); printf(",\"number\":"); json_str(acc->number); printf(",\"subscription_type\":"); json_str(acc->subscription_type); printf(",\"history\":"); json_val(history); printf("}\n"); free(history); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_pending_transactions(void){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\",\"active\":"); json_str(act?act:""); printf("}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *pending=get_pending_transactions_retry(&a,acc,&t); int ok=!api_auth_failed(pending)&&api_success_response(pending),has_pending=pending_payment_present(pending); printf("{\"ok\":%s",ok?"true":"false"); if(!ok) printf(",\"error\":\"pending transactions failed\""); printf(",\"has_pending_payment\":%s,\"number\":",has_pending?"true":"false"); json_str(acc->number); printf(",\"subscription_type\":"); json_str(acc->subscription_type); printf(",\"pending\":"); json_val(pending); printf("}\n"); free(pending); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_transaction_status(const char *tx,const char *status_arg){ if(!tx||!*tx){ printf("{\"ok\":false,\"error\":\"missing transaction id\"}\n"); return; } Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\",\"active\":"); json_str(act?act:""); printf("}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *detail=get_transaction_status_retry(&a,acc,&t,tx,status_arg?status_arg:""); int ok=!api_auth_failed(detail)&&api_success_response(detail); printf("{\"ok\":%s",ok?"true":"false"); if(!ok) printf(",\"error\":\"transaction status failed\""); printf(",\"transaction_id\":"); json_str(tx); printf(",\"status_arg\":"); json_str(status_arg?status_arg:""); printf(",\"detail\":"); json_val(detail); printf("}\n"); free(detail); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_notifications(void){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\",\"active\":"); json_str(act?act:""); printf("}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *notifications=get_notifications_retry(&a,acc,&t); int ok=!api_auth_failed(notifications)&&api_success_response(notifications); printf("{\"ok\":%s",ok?"true":"false"); if(!ok) printf(",\"error\":\"notifications failed\""); printf(",\"number\":"); json_str(acc->number); printf(",\"subscription_type\":"); json_str(acc->subscription_type); printf(",\"notifications\":"); json_val(notifications); printf("}\n"); free(notifications); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_notification_read(int n,char **ids){ if(n<1){ printf("{\"ok\":false,\"error\":\"missing notification id\"}\n"); return; } if(n>64){ printf("{\"ok\":false,\"error\":\"too many notification ids\",\"limit\":64}\n"); return; } for(int i=0;i<n;i++) if(!notification_id_ok(ids[i])){ printf("{\"ok\":false,\"error\":\"invalid notification id\",\"index\":%d}\n",i); return; } Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\",\"active\":"); json_str(act?act:""); printf("}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); int marked=0,attempted=0,auth_failed=0; printf("{\"results\":["); for(int i=0;i<n;i++){ char *detail=get_notification_detail_once(acc,&t,ids[i]); int auth=api_auth_failed(detail),ok=!auth&&api_success_response(detail); if(attempted) putchar(','); printf("{\"notification_id\":"); json_str(ids[i]); printf(",\"ok\":%s,\"response\":",ok?"true":"false"); json_val(detail); putchar('}'); attempted++; if(ok) marked++; free(detail); if(auth){ auth_failed=1; break; } } printf("],\"ok\":%s,\"requested\":%d,\"attempted\":%d,\"marked\":%d",marked==n?"true":"false",n,attempted,marked); if(auth_failed) printf(",\"error\":\"auth failed; retry action manually\""); else if(marked!=n) printf(",\"error\":\"one or more notifications failed\""); printf("}\n"); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json_dashboard(void){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ printf("{\"ok\":false,\"error\":\"no active account\",\"active\":"); json_str(act?act:""); printf("}\n"); free(act); accounts_free(&a); return; } Tokens t={0}; if(refresh_account(acc,&t)){ printf("{\"ok\":false,\"error\":\"refresh failed\",\"number\":"); json_str(acc->number); printf("}\n"); free(act); accounts_free(&a); return; } account_sync(&a,acc,&t); char *bal=get_balance_retry(&a,acc,&t),*quota=get_quota_retry(&a,acc,&t); if(api_auth_failed(bal)||api_auth_failed(quota)){ printf("{\"ok\":false,\"error\":\"auth failed\",\"number\":"); json_str(acc->number); printf(",\"balance\":"); json_val(bal); printf(",\"quota\":"); json_val(quota); printf("}\n"); free(bal); free(quota); tokens_free(&t); free(act); accounts_free(&a); return; } printf("{\"ok\":true,\"number\":"); json_str(acc->number); printf(",\"subscription_type\":"); json_str(acc->subscription_type); printf(",\"balance\":"); json_val(bal); printf(",\"quota\":"); json_val(quota); printf("}\n"); free(bal); free(quota); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_json(int argc,char **argv){
	if(argc<1){ printf("{\"ok\":false,\"error\":\"missing json command\"}\n"); return; }
	if(!strcmp(argv[0],"dashboard")||!strcmp(argv[0],"status")){ if(json_require_config()) return; cmd_json_dashboard(); }
	else if(!strcmp(argv[0],"accounts")) cmd_json_accounts();
	else if(!strcmp(argv[0],"env")||!strcmp(argv[0],"settings")) cmd_json_env();
	else if(!strcmp(argv[0],"quota")){ if(json_require_config()) return; cmd_json_quota(argc>=2?argv[1]:NULL,argc>=3&&!strcmp(argv[2],"fresh")); }
	else if(!strcmp(argv[0],"tiering")){ if(json_require_config()) return; cmd_json_tiering(); }
	else if(!strcmp(argv[0],"transaction-history")||!strcmp(argv[0],"history")){ if(json_require_config()) return; cmd_json_transaction_history(); }
	else if(!strcmp(argv[0],"pending")||!strcmp(argv[0],"pending-transactions")){ if(json_require_config()) return; cmd_json_pending_transactions(); }
	else if(!strcmp(argv[0],"transaction-status")||!strcmp(argv[0],"status-transaction")||!strcmp(argv[0],"payment-status")){ if(json_require_config()) return; cmd_json_transaction_status(argc>=2?argv[1]:NULL,argc>=3?argv[2]:""); }
	else if(!strcmp(argv[0],"notifications")||!strcmp(argv[0],"notification")){ if(json_require_config()) return; cmd_json_notifications(); }
	else if(!strcmp(argv[0],"notification-detail")||!strcmp(argv[0],"notification-read")||!strcmp(argv[0],"notification-read-all")){ if(json_require_config()) return; cmd_json_notification_read(argc-1,argv+1); }
	else if(!strcmp(argv[0],"point")||!strcmp(argv[0],"redeemables")){ if(json_require_config()) return; cmd_json_redeemables(argc>=2?argv[1]:""); }
	else if(!strcmp(argv[0],"segments")||!strcmp(argv[0],"store-segments")){ if(json_require_config()) return; cmd_json_store_segments(argc>=2?argv[1]:""); }
	else if(!strcmp(argv[0],"shop")||!strcmp(argv[0],"store-packages")){
		if(json_require_config()) return;
		if(argc>=3&&!strcmp(argv[1],"family")) cmd_json_shop_family(argv[2],argc>=4?argv[3]:"");
		else if(argc>=2&&!strcmp(argv[1],"family-list")) cmd_json_shop_family_list(argc>=3?argv[2]:"");
		else cmd_json_shop(argc>=2?argv[1]:"");
	}
	else if(!strcmp(argv[0],"family-list")){ if(json_require_config()) return; cmd_json_shop_family_list(argc>=2?argv[1]:""); }
	else if(!strcmp(argv[0],"family")){ if(json_require_config()) return; cmd_json_shop_family(argc>=2?argv[1]:NULL,argc>=3?argv[2]:""); }
	else if((!strcmp(argv[0],"package")||!strcmp(argv[0],"detail"))&&argc>=2){ if(json_require_config()) return; if(!strcmp(argv[0],"package")&&!strcmp(argv[1],"detail")) cmd_json_package_detail(argc>=3?argv[2]:NULL,argc>=4?argv[3]:"",argc>=5?argv[4]:"",argc>=6?argv[5]:""); else cmd_json_package_detail(argv[1],argc>=3?argv[2]:"",argc>=4?argv[3]:"",argc>=5?argv[4]:""); }
	else if(!strcmp(argv[0],"payment")&&argc>=2){
		if(json_require_config()) return;
		int pay_argc=argc-2;
		char **pay_argv=argv+2;
		long long custom_amount=-1;
		json_payment_confirmed=strip_json_confirm_args(&pay_argc,pay_argv);
		if(json_payment_confirmed<0){ printf("{\"ok\":false,\"error\":\"invalid confirm\"}\n"); return; }
		if(strip_custom_amount_args(&pay_argc,pay_argv,&custom_amount)){ printf("{\"ok\":false,\"error\":\"invalid custom amount\"}\n"); return; }
		if(!strcmp(argv[1],"quote")){ if(pay_argc<1) printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); else cmd_json_payment_quote(pay_argv[0]); }
		else if(!strcmp(argv[1],"probe")){ if(pay_argc<1) printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); else cmd_json_payment_probe(pay_argv[0]); }
		else if(!strcmp(argv[1],"pulsa")){ if(pay_argc<1) printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); else cmd_json_payment_pulsa(pay_argv[0],pay_argc>=2?pay_argv[1]:NULL,custom_amount); }
		else if(!strcmp(argv[1],"cart")) cmd_json_payment_pulsa_cart(pay_argc,pay_argv,custom_amount);
		else if(!strcmp(argv[1],"auto")||!strcmp(argv[1],"balance")) cmd_json_payment_balance_mode(argv[1],pay_argc,pay_argv,custom_amount);
		else if(!strcmp(argv[1],"balance-decoy-standard")||!strcmp(argv[1],"decoy-standard")) cmd_json_payment_decoy_mode("balance-decoy-standard",1,pay_argc,pay_argv,custom_amount);
		else if(!strcmp(argv[1],"balance-decoy-v2")||!strcmp(argv[1],"decoy-v2")||!strcmp(argv[1],"balance-decoy")||!strcmp(argv[1],"decoy")||!strcmp(argv[1],"prio")) cmd_json_payment_decoy_mode("balance-decoy-v2",0,pay_argc,pay_argv,custom_amount);
		else if(!strcmp(argv[1],"qris")) cmd_json_payment_qris_mode("qris",NULL,pay_argc,pay_argv,custom_amount);
		else if(!strcmp(argv[1],"dana")){ if(pay_argc<2) printf("{\"ok\":false,\"error\":\"missing wallet/package\"}\n"); else cmd_json_payment_ewallet_mode("DANA",pay_argc-1,pay_argv,pay_argv[pay_argc-1],custom_amount); }
		else if(!strcmp(argv[1],"shopeepay")) cmd_json_payment_ewallet_mode("SHOPEEPAY",pay_argc,pay_argv,"",custom_amount);
		else if(!strcmp(argv[1],"gopay")) cmd_json_payment_ewallet_mode("GOPAY",pay_argc,pay_argv,"",custom_amount);
		else if(!strcmp(argv[1],"ewallet")){ const char *m=pay_argc>=1?ewallet_method_arg(pay_argv[0]):""; if(pay_argc<3) printf("{\"ok\":false,\"error\":\"usage: payment ewallet METHOD wallet_or_dash code...\"}\n"); else if(!*m) printf("{\"ok\":false,\"error\":\"unsupported ewallet\"}\n"); else cmd_json_payment_ewallet_mode(m,pay_argc-2,pay_argv+2,strcmp(pay_argv[1],"-")?pay_argv[1]:"",custom_amount); }
		else if(!strcmp(argv[1],"point")){ if(custom_amount>=0) printf("{\"ok\":false,\"error\":\"custom amount not supported for point\"}\n"); else cmd_json_payment_single_special("point",pay_argc,pay_argv,NULL); }
		else if(!strcmp(argv[1],"voucher")){ if(custom_amount>=0) printf("{\"ok\":false,\"error\":\"custom amount not supported for voucher\"}\n"); else cmd_json_payment_single_special("voucher",pay_argc,pay_argv,NULL); }
		else if(!strcmp(argv[1],"gift")){ if(custom_amount>=0) printf("{\"ok\":false,\"error\":\"custom amount not supported for gift\"}\n"); else if(pay_argc<2) printf("{\"ok\":false,\"error\":\"missing destination/package\"}\n"); else cmd_json_payment_single_special("gift",pay_argc-1,pay_argv,pay_argv[pay_argc-1]); }
		else if(!strcmp(argv[1],"status")||!strcmp(argv[1],"pending-detail")) cmd_json_transaction_status(pay_argc>=1?pay_argv[0]:NULL,pay_argc>=2?pay_argv[1]:"");
		else if(!strcmp(argv[1],"history")||!strcmp(argv[1],"transaction-history")) cmd_json_transaction_history();
		else printf("{\"ok\":false,\"error\":\"unknown payment command\"}\n");
	}
	else if(!strcmp(argv[0],"pay-quote")&&argc>=2){ if(json_require_config()) return; cmd_json_payment_quote(argv[1]); }
	else if(!strcmp(argv[0],"pay-pulsa")&&argc>=2){ if(json_require_config()) return; int pay_argc=argc-1; char **pay_argv=argv+1; long long custom_amount=-1; if(strip_custom_amount_args(&pay_argc,pay_argv,&custom_amount)){ printf("{\"ok\":false,\"error\":\"invalid custom amount\"}\n"); return; } if(pay_argc<1) printf("{\"ok\":false,\"error\":\"missing package code\"}\n"); else cmd_json_payment_pulsa(pay_argv[0],pay_argc>=2?pay_argv[1]:NULL,custom_amount); }
	else if(!strcmp(argv[0],"use")&&argc>=2) cmd_json_use(argv[1]);
	else if(!strcmp(argv[0],"del")&&argc>=2) cmd_json_del(argv[1]);
	else if(!strcmp(argv[0],"logout")||!strcmp(argv[0],"reset-session")) cmd_json_logout();
	else if((!strcmp(argv[0],"unsub")||!strcmp(argv[0],"unsubscribe"))&&argc>=2){ if(json_require_config()) return; cmd_json_unsub(argv[1],argc>=3?argv[2]:"",argc>=4?argv[3]:""); }
	else if(!strcmp(argv[0],"login")&&argc>=2){ if(json_require_config()) return; cmd_json_login(argv[1]); }
	else if(!strcmp(argv[0],"otp")&&argc>=3){ if(json_require_config()) return; cmd_json_otp(argv[1],argv[2]); }
	else printf("{\"ok\":false,\"error\":\"unknown json command\"}\n");
}

static void prompt_line(const char *label, char *buf, size_t sz){ printf("%s",label); fflush(stdout); if(!fgets(buf,sz,stdin)){ buf[0]=0; return; } trim(buf); }
static void pause_enter(void){ char b[8]; printf("Tekan Enter untuk lanjut..."); fflush(stdout); fgets(b,sizeof(b),stdin); }
static char *normalize_msisdn(const char *input){ size_t len=input?strlen(input):0,j=0; char *digits=xmalloc(len+1); for(size_t i=0;i<len;i++) if(isdigit((unsigned char)input[i])) digits[j++]=input[i]; digits[j]=0; if(starts_with(digits,"08")){ char *number=xasprintf("62%s",digits+1); free(digits); return number; } if(digits[0]=='8'){ char *number=xasprintf("62%s",digits); free(digits); return number; } return digits; }
static int msisdn_ok(const char *s){ if(!starts_with(s,"628")) return 0; size_t n=strlen(s); if(n<8||n>14) return 0; for(size_t i=0;i<n;i++) if(!isdigit((unsigned char)s[i])) return 0; return 1; }
static char *login_msisdn(const char *input){ char *number=normalize_msisdn(input); if(msisdn_ok(number)) return number; free(number); return NULL; }
static int otp_ok(const char *s){ if(!s||strlen(s)!=6) return 0; for(size_t i=0;i<6;i++) if(!isdigit((unsigned char)s[i])) return 0; return 1; }
static void interactive_login_prompt(void){
	char input[32],otp[32];
	printf("-------------------------------------------------------\nLogin ke MyXL\n-------------------------------------------------------\n");
	prompt_line("Nomor XL (08xxxx / 628xxxx): ",input,sizeof(input));
	char *number=login_msisdn(input);
	if(!number){ puts("Nomor tidak valid. Gunakan format 08 atau 628."); pause_enter(); return; }
	char *otp_res=request_otp(number);
	if(!strstr(otp_res,"subscriber_id")){ printf("Gagal request OTP:\n%s\n",otp_res); free(otp_res); free(number); pause_enter(); return; }
	free(otp_res);
	puts("OTP berhasil dikirim.");
	for(int tries=3;tries>0;tries--){
		printf("Sisa percobaan: %d\n",tries);
		prompt_line("Masukkan OTP: ",otp,sizeof(otp));
		if(!otp_ok(otp)){ puts("OTP tidak valid. Harus 6 digit angka."); continue; }
		char *resp=submit_otp("SMS",number,otp);
		Tokens t={0};
		if(parse_tokens(resp,&t)){ printf("OTP salah/gagal:\n%s\n",resp); free(resp); continue; }
		free(resp);
		char *profile=get_profile(&t),*sub=json_get_string(profile,"subscriber_id"),*typ=json_get_string(profile,"subscription_type");
		Accounts a=accounts_load();
		accounts_put(&a,number,sub?sub:"",typ?typ:"",t.refresh_token);
		Account *saved=accounts_find(&a,number);
		if(saved) token_cache_save(saved,&t);
		accounts_save(&a);
		active_set(number);
		printf("Berhasil login: %s [%s]\n",number,typ?typ:"unknown");
		free(sub); free(typ); free(profile); accounts_free(&a); tokens_free(&t); free(number);
		pause_enter();
		return;
	}
	free(number);
	puts("Gagal login setelah beberapa percobaan.");
	pause_enter();
}
static void interactive_account_menu(void){ for(;;){ Accounts a=accounts_load(); char *act=active_get(); printf("\033[H\033[J"); printf("-------------------------------------------------------\nAkun Tersimpan:\n"); if(!a.len) puts("Tidak ada akun tersimpan."); for(size_t i=0;i<a.len;i++) printf("%zu. %s [%s] %s\n",i+1,a.items[i].number,a.items[i].subscription_type,(act&&strcmp(act,a.items[i].number)==0)?"(Aktif)":""); printf("-------------------------------------------------------\nCommand:\n0: Tambah Akun\nMasukan nomor urut akun untuk berganti.\nMasukan del <nomor urut> untuk menghapus akun tertentu.\n00: Kembali ke menu utama\n-------------------------------------------------------\n"); char choice[64]; prompt_line("Pilihan: ",choice,sizeof(choice)); if(!strcmp(choice,"00")){ free(act); accounts_free(&a); return; } if(!strcmp(choice,"0")){ free(act); accounts_free(&a); interactive_login_prompt(); continue; } if(starts_with(choice,"del ")){ int idx=atoi(choice+4); if(idx<1 || (size_t)idx>a.len){ puts("Nomor urut tidak valid."); pause_enter(); } else if(act && !strcmp(act,a.items[idx-1].number)){ puts("Tidak dapat menghapus akun aktif. Ganti akun dulu."); pause_enter(); } else { char confirm[8]; printf("Yakin hapus akun %s? (y/n): ",a.items[idx-1].number); fflush(stdout); fgets(confirm,sizeof(confirm),stdin); if(confirm[0]=='y'||confirm[0]=='Y'){ char *number=xstrdup(a.items[idx-1].number); delete_account_number(number); free(number); puts("Akun berhasil dihapus."); } else puts("Penghapusan dibatalkan."); pause_enter(); } free(act); accounts_free(&a); continue; } if(choice[0] && strspn(choice,"0123456789")==strlen(choice)){ int idx=atoi(choice); if(idx>=1 && (size_t)idx<=a.len){ active_set(a.items[idx-1].number); printf("Akun aktif: %s\n",a.items[idx-1].number); pause_enter(); free(act); accounts_free(&a); return; } } puts("Input tidak valid."); pause_enter(); free(act); accounts_free(&a); } }
static void interactive_quota(const char *number){ for(;;){ Accounts a=accounts_load(); Account *acc=accounts_find(&a,number); if(!acc){ printf("Akun tidak ditemukan: %s\n",number); accounts_free(&a); pause_enter(); return; } Tokens t={0}; if(refresh_account(acc,&t)){ puts("Gagal refresh token."); accounts_free(&a); pause_enter(); return; } account_sync(&a,acc,&t); char *q=get_quota(&t); QuotaList ql={0}; show_quota_pretty(q,acc->number,acc->subscription_type,&ql); printf("%sCommand:%s del <nomor paket> hapus kuota, 00/Enter kembali\n",CB,C0); char choice[64]; prompt_line("Pilihan: ",choice,sizeof(choice)); if(!choice[0]||!strcmp(choice,"00")){ ql_free(&ql); free(q); tokens_free(&t); accounts_free(&a); return; } if(starts_with(choice,"del ")){ int n=atoi(choice+4); if(n<1||(size_t)n>ql.n){ puts("Nomor paket tidak valid."); pause_enter(); } else { char yn[8]; printf("Hapus paket %d: %s? (y/n): ",n,ql.v[n-1].name); fflush(stdout); fgets(yn,sizeof(yn),stdin); if(yn[0]=='y'||yn[0]=='Y'){ char *res=unsubscribe_quota_once(&a,acc,&t,ql.v[n-1].code,ql.v[n-1].subtype,ql.v[n-1].domain); char *status=json_get_string(res,"status"),*msg=json_get_string(res,"message"); printf("Result: %s%s%s\n",status?status:"UNKNOWN",msg?" - ":"",msg?msg:""); free(status); free(msg); free(res); } else puts("Dibatalkan."); pause_enter(); } } else { puts("Input tidak valid."); pause_enter(); } ql_free(&ql); free(q); tokens_free(&t); accounts_free(&a); } }
static void interactive_payment_pulsa(Accounts *a,Account *acc,Tokens *t,const char *option_code);
static void interactive_payment_special(Accounts *a,Account *acc,Tokens *t,const char *option_code,const char *mode);
static void interactive_payment_decoy(Accounts *a,Account *acc,Tokens *t,const char *option_code,int standard);
static void interactive_payment_qris(Accounts *a,Account *acc,Tokens *t,const char *option_code);
static void interactive_payment_ewallet(Accounts *a,Account *acc,Tokens *t,const char *option_code);
static void interactive_purchase_n_times_option(const char *option_code);
static int confirm_amount(const char *label,long long total);
static long long custom_amount_or_prompt(long long total,long long custom_amount);
static int print_payment_response(const char *resp);
static int interactive_payment_decoy_quotes(Accounts *a,Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,long long custom_amount,int standard);
static int interactive_payment_qris_quotes(Accounts *a,Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,long long custom_amount);
static int interactive_payment_ewallet_quotes(Accounts *a,Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,long long custom_amount);
static int interactive_payment_special_quote(Accounts *a,Account *acc,Tokens *t,PaymentQuote *q,const char *mode);
static const char *family_icon(const char *family){ static char out[12]; if(!family) return "[PKG]"; char b[256]; size_t n=strlen(family); if(n>=sizeof(b)) n=sizeof(b)-1; for(size_t i=0;i<n;i++) b[i]=(char)toupper((unsigned char)family[i]); b[n]=0; if(strstr(b,"HOT")) return "[HOT]"; if(strstr(b,"GAME")) return "[GM]"; if(strstr(b,"VIDEO")||strstr(b,"VIDIO")||strstr(b,"YOUTUBE")||strstr(b,"TIKTOK")) return "[VID]"; if(strstr(b,"AKRAB")||strstr(b,"FAMILY")) return "[AK]"; if(strstr(b,"VOICE")||strstr(b,"NELPON")||strstr(b,"CALL")) return "[CALL]"; if(strstr(b,"SMS")||strstr(b,"TEXT")) return "[SMS]"; if(strstr(b,"UNLIMITED")) return "[UL]"; if(strstr(b,"COMBO")||strstr(b,"DATA")||strstr(b,"KUOTA")) return "[DATA]"; char abbr[5]={0}; int k=0,new_word=1; for(size_t i=0;b[i]&&k<4;i++){ if(isalnum((unsigned char)b[i])&&new_word){ abbr[k++]=b[i]; new_word=0; } else if(!isalnum((unsigned char)b[i])) new_word=1; } if(!abbr[0]) snprintf(out,sizeof(out),"[PKG]"); else snprintf(out,sizeof(out),"[%s]",abbr); return out; }
static char *first_json_string(const char *json,const char **keys,size_t n){ for(size_t i=0;i<n;i++){ char *v=json_get_string(json,keys[i]); if(v&&*v) return v; free(v); } return NULL; }
static void show_package_detail_cli(Accounts *a,Account *acc,Tokens *t,const char *option_code,CartItem cart[3],int *cart_n){
	char *detail=get_package_detail_retry(a,acc,t,option_code);
	if(!package_detail_valid(detail)){ printf("Gagal load detail: %s\n",detail); free(detail); pause_enter(); return; }
	char *data=json_object_dup(detail,"data"); if(!data) data=xstrdup(detail);
	char *option=json_object_dup(data,"package_option"); if(!option) option=xstrdup("{}");
	char *family=json_object_dup(data,"package_family"); if(!family) family=xstrdup("{}");
	char *variant=json_object_dup(data,"package_detail_variant"); if(!variant) variant=xstrdup("{}");
	char *fname=json_get_string(family,"name"),*vname=json_get_string(variant,"name"),*oname=json_get_string(option,"name"),*validity=json_get_string(option,"validity"),*payfor=json_get_string(family,"payment_for"),*plan=json_get_string(family,"plan_type");
	long long price=json_get_ll_any(option,"price",0),point=json_get_ll_any(option,"point",0);
	char pricebuf[64]; money_id(price,pricebuf,sizeof(pricebuf));
	int redeem=payfor&&!strcmp(payfor,"REDEEM_VOUCHER");
	printf("\033[H\033[J%s=======================================================%s\n",CC,C0);
	printf("%sDetail Paket%s\n",CB,C0);
	printf("%s=======================================================%s\n",CC,C0);
	printf("Nama       : %s%s%s%s%s%s%s\n",fname?fname:"",vname&&*vname?" - ":"",vname?vname:"",oname&&*oname?" - ":"",oname?oname:"",C0,"");
	printf("Harga      : Rp %s\nMasa Aktif : %s\nPoint      : %lld\nPayment For: %s\nPlan Type  : %s\n",pricebuf,validity?validity:"-",point,payfor?payfor:"-",plan?plan:"-");
	printf("%s-------------------------------------------------------%s\nKuota Detail\n",CL,C0);
	const char *bs,*be;
	if(!json_array_span(option,"benefits",&bs,&be)){
		const char *bp=bs;
		while(bp<be){
			char *benefit=next_object(&bp,be); if(!benefit) break;
			char *name=json_get_string(benefit,"name"),*type=json_get_string(benefit,"data_type");
			long long total=json_get_ll_any(benefit,"total",0),unlimited=json_get_ll_any(benefit,"is_unlimited",0);
			char quota[64]; if(unlimited) snprintf(quota,sizeof(quota),"Unlimited"); else quota_value_str(type?type:"DATA",total,quota,sizeof(quota));
			printf("- %-32s %12s\n",name?name:"Benefit",quota);
			free(name); free(type); free(benefit);
		}
	} else puts("Tidak ada detail kuota.");
	printf("%s-------------------------------------------------------%s\nCart: %d/3\n",CL,C0,*cart_n);
	char choice[16];
	prompt_line(redeem?"a cart, b pulsa, n Pulsa N Kali, s Balance + Decoy, x Balance + Decoy V2, q qris, e e-wallet, p point, v voucher/bonus, g kirim bonus, Enter kembali: ":"a cart, b pulsa, n Pulsa N Kali, s Balance + Decoy, x Balance + Decoy V2, q qris, e e-wallet, Enter kembali: ",choice,sizeof(choice));
	if(choice[0]=='a'||choice[0]=='A'){
		if(*cart_n>=3) puts("Cart penuh. Max 3 paket.");
		else {
			snprintf(cart[*cart_n].code,sizeof(cart[*cart_n].code),"%s",option_code);
			snprintf(cart[*cart_n].name,sizeof(cart[*cart_n].name),"%s%s%s",vname?vname:"",(vname&&oname)?" ":"",oname?oname:option_code);
			cart[*cart_n].price=price; (*cart_n)++; puts("Masuk cart.");
		}
		pause_enter();
	} else if(choice[0]=='b'||choice[0]=='B') interactive_payment_pulsa(a,acc,t,option_code);
	else if(choice[0]=='n'||choice[0]=='N') interactive_purchase_n_times_option(option_code);
	else if(choice[0]=='s'||choice[0]=='S') interactive_payment_decoy(a,acc,t,option_code,1);
	else if(choice[0]=='x'||choice[0]=='X') interactive_payment_decoy(a,acc,t,option_code,0);
	else if(choice[0]=='q'||choice[0]=='Q') interactive_payment_qris(a,acc,t,option_code);
	else if(choice[0]=='e'||choice[0]=='E') interactive_payment_ewallet(a,acc,t,option_code);
	else if(redeem&&(choice[0]=='p'||choice[0]=='P')) interactive_payment_special(a,acc,t,option_code,"point");
	else if(redeem&&(choice[0]=='v'||choice[0]=='V')) interactive_payment_special(a,acc,t,option_code,"voucher");
	else if(redeem&&(choice[0]=='g'||choice[0]=='G')) interactive_payment_special(a,acc,t,option_code,"gift");
	free(detail); free(data); free(option); free(family); free(variant); free(fname); free(vname); free(oname); free(validity); free(payfor); free(plan);
}
static void checkout_cart_pulsa(Accounts *a,Account *acc,Tokens *t,CartItem cart[3],int *cart_n){
	if(*cart_n<1){ puts("Cart kosong."); pause_enter(); return; }
	const char *missing=payment_config_missing();
	if(missing){ printf("Missing env %s.\n",missing); pause_enter(); return; }
	PaymentQuote q[4]; memset(q,0,sizeof(q));
	long long total=0; int loaded=0;
	for(int i=0;i<*cart_n;i++){
		char *err=NULL; int refreshed=0;
		if(payment_quote_load_retry_track(a,acc,t,cart[i].code,&q[i],&err,&refreshed)){
			printf("Quote gagal %s: %s\n",cart[i].name,err?err:"unknown");
			free(err);
			for(int j=0;j<loaded;j++) payment_quote_free(&q[j]);
			pause_enter();
			return;
		}
		if(refreshed&&loaded>0){
			long long reloaded_total=0;
			if(payment_quotes_reload_current(t,q,loaded,&reloaded_total,&err)){
				printf("Quote sebelumnya gagal dimuat ulang setelah refresh token: %s\n",err?err:"unknown");
				free(err); payment_quote_free(&q[i]); pause_enter(); return;
			}
			total=reloaded_total;
		}
		if(q[i].price>LLONG_MAX-total){
			puts("Total quote cart overflow."); payment_quote_free(&q[i]); for(int j=0;j<loaded;j++) payment_quote_free(&q[j]); pause_enter(); return;
		}
		total+=q[i].price;
		loaded++;
	}
	char totalbuf[64],choice[32];
	money_id(total,totalbuf,sizeof(totalbuf));
	printf("Total checkout: Rp %s\n",totalbuf);
	printf("Mode: b Balance, s Balance + Decoy, x Balance + Decoy V2, q QRIS, e E-Wallet");
	if(loaded==1&&!strcmp(q[0].payment_for?q[0].payment_for:"","REDEEM_VOUCHER")) printf(", p point, v voucher/bonus, g kirim bonus");
	printf(", Enter batal\n");
	prompt_line("Pilihan: ",choice,sizeof(choice));
	if(!choice[0]) puts("Dibatalkan.");
	else if(choice[0]=='b'||choice[0]=='B'){
		long long amount=custom_amount_or_prompt(total,-1);
		if(!confirm_amount("Balance",amount)){
			char *resp=payment_pulsa_settle_many_once(a,acc,t,q,loaded,amount),*status=json_get_string(resp,"status");
			print_payment_response(resp);
			if(payment_ok_status(status)) *cart_n=0;
			free(status); free(resp);
		}
	} else if(choice[0]=='s'||choice[0]=='S'){ if(interactive_payment_decoy_quotes(a,acc,t,q,loaded,total,-1,1)) *cart_n=0; }
	else if(choice[0]=='x'||choice[0]=='X'){ if(interactive_payment_decoy_quotes(a,acc,t,q,loaded,total,-1,0)) *cart_n=0; }
	else if(choice[0]=='q'||choice[0]=='Q'){ if(interactive_payment_qris_quotes(a,acc,t,q,loaded,total,-1)) *cart_n=0; }
	else if(choice[0]=='e'||choice[0]=='E'){ if(interactive_payment_ewallet_quotes(a,acc,t,q,loaded,total,-1)) *cart_n=0; }
	else if(loaded==1&&(choice[0]=='p'||choice[0]=='P')){ if(interactive_payment_special_quote(a,acc,t,&q[0],"point")) *cart_n=0; }
	else if(loaded==1&&(choice[0]=='v'||choice[0]=='V')){ if(interactive_payment_special_quote(a,acc,t,&q[0],"voucher")) *cart_n=0; }
	else if(loaded==1&&(choice[0]=='g'||choice[0]=='G')){ if(interactive_payment_special_quote(a,acc,t,&q[0],"gift")) *cart_n=0; }
	else puts("Pilihan tidak valid.");
	for(int i=0;i<loaded;i++) payment_quote_free(&q[i]);
	pause_enter();
}
static int show_store_packages_pretty(const char *json,QuotaList *choices){ const char *start,*end,*arrays[]={"results_price_only","results","packages"}; if(json_array_span_any(json,arrays,3,&start,&end)){ printf("%sGagal membaca Store Packages.%s\n%s\n",CR,C0,json); return 0; } const char *p=start; int idx=1,total=0; printf("%s=======================================================%s\n%s                  Store Packages%s\n%s=======================================================%s\n",CC,C0,CB,C0,CC,C0); char printed[4096]=""; while(p<end){ char *item=next_object(&p,end); if(!item) break; const char *family_keys[]={"package_family_code","family_code","family_id","family_name"}; const char *name_keys[]={"title","name","package_name"}; const char *code_keys[]={"action_param","package_option_code","option_code"}; const char *type_keys[]={"action_type"}; char *family=first_json_string(item,family_keys,4),*title=first_json_string(item,name_keys,3),*code=first_json_string(item,code_keys,3),*type=first_json_string(item,type_keys,1),*validity=json_get_string(item,"validity"),*family_name=json_get_string(item,"family_name"); long long discounted=json_get_ll_any(item,"discounted_price",-1),price=json_get_ll_any(item,"price",-1),original=json_get_ll_any(item,"original_price",-1); if(discounted>0) price=discounted; else if(price<0) price=original; if(!family) family=xstrdup(family_name?family_name:"N/A"); char needle[300]; snprintf(needle,sizeof(needle),"|%s|",family); if(!strstr(printed,needle)){ strncat(printed,needle,sizeof(printed)-strlen(printed)-1); printf("\n%s %s %s%s\n",family_icon(family_name?family_name:family),CB,family,C0); printf("%s-------------------------------------------------------%s\n",CL,C0); } char pricebuf[64]; if(price>=0) money_id(price,pricebuf,sizeof(pricebuf)); else snprintf(pricebuf,sizeof(pricebuf),"N/A"); char key[16]; snprintf(key,sizeof(key),"%d",idx); ql_add(choices,xstrdup(title?title:"N/A"),xstrdup(family),xstrdup(code?code:""),xstrdup(key),xstrdup(type?type:"")); printf("%s%2d%s. %s\n    Family: %s | Price: Rp %s | Validity: %s\n",CB,idx,C0,title?title:"N/A",family_name?family_name:family,pricebuf,validity?validity:"N/A"); idx++; total++; free(family); free(title); free(code); free(type); free(validity); free(family_name); free(item); } printf("%s=======================================================%s\n",CC,C0); return total; }
static int show_store_family_pretty(const char *json,const char *family_code,QuotaList *choices){
	char *data=json_object_dup(json,"data"); if(!data) data=xstrdup(json);
	char *family=json_object_dup(data,"package_family"); if(!family) family=xstrdup("{}");
	char *fname=json_get_string(family,"name"),*ftype=json_get_string(family,"package_family_type"),*rc=json_get_string(family,"rc_bonus_type");
	const char *currency=(rc&&!strcmp(rc,"MYREWARDS"))?"Poin":"Rp";
	const char *start,*end;
	if(json_array_span(data,"package_variants",&start,&end)){
		printf("%sGagal membaca Family Code.%s\n%s\n",CR,C0,json);
		free(data); free(family); free(fname); free(ftype); free(rc);
		return 0;
	}
	printf("%s=======================================================%s\n%s              Custom Family Code%s\n%s=======================================================%s\n",CC,C0,CB,C0,CC,C0);
	printf("Family Name: %s\nFamily Code: %s\nFamily Type: %s\n",fname&&*fname?fname:"-",family_code?family_code:"-",ftype&&*ftype?ftype:"-");
	printf("%s-------------------------------------------------------%s\n",CL,C0);
	const char *p=start; int idx=1,total=0,variant_no=1;
	while(p<end){
		char *variant=next_object(&p,end); if(!variant) break;
		char *vname=json_get_string(variant,"name"),*vcode=json_get_string(variant,"package_variant_code");
		printf("%sVariant %d:%s %s\nCode: %s\n",CB,variant_no++,C0,vname?vname:"-",vcode?vcode:"-");
		const char *os,*oe;
		if(!json_array_span(variant,"package_options",&os,&oe)){
			const char *op=os;
			while(op<oe){
				char *option=next_object(&op,oe); if(!option) break;
				char *oname=json_get_string(option,"name"),*code=json_get_string(option,"package_option_code");
				long long price=json_get_ll_any(option,"price",-1);
				char pricebuf[64],key[16];
				if(price>=0) money_id(price,pricebuf,sizeof(pricebuf)); else snprintf(pricebuf,sizeof(pricebuf),"N/A");
				snprintf(key,sizeof(key),"%d",idx);
				char *title=xasprintf("%s%s%s",vname?vname:"",(vname&&*vname&&oname&&*oname)?" - ":"",oname&&*oname?oname:(code?code:"N/A"));
				ql_add(choices,title,xstrdup(fname&&*fname?fname:(family_code?family_code:"Custom")),xstrdup(code?code:""),xstrdup(key),xstrdup("PDP"));
				printf("%s%2d%s. %s - %s %s\n",CB,idx,C0,oname&&*oname?oname:"N/A",currency,pricebuf);
				idx++; total++;
				free(oname); free(code); free(option);
			}
		}
		printf("%s-------------------------------------------------------%s\n",CL,C0);
		free(vname); free(vcode); free(variant);
	}
	free(data); free(family); free(fname); free(ftype); free(rc);
	printf("%s=======================================================%s\n",CC,C0);
	return total;
}
static int show_store_segments_pretty(const char *json,QuotaList *choices){ const char *start,*end; if(json_array_span(json,"store_segments",&start,&end)){ printf("%sGagal membaca Store Segments.%s\n%s\n",CR,C0,json); return 0; } printf("%s=======================================================%s\n",CC,C0); printf("%s                 Store Segments%s\n",CB,C0); printf("%s=======================================================%s\n",CC,C0); const char *p=start; int si=0,total=0; while(p<end){ char *seg=next_object(&p,end); if(!seg) break; char letter=(char)('A'+si++); char *seg_title=json_get_string(seg,"title"); printf("\n%s%c. %s%s\n",CC,letter,seg_title?seg_title:"Banner",C0); printf("%s-------------------------------------------------------%s\n",CL,C0); const char *bs,*be; if(!json_array_span(seg,"banners",&bs,&be)){ const char *bp=bs; int bi=1; while(bp<be){ char *ban=next_object(&bp,be); if(!ban) break; char *family=json_get_string(ban,"family_name"),*title=json_get_string(ban,"title"),*validity=json_get_string(ban,"validity"),*price=json_get_string(ban,"discounted_price"),*action=json_get_string(ban,"action_param"),*type=json_get_string(ban,"action_type"); long long price_num=json_get_ll(ban,"discounted_price",-1); char price_buf[64],key[16]; if(price) snprintf(price_buf,sizeof(price_buf),"%s",price); else if(price_num>=0) money_id(price_num,price_buf,sizeof(price_buf)); else snprintf(price_buf,sizeof(price_buf),"N/A"); snprintf(key,sizeof(key),"%c%d",(char)tolower((unsigned char)letter),bi); ql_add(choices,xstrdup(title?title:"N/A"),xstrdup(family?family:"N/A"),xstrdup(action?action:""),xstrdup(key),xstrdup(type?type:"")); printf("  %s%c%d%s. %s%s%s - %s\n",CB,letter,bi,C0,CW,family?family:"N/A",C0,title?title:"N/A"); printf("      Price   : Rp%s\n",price_buf); printf("      Validity: %s\n",validity?validity:"N/A"); bi++; total++; free(family); free(title); free(validity); free(price); free(action); free(type); free(ban); } } free(seg_title); free(seg); } printf("%s=======================================================%s\n",CC,C0); return total; }
static int show_redeemables_pretty(const char *json,QuotaList *choices){ const char *start,*end; if(json_array_span(json,"categories",&start,&end)){ printf("%sGagal membaca Point/Redeemables.%s\n%s\n",CR,C0,json); return 0; } printf("%s=======================================================%s\n%s                Point / Redeemables%s\n%s=======================================================%s\n",CC,C0,CB,C0,CC,C0); const char *p=start; int ci=0,total=0; while(p<end){ char *cat=next_object(&p,end); if(!cat) break; char letter=(char)('A'+ci++); char *name=json_get_string(cat,"category_name"),*code=json_get_string(cat,"category_code"); printf("\n%s%c. %s%s\n",CC,letter,name?name:"Category",C0); if(code&&*code) printf("Code: %s\n",code); printf("%s-------------------------------------------------------%s\n",CL,C0); const char *rs,*re; if(!json_array_span(cat,"redeemables",&rs,&re)){ const char *rp=rs; int ri=1; while(rp<re){ char *row=next_object(&rp,re); if(!row) break; char *rname=json_get_string(row,"name"),*action=json_get_string(row,"action_param"),*type=json_get_string(row,"action_type"); long long until=json_get_ll_any(row,"valid_until",0); if(until>9999999999LL) until/=1000; char date[32],key[16]; date_str(until,date,sizeof(date)); snprintf(key,sizeof(key),"%c%d",(char)tolower((unsigned char)letter),ri); ql_add(choices,xstrdup(rname?rname:"N/A"),xstrdup(name?name:"Point"),xstrdup(action?action:""),xstrdup(key),xstrdup(type?type:"")); printf("  %s%c%d%s. %s\n      Valid Until: %s | Action: %s\n",CB,letter,ri,C0,rname?rname:"N/A",date,type?type:"-"); ri++; total++; free(rname); free(action); free(type); free(row); } } else puts("  No redeemables in this category."); free(name); free(code); free(cat); } printf("%s=======================================================%s\n",CC,C0); return total; }
static void payment_quote_print_pretty(PaymentQuote *q){ char pr[64],bal[64]; money_id(q->price,pr,sizeof(pr)); if(q->balance>=0) money_id(q->balance,bal,sizeof(bal)); else snprintf(bal,sizeof(bal),"N/A"); printf("%s-------------------------------------------------------%s\n",CC,C0); printf("%sPaket:%s %s\n",CB,C0,q->item_name); if(q->family&&*q->family) printf("%sFamily:%s %s\n",CB,C0,q->family); if(q->validity&&*q->validity) printf("%sMasa aktif:%s %s\n",CB,C0,q->validity); printf("%sHarga:%s Rp %s\n",CB,C0,pr); printf("%sPulsa:%s Rp %s\n",CB,C0,bal); printf("%s-------------------------------------------------------%s\n",CC,C0); }
static int payment_quote_pretty_load(Accounts *a,Account *acc,Tokens *t,const char *option_code,PaymentQuote *q){ char *err=NULL; if(payment_quote_load_retry(a,acc,t,option_code,q,&err)){ printf("Gagal load quote: %s\n",err?err:"unknown"); free(err); return -1; } payment_quote_print_pretty(q); return 0; }
static int confirm_amount(const char *label,long long total){ char expect[80],choice[96]; snprintf(expect,sizeof(expect),"BAYAR %lld",total); printf("Ketik %s untuk %s: ",expect,label); fflush(stdout); if(!fgets(choice,sizeof(choice),stdin)) choice[0]=0; trim(choice); if(strcmp(choice,expect)){ puts("Dibatalkan."); return -1; } return 0; }
static long long custom_amount_or_prompt(long long total,long long custom_amount){ if(custom_amount>0) return custom_amount; char rp[64],choice[64]; money_id(total,rp,sizeof(rp)); printf("Custom price (Enter = Rp %s): ",rp); fflush(stdout); if(!fgets(choice,sizeof(choice),stdin)) return total; trim(choice); if(!choice[0]) return total; long long amount=0; if(parse_amount_arg(choice,&amount)){ puts("Custom price tidak valid. Pakai default."); return total; } return amount; }
static int print_payment_response(const char *resp){ char *status=json_get_string(resp,"status"),*msg=json_get_string(resp,"message"),*deeplink=json_get_string(resp,"deeplink"); int ok=payment_ok_status(status); printf("Result: %s%s%s\n",status?status:"UNKNOWN",msg?" - ":"",msg?msg:""); if(deeplink&&*deeplink) printf("Link pembayaran:\n%s\n",deeplink); if(!ok) printf("%s\n",resp); free(status); free(msg); free(deeplink); return ok; }
static const char *prompt_ewallet(char wallet[32]){ wallet[0]=0; char choice[16]; puts("E-Wallet:\n1. DANA\n2. ShopeePay\n3. GoPay"); prompt_line("Pilihan: ",choice,sizeof(choice)); if(!strcmp(choice,"1")){ char raw[32],*norm; prompt_line("Nomor DANA (08xxxx / 628xxxx): ",raw,sizeof(raw)); norm=wallet_api_number(raw); if(!starts_with(norm,"08")||strlen(norm)<10||strlen(norm)>13){ puts("Nomor DANA tidak valid."); free(norm); return ""; } snprintf(wallet,32,"%s",norm); free(norm); return "DANA"; } if(!strcmp(choice,"2")) return "SHOPEEPAY"; if(!strcmp(choice,"3")) return "GOPAY"; puts("Pilihan tidak valid."); return ""; }
static int interactive_payment_qris_quotes(Accounts *a,Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,long long custom_amount){ long long amount=custom_amount_or_prompt(total,custom_amount); if(confirm_amount("QRIS",amount)) return 0; char *resp=payment_qris_settle_many_ex(acc,t,q,n,amount,0,NULL); char *status=json_get_string(resp,"status"),*msg=json_get_string(resp,"message"),*tx=json_get_string(resp,"transaction_code"); int ok=payment_ok_status(status); printf("Result: %s%s%s\n",status?status:"UNKNOWN",msg?" - ":"",msg?msg:""); if(tx&&*tx){ char *pending=get_transaction_status_retry(a,acc,t,tx,""),*qr=json_get_string(pending,"qr_code"); printf("Transaction: %s\n",tx); if(qr&&*qr){ char *b64=b64_encode((unsigned char *)qr,strlen(qr),1); char *url=xasprintf("http://192.168.1.1/qrcode.php?data=%s",b64); printf("QRIS URL:\n%s\nQRIS data:\n%s\n",url,qr); free(url); free(b64); } else printf("Pending detail:\n%s\n",pending); free(pending); free(qr); } if(!ok) printf("%s\n",resp); free(status); free(msg); free(tx); free(resp); return ok; }
static int interactive_payment_ewallet_quotes(Accounts *a,Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,long long custom_amount){ char wallet[32],*resp; const char *method=prompt_ewallet(wallet); if(!*method) return 0; long long amount=custom_amount_or_prompt(total,custom_amount); if(confirm_amount(method,amount)) return 0; resp=payment_ewallet_settle_many(acc,t,q,n,amount,method,wallet); int ok=print_payment_response(resp); free(resp); return ok; }
static int interactive_payment_decoy_quotes(Accounts *a,Account *acc,Tokens *t,PaymentQuote *q,int n,long long total,long long custom_amount,int standard){
	const char *label=standard?"Balance + Decoy":"Balance + Decoy V2";
	if(n>3){ printf("%s max 3 paket.\n",label); return 0; }
	char *err=NULL,*decoy=NULL; int decoy_refreshed=0;
	if(decoy_quote_resolve(a,acc,t,&q[n],&decoy,&err,&decoy_refreshed)){
		printf("Decoy gagal sebelum settlement: %s\n",err?err:"resolve/validation failed");
		free(err); free(decoy); return 0;
	}
	free(decoy);
	if(decoy_refreshed&&payment_quotes_reload_current(t,q,n,&total,&err)){
		printf("Target quote gagal dimuat ulang setelah refresh decoy: %s\n",err?err:"unknown");
		free(err); payment_quote_free(&q[n]); return 0;
	}
	if(q[n].price>LLONG_MAX-total){ puts("Decoy gagal: payment amount overflow"); payment_quote_free(&q[n]); return 0; }
	total+=q[n].price;
	long long amount=custom_amount_or_prompt(total,custom_amount);
	if(confirm_amount(label,amount)){ payment_quote_free(&q[n]); return 0; }
	int token_idx=standard?0:n;
	const char *payment_for_override=standard?NULL:DECOY_V2_PAYMENT_FOR;
	char *resp=payment_pulsa_settle_retry(acc,t,q,n+1,&amount,token_idx,payment_for_override,NULL);
	int ok=print_payment_response(resp);
	free(resp); payment_quote_free(&q[n]); return ok;
}
static int interactive_payment_special_quote(Accounts *a,Account *acc,Tokens *t,PaymentQuote *q,const char *mode){ if(strcmp(q->payment_for?q->payment_for:"","REDEEM_VOUCHER")){ puts("Mode ini hanya untuk REDEEM_VOUCHER."); return 0; } char *dest=NULL; if(!strcmp(mode,"gift")){ char raw[32]; prompt_line("Nomor tujuan (628xxxx / 08xxxx): ",raw,sizeof(raw)); dest=dest_msisdn(raw); if(!msisdn_ok(dest)){ puts("Nomor tujuan tidak valid."); free(dest); return 0; } } char expect[80],choice[96]; snprintf(expect,sizeof(expect),"AMBIL %s",mode); printf("Ketik %s untuk lanjut: ",expect); fflush(stdout); if(!fgets(choice,sizeof(choice),stdin)) choice[0]=0; trim(choice); if(strcmp(choice,expect)){ puts("Dibatalkan."); free(dest); return 0; } char *resp=!strcmp(mode,"point")?payment_point_settle(acc,t,q):(!strcmp(mode,"voucher")?payment_voucher_settle(acc,t,q):payment_gift_settle(acc,t,q,dest)); int ok=print_payment_response(resp); free(dest); free(resp); return ok; }
static void interactive_payment_decoy(Accounts *a,Account *acc,Tokens *t,const char *option_code,int standard){ const char *missing=payment_config_missing(); if(missing){ printf("Missing env %s. Set di Settings dulu.\n",missing); pause_enter(); return; } PaymentQuote q[2]; memset(q,0,sizeof(q)); if(payment_quote_pretty_load(a,acc,t,option_code,&q[0])){ pause_enter(); return; } interactive_payment_decoy_quotes(a,acc,t,q,1,q[0].price,-1,standard); payment_quote_free(&q[0]); pause_enter(); }
static void interactive_payment_qris(Accounts *a,Account *acc,Tokens *t,const char *option_code){ const char *missing=payment_config_missing(); if(missing){ printf("Missing env %s. Set di Settings dulu.\n",missing); pause_enter(); return; } PaymentQuote q={0}; if(payment_quote_pretty_load(a,acc,t,option_code,&q)){ pause_enter(); return; } interactive_payment_qris_quotes(a,acc,t,&q,1,q.price,-1); payment_quote_free(&q); pause_enter(); }
static void interactive_payment_ewallet(Accounts *a,Account *acc,Tokens *t,const char *option_code){ const char *missing=payment_config_missing(); if(missing){ printf("Missing env %s. Set di Settings dulu.\n",missing); pause_enter(); return; } PaymentQuote q={0}; if(payment_quote_pretty_load(a,acc,t,option_code,&q)){ pause_enter(); return; } interactive_payment_ewallet_quotes(a,acc,t,&q,1,q.price,-1); payment_quote_free(&q); pause_enter(); }
static void interactive_payment_special(Accounts *a,Account *acc,Tokens *t,const char *option_code,const char *mode){ const char *missing=payment_config_missing(); if(missing){ printf("Missing env %s. Set di Settings dulu.\n",missing); pause_enter(); return; } PaymentQuote q={0}; if(payment_quote_pretty_load(a,acc,t,option_code,&q)){ pause_enter(); return; } interactive_payment_special_quote(a,acc,t,&q,mode); payment_quote_free(&q); pause_enter(); }
static void interactive_payment_pulsa(Accounts *a,Account *acc,Tokens *t,const char *option_code){ const char *missing=payment_config_missing(); if(missing){ printf("Missing env %s. Set di Settings dulu.\n",missing); pause_enter(); return; } PaymentQuote q={0}; if(payment_quote_pretty_load(a,acc,t,option_code,&q)){ pause_enter(); return; } long long amount=custom_amount_or_prompt(q.price,-1); char expect[80],choice[96]; snprintf(expect,sizeof(expect),"BAYAR %lld",amount); printf("Ketik %s untuk beli dengan Pulsa: ",expect); fflush(stdout); if(!fgets(choice,sizeof(choice),stdin)) choice[0]=0; trim(choice); if(strcmp(choice,expect)){ puts("Dibatalkan."); payment_quote_free(&q); pause_enter(); return; } char *resp=payment_pulsa_settle_many_once(a,acc,t,&q,1,amount); char *status=json_get_string(resp,"status"),*msg=json_get_string(resp,"message"); printf("Result: %s%s%s\n",status?status:"UNKNOWN",msg?" - ":"",msg?msg:""); free(status); free(msg); free(resp); payment_quote_free(&q); pause_enter(); }
static void interactive_store_segments(void){ char ent[8]; prompt_line("Enterprise store? (y/N): ",ent,sizeof(ent)); int is_enterprise=(ent[0]=='y'||ent[0]=='Y'); for(;;){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ puts("No active user. Pilih/login akun dulu."); free(act); accounts_free(&a); pause_enter(); return; } Tokens t={0}; if(refresh_account(acc,&t)){ puts("Gagal refresh token."); free(act); accounts_free(&a); pause_enter(); return; } account_sync(&a,acc,&t); char *segments=get_store_segments_retry(&a,acc,&t,is_enterprise); QuotaList choices={0}; printf("\033[H\033[J"); show_store_segments_pretty(segments,&choices); printf("%sCommand:%s A1/B2 lihat action, 00/Enter kembali\n",CB,C0); char choice[32]; prompt_line("Pilihan: ",choice,sizeof(choice)); for(size_t i=0;i<strlen(choice);i++) choice[i]=(char)tolower((unsigned char)choice[i]); if(!choice[0]||!strcmp(choice,"00")){ ql_free(&choices); free(segments); tokens_free(&t); free(act); accounts_free(&a); return; } QuotaPkg *picked=NULL; for(size_t i=0;i<choices.n;i++) if(!strcmp(choices.v[i].domain,choice)){ picked=&choices.v[i]; break; } if(!picked){ puts("Pilihan tidak valid."); pause_enter(); } else { printf("\n%s%s%s\nFamily : %s\nAction : %s\nCode   : %s\n",CB,picked->name,C0,picked->group,picked->subtype,picked->code); if(!strcmp(picked->subtype,"PDP")) interactive_payment_pulsa(&a,acc,&t,picked->code); else { puts("Action type belum ditangani."); pause_enter(); } } ql_free(&choices); free(segments); tokens_free(&t); free(act); accounts_free(&a); } }
static void interactive_store_packages(void){ char ent[8]; prompt_line("Enterprise store? (y/N): ",ent,sizeof(ent)); int is_enterprise=(ent[0]=='y'||ent[0]=='Y'); CartItem cart[3]; int cart_n=0; for(;;){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ puts("No active user. Pilih/login akun dulu."); free(act); accounts_free(&a); pause_enter(); return; } Tokens t={0}; if(refresh_account(acc,&t)){ puts("Gagal refresh token."); free(act); accounts_free(&a); pause_enter(); return; } account_sync(&a,acc,&t); char *shop=get_store_packages_retry(&a,acc,&t,is_enterprise); QuotaList choices={0}; printf("\033[H\033[J"); show_store_packages_pretty(shop,&choices); printf("%sCart:%s %d/3 | Command: nomor detail, c checkout, r reset cart, 00 kembali\n",CB,C0,cart_n); char choice[32]; prompt_line("Pilihan: ",choice,sizeof(choice)); for(size_t i=0;i<strlen(choice);i++) choice[i]=(char)tolower((unsigned char)choice[i]); if(!choice[0]||!strcmp(choice,"00")){ ql_free(&choices); free(shop); tokens_free(&t); free(act); accounts_free(&a); return; } if(!strcmp(choice,"c")){ checkout_cart_pulsa(&a,acc,&t,cart,&cart_n); } else if(!strcmp(choice,"r")){ cart_n=0; puts("Cart kosong."); pause_enter(); } else { QuotaPkg *picked=NULL; for(size_t i=0;i<choices.n;i++) if(!strcmp(choices.v[i].domain,choice)){ picked=&choices.v[i]; break; } if(!picked) { puts("Pilihan tidak valid."); pause_enter(); } else if(strcmp(picked->subtype,"PDP")) { puts("Action type belum ditangani."); pause_enter(); } else show_package_detail_cli(&a,acc,&t,picked->code,cart,&cart_n); } ql_free(&choices); free(shop); tokens_free(&t); free(act); accounts_free(&a); } }
static void interactive_store_family_code(const char *family_code,const char *enterprise_arg){ CartItem cart[3]; int cart_n=0; for(;;){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ puts("No active user. Pilih/login akun dulu."); free(act); accounts_free(&a); pause_enter(); return; } Tokens t={0}; if(refresh_account(acc,&t)){ puts("Gagal refresh token."); free(act); accounts_free(&a); pause_enter(); return; } account_sync(&a,acc,&t); char *shop=get_store_family_retry(&a,acc,&t,family_code,enterprise_arg?enterprise_arg:""); QuotaList choices={0}; printf("\033[H\033[J"); show_store_family_pretty(shop,family_code,&choices); printf("%sCart:%s %d/3 | Command: nomor detail, c checkout, r reset cart, 00 kembali\n",CB,C0,cart_n); char choice[32]; prompt_line("Pilihan: ",choice,sizeof(choice)); for(size_t i=0;i<strlen(choice);i++) choice[i]=(char)tolower((unsigned char)choice[i]); if(!choice[0]||!strcmp(choice,"00")){ ql_free(&choices); free(shop); tokens_free(&t); free(act); accounts_free(&a); return; } if(!strcmp(choice,"c")){ checkout_cart_pulsa(&a,acc,&t,cart,&cart_n); } else if(!strcmp(choice,"r")){ cart_n=0; puts("Cart kosong."); pause_enter(); } else { QuotaPkg *picked=NULL; for(size_t i=0;i<choices.n;i++) if(!strcmp(choices.v[i].domain,choice)){ picked=&choices.v[i]; break; } if(!picked) { puts("Pilihan tidak valid."); pause_enter(); } else if(strcmp(picked->subtype,"PDP")) { puts("Action type belum ditangani."); pause_enter(); } else show_package_detail_cli(&a,acc,&t,picked->code,cart,&cart_n); } ql_free(&choices); free(shop); tokens_free(&t); free(act); accounts_free(&a); } }
static void interactive_store_family(void){ char family_code[160]; prompt_line("Family code: ",family_code,sizeof(family_code)); if(!family_code_ok(family_code)){ puts("Family code tidak valid."); pause_enter(); return; } interactive_store_family_code(family_code,""); }
static void interactive_redeemables(const char *enterprise_arg){ int is_enterprise=arg_true(enterprise_arg); CartItem cart[3]; int cart_n=0; for(;;){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc){ puts("No active user. Pilih/login akun dulu."); free(act); accounts_free(&a); pause_enter(); return; } Tokens t={0}; if(refresh_account(acc,&t)){ puts("Gagal refresh token."); free(act); accounts_free(&a); pause_enter(); return; } account_sync(&a,acc,&t); char *redeemables=get_redeemables_retry(&a,acc,&t,is_enterprise); QuotaList choices={0}; printf("\033[H\033[J"); show_redeemables_pretty(redeemables,&choices); printf("%sCommand:%s A1/B2 buka action, 00/Enter kembali\n",CB,C0); char choice[32]; prompt_line("Pilihan: ",choice,sizeof(choice)); for(size_t i=0;i<strlen(choice);i++) choice[i]=(char)tolower((unsigned char)choice[i]); if(!choice[0]||!strcmp(choice,"00")){ ql_free(&choices); free(redeemables); tokens_free(&t); free(act); accounts_free(&a); return; } QuotaPkg *picked=NULL; for(size_t i=0;i<choices.n;i++) if(!strcmp(choices.v[i].domain,choice)){ picked=&choices.v[i]; break; } if(!picked){ puts("Pilihan tidak valid."); pause_enter(); } else { char type[16]; snprintf(type,sizeof(type),"%s",picked->subtype); for(size_t i=0;type[i];i++) type[i]=(char)toupper((unsigned char)type[i]); if(!strcmp(type,"PDP")) show_package_detail_cli(&a,acc,&t,picked->code,cart,&cart_n); else if(!strcmp(type,"PLP")){ char ent[4],code[160]; snprintf(ent,sizeof(ent),"%d",is_enterprise); snprintf(code,sizeof(code),"%s",picked->code); ql_free(&choices); free(redeemables); tokens_free(&t); free(act); accounts_free(&a); interactive_store_family_code(code,ent); continue; } else { printf("Action type belum ditangani: %s\n",picked->subtype); pause_enter(); } } ql_free(&choices); free(redeemables); tokens_free(&t); free(act); accounts_free(&a); } }
static void cmd_shop(const char *enterprise_arg){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc) die("no active account. run: bitsxl use <number>"); Tokens t={0}; if(refresh_account(acc,&t)) exit(1); account_sync(&a,acc,&t); char *shop=get_store_packages_retry(&a,acc,&t,arg_true(enterprise_arg)); QuotaList choices={0}; show_store_packages_pretty(shop,&choices); ql_free(&choices); free(shop); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_segments(const char *enterprise_arg){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc) die("no active account. run: bitsxl use <number>"); Tokens t={0}; if(refresh_account(acc,&t)) exit(1); account_sync(&a,acc,&t); char *segments=get_store_segments_retry(&a,acc,&t,arg_true(enterprise_arg)); QuotaList choices={0}; show_store_segments_pretty(segments,&choices); ql_free(&choices); free(segments); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_pay_quote(const char *option_code){ Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc) die("no active account. run: bitsxl use <number>"); Tokens t={0}; if(refresh_account(acc,&t)) exit(1); account_sync(&a,acc,&t); PaymentQuote q={0}; if(!payment_quote_pretty_load(&a,acc,&t,option_code,&q)) payment_quote_free(&q); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_pay_pulsa(const char *option_code,const char *confirm,long long custom_amount){ const char *missing=payment_config_missing(); if(missing) die("missing env %s",missing); Accounts a=accounts_load(); char *act=active_get(); Account *acc=(act&&*act)?accounts_find(&a,act):NULL; if(!acc) die("no active account. run: bitsxl use <number>"); Tokens t={0}; if(refresh_account(acc,&t)) exit(1); account_sync(&a,acc,&t); PaymentQuote q={0}; if(payment_quote_pretty_load(&a,acc,&t,option_code,&q)){ tokens_free(&t); free(act); accounts_free(&a); return; } long long amount=payment_amount(q.price,custom_amount); char expect[80]; snprintf(expect,sizeof(expect),"BAYAR-%lld",amount); if(!confirm||strcmp(confirm,expect)) die("confirm required: bitsxl pay-pulsa %s %s",option_code,expect); char *resp=payment_pulsa_settle_many_once(&a,acc,&t,&q,1,amount); puts(resp); free(resp); payment_quote_free(&q); tokens_free(&t); free(act); accounts_free(&a); }
static int cli_begin(Accounts *a,char **act,Account **acc,Tokens *t){ *a=accounts_load(); *act=active_get(); *acc=(*act&&**act)?accounts_find(a,*act):NULL; if(!*acc){ fprintf(stderr,"no active account. run: bitsxl use <number>\n"); free(*act); accounts_free(a); return -1; } if(refresh_account(*acc,t)){ fprintf(stderr,"refresh failed: %s\n",(*acc)->number); free(*act); accounts_free(a); return -1; } account_sync(a,*acc,t); return 0; }
static char *repeat_family_option_code(Accounts *a,Account *acc,Tokens *t,const char *family,const char *variant,int order,char **err){
	*err=NULL;
	char *code=find_family_option_code(t,family,variant,order,"",err);
	if(!code&&*err&&api_auth_failed(*err)){
		char *auth_err=*err; *err=NULL;
		if(!api_retry_auth(acc,a,t)){ free(auth_err); code=find_family_option_code(t,family,variant,order,"",err); }
		else *err=auth_err;
	}
	return code;
}
static void repeat_prefix_item_name(PaymentQuote *q){
	unsigned char random[2]; random_bytes(random,sizeof(random));
	unsigned int prefix=1000U+((((unsigned int)random[0]<<8)|random[1])%9000U);
	char *name=xasprintf("%u %s",prefix,q->item_name?q->item_name:"");
	free(q->item_name); q->item_name=name;
}
static int repeat_purchase_run(const char *family,const char *variant,int order,const char *option_code,const RepeatPurchaseOptions *opts,int decoy_confirmed){
	Accounts a={0}; char *act=NULL; Account *acc=NULL; Tokens t={0};
	if(cli_begin(&a,&act,&acc,&t)) return 1;
	PaymentQuote preflight_q[2]; memset(preflight_q,0,sizeof(preflight_q));
	char *preflight_target=NULL,*preflight_decoy=NULL,*preflight_err=NULL;
	int preflight_ok=0,preflight_cancelled=0;
	if(option_code) preflight_target=xstrdup(option_code);
	else preflight_target=repeat_family_option_code(&a,acc,&t,family,variant,order,&preflight_err);
	if(!preflight_target){ fprintf(stderr,"repeat preflight failed: %s\n",preflight_err?preflight_err:"target family/variant/order not found"); goto repeat_preflight_done; }
	int preflight_decoy_refreshed=0;
	if(payment_quote_load_retry_track(&a,acc,&t,preflight_target,&preflight_q[0],&preflight_err,NULL)){
		fprintf(stderr,"repeat preflight failed: %s\n",preflight_err?preflight_err:"target quote failed"); goto repeat_preflight_done;
	}
	if(opts->use_decoy&&decoy_quote_resolve(&a,acc,&t,&preflight_q[1],&preflight_decoy,&preflight_err,&preflight_decoy_refreshed)){
		fprintf(stderr,"repeat preflight failed before settlement: %s\n",preflight_err?preflight_err:"decoy resolve/validation failed"); goto repeat_preflight_done;
	}
	if(opts->use_decoy&&preflight_decoy_refreshed){
		payment_quote_free(&preflight_q[0]);
		if(payment_quote_load(&t,preflight_target,&preflight_q[0],&preflight_err)){
			fprintf(stderr,"repeat preflight failed reloading target after decoy refresh: %s\n",preflight_err?preflight_err:"target quote failed"); goto repeat_preflight_done;
		}
	}
	if(opts->use_decoy){
		char decoy_price[64],answer[16]; money_id(preflight_q[1].price,decoy_price,sizeof(decoy_price));
		printf("Pastikan sisa balance KURANG DARI Rp %s (harga decoy).\n",decoy_price);
		if(!decoy_confirmed){
			prompt_line("Lanjutkan repeat purchase dengan decoy? (y/N): ",answer,sizeof(answer));
			if(answer[0]!='y'&&answer[0]!='Y'){ puts("Repeat purchase dibatalkan."); preflight_cancelled=1; goto repeat_preflight_done; }
		}
	}
	preflight_ok=1;
repeat_preflight_done:
	free(preflight_target); free(preflight_decoy); free(preflight_err);
	payment_quote_free(&preflight_q[0]); payment_quote_free(&preflight_q[1]);
	if(!preflight_ok){ tokens_free(&t); free(act); accounts_free(&a); return preflight_cancelled?0:1; }
	const char *token_source=(opts->use_decoy&&opts->token_idx==1)?"decoy":"target";
	printf("Repeat purchase: %d time(s) | decoy=%s | delay=%u second(s) | token_confirmation_idx=%d (selected=%d, source=%s) | payment_for=%s\n",opts->count,opts->use_decoy?"yes":"no",opts->delay_seconds,opts->token_idx_raw,opts->token_idx,token_source,DECOY_V2_PAYMENT_FOR);
	if(option_code) printf("Target option_code: %s\n",option_code);
	else printf("Target family=%s variant=%s order=%d\n",family,variant,order);
	int success=0,failed=0; StrList successful={0};
	for(int round=0;round<opts->count;round++){
		PaymentQuote q[2]; memset(q,0,sizeof(q));
		char *target_code=NULL,*decoy_code=NULL,*display_name=NULL,*err=NULL,*reason=NULL,*resp=NULL,*first=NULL,*status=NULL,*msg=NULL;
		long long amount=0; int round_ok=0;
		tokens_free(&t);
		if(refresh_account(acc,&t)){ reason=xstrdup("active token retrieval failed"); goto repeat_round_done; }
		account_sync(&a,acc,&t);
		if(option_code) target_code=xstrdup(option_code);
		else target_code=repeat_family_option_code(&a,acc,&t,family,variant,order,&err);
		if(!target_code){ reason=err?err:xstrdup("target family/variant/order not found"); err=NULL; goto repeat_round_done; }
		int decoy_refreshed=0;
		if(payment_quote_load_retry_track(&a,acc,&t,target_code,&q[0],&err,NULL)){
			reason=err?err:xstrdup("target quote failed"); err=NULL; goto repeat_round_done;
		}
		if(opts->use_decoy&&decoy_quote_resolve(&a,acc,&t,&q[1],&decoy_code,&err,&decoy_refreshed)){
			reason=err?err:xstrdup("decoy resolve/validation failed before settlement"); err=NULL; goto repeat_round_done;
		}
		if(opts->use_decoy&&decoy_refreshed){
			payment_quote_free(&q[0]);
			if(payment_quote_load(&t,target_code,&q[0],&err)){
				reason=err?err:xstrdup("target quote reload failed after decoy refresh"); err=NULL; goto repeat_round_done;
			}
		}
		amount=q[0].price;
		if(opts->use_decoy){
			if(q[1].price>LLONG_MAX-amount){ reason=xstrdup("payment amount overflow"); goto repeat_round_done; }
			amount+=q[1].price;
		}
		display_name=xstrdup(q[0].item_name);
		repeat_prefix_item_name(&q[0]);
		if(opts->use_decoy) repeat_prefix_item_name(&q[1]);
		resp=payment_pulsa_settle_retry(acc,&t,q,opts->use_decoy?2:1,&amount,opts->token_idx,DECOY_V2_PAYMENT_FOR,&first);
		status=resp?json_get_string(resp,"status"):NULL;
		msg=resp?json_get_string(resp,"message"):NULL;
		round_ok=payment_ok_status(status);
		if(!round_ok) reason=xstrdup(msg?msg:(status?status:((resp&&*resp)?"unparseable payment response":"empty payment response")));
repeat_round_done:
		if(round_ok){
			success++;
			sl_add(&successful,xasprintf("%s | amount=%lld",display_name?display_name:target_code,amount));
			printf("[%d/%d] SUCCESS - %s | amount=%lld%s%s\n",round+1,opts->count,display_name?display_name:target_code,amount,msg?" | ":"",msg?msg:"");
		} else {
			failed++;
			printf("[%d/%d] FAILED - %s\n",round+1,opts->count,reason?reason:"unknown error");
			if(resp) printf("  Response: %s\n",resp);
		}
		if(first) printf("  Bizz-err.Amount.Total adjusted amount to %lld and retried with token index %d.\n",amount,opts->token_idx);
		free(target_code); free(decoy_code); free(display_name); free(err); free(reason); free(resp); free(first); free(status); free(msg);
		payment_quote_free(&q[0]); payment_quote_free(&q[1]);
		if(opts->delay_seconds&&round+1<opts->count){
			printf("Waiting for %u seconds before next purchase...\n",opts->delay_seconds);
			sleep(opts->delay_seconds);
		}
	}
	printf("Repeat purchase summary: requested=%d success=%d failed=%d\n",opts->count,success,failed);
	if(successful.n){ puts("Successful purchases:"); for(size_t i=0;i<successful.n;i++) printf("%zu. %s\n",i+1,successful.v[i]); }
	free_sl(&successful);
	tokens_free(&t); free(act); accounts_free(&a);
	return failed?1:0;
}
static int cmd_purchase_n_times(int argc,char **argv){
	if(argc<4) die("usage: bitsxl purchase-n-times <family_code> <variant_code> <order> <count> [--delay seconds] [--use-decoy y|n] [--token-confirmation-idx idx]");
	if(!purchase_code_ok(argv[0])||!purchase_code_ok(argv[1])) die("invalid family_code or variant_code");
	long long order=0,count=0;
	if(parse_bounded_ll(argv[2],0,INT_MAX,&order)) die("invalid order: %s",argv[2]);
	if(parse_bounded_ll(argv[3],1,INT_MAX,&count)) die("invalid count: %s",argv[3]);
	RepeatPurchaseOptions opts; parse_repeat_purchase_options(argc-4,argv+4,(int)count,&opts);
	const char *missing=payment_config_missing(); if(missing) die("missing env %s",missing);
	return repeat_purchase_run(argv[0],argv[1],(int)order,NULL,&opts,0);
}
static int cmd_purchase_n_times_by_option_code(int argc,char **argv){
	if(argc<2) die("usage: bitsxl purchase-n-times-by-option-code <option_code> <count> [--delay seconds] [--use-decoy y|n] [--token-confirmation-idx idx]");
	if(!purchase_code_ok(argv[0])) die("invalid option_code");
	long long count=0; if(parse_bounded_ll(argv[1],1,INT_MAX,&count)) die("invalid count: %s",argv[1]);
	RepeatPurchaseOptions opts; parse_repeat_purchase_options(argc-2,argv+2,(int)count,&opts);
	const char *missing=payment_config_missing(); if(missing) die("missing env %s",missing);
	return repeat_purchase_run(NULL,NULL,0,argv[0],&opts,0);
}
static void interactive_purchase_n_times_option(const char *option_code){
	const char *missing=payment_config_missing();
	if(missing){ printf("Missing env %s. Set di Settings dulu.\n",missing); pause_enter(); return; }
	if(!purchase_code_ok(option_code)){ puts("Option Code target tidak valid."); pause_enter(); return; }
	char answer[32],input[64]; long long parsed=0;
	RepeatPurchaseOptions opts; memset(&opts,0,sizeof(opts));
	prompt_line("Use decoy package? (y/n): ",answer,sizeof(answer));
	if(parse_bool_strict(answer,&opts.use_decoy)) goto repeat_option_invalid;
	prompt_line("Enter number of times to purchase: ",input,sizeof(input));
	if(parse_bounded_ll(input,1,INT_MAX,&parsed)) goto repeat_option_invalid;
	opts.count=(int)parsed;
	prompt_line("Enter delay between purchases in seconds: ",input,sizeof(input));
	if(parse_bounded_ll(input,0,UINT_MAX,&parsed)) goto repeat_option_invalid;
	opts.delay_seconds=(unsigned int)parsed;
	opts.token_idx_raw=opts.use_decoy?1:0;
	opts.token_idx=opts.token_idx_raw;
	printf("%s-------------------------------------------------------%s\n",CL,C0);
	printf("Pulsa N Kali\nTarget Option Code : %s\nJumlah Pembelian  : %d\nDelay              : %u detik\nUse Decoy          : %s\nToken Source       : %s\nPayment For        : %s\n",option_code,opts.count,opts.delay_seconds,opts.use_decoy?"Ya":"Tidak",opts.use_decoy?"decoy":"target",DECOY_V2_PAYMENT_FOR);
	char expected[64]; snprintf(expected,sizeof(expected),"BELI %d KALI",opts.count);
	printf("Ketik %s untuk konfirmasi: ",expected); fflush(stdout);
	if(!fgets(answer,sizeof(answer),stdin)) answer[0]=0;
	trim(answer);
	if(strcmp(answer,expected)){ puts("Pulsa N Kali dibatalkan."); pause_enter(); return; }
	repeat_purchase_run(NULL,NULL,0,option_code,&opts,0);
	pause_enter();
	return;
repeat_option_invalid:
	puts("Input Pulsa N Kali tidak valid.");
	pause_enter();
}
static int family_purchase_options_load(const char *response,const char *family_code,QuotaList *options,char **family_name,char **err){
	if(err) *err=NULL;
	if(family_name) *family_name=NULL;
	char *data=json_object_dup(response,"data"); if(!data) data=xstrdup(response);
	char *family=json_object_dup(data,"package_family"); if(!family) family=xstrdup("{}");
	char *name=json_get_string(family,"name");
	const char *vs,*ve;
	if(json_array_span(data,"package_variants",&vs,&ve)){
		if(err) *err=xasprintf("target family %s gagal dimuat: %s",family_code,response);
		free(data); free(family); free(name);
		return -1;
	}
	int number=0; const char *vp=vs;
	while(vp<ve){
		char *variant=next_object(&vp,ve); if(!variant) break;
		char *variant_name=json_get_string(variant,"name"),*variant_code=json_get_string(variant,"package_variant_code");
		const char *os,*oe;
		if(!json_array_span(variant,"package_options",&os,&oe)){
			const char *op=os;
			while(op<oe){
				char *option=next_object(&op,oe); if(!option) break;
				char *option_name=json_get_string(option,"name"),*option_code=json_get_string(option,"package_option_code");
				long long option_order=json_get_ll_any(option,"order",-1);
				number++;
				if(!option_code||!*option_code||option_order<0||option_order>INT_MAX){
					if(err) *err=xasprintf("target family %s memiliki package option tidak valid pada nomor %d",family_code,number);
					free(option_name); free(option_code); free(option); free(variant_name); free(variant_code); free(variant); free(data); free(family); free(name); ql_free(options);
					return -1;
				}
				char number_text[32]; snprintf(number_text,sizeof(number_text),"%lld",option_order);
				ql_add(options,option_name?option_name:xstrdup(option_code),xstrdup(variant_name?variant_name:""),option_code,xstrdup(number_text),xstrdup(variant_code?variant_code:""));
				free(option);
			}
		}
		free(variant_name); free(variant_code); free(variant);
	}
	free(data); free(family);
	if(!options->n){
		if(err) *err=xasprintf("target family %s tidak memiliki package option",family_code);
		free(name);
		return -1;
	}
	if(family_name) *family_name=name?name:xstrdup(family_code); else free(name);
	return 0;
}
static int family_purchase_start_index(const QuotaList *options,int start_option,size_t *index_out){
	if(!options||!options->n||!index_out) return -1;
	if(start_option<=1){ *index_out=0; return 0; }
	for(size_t i=0;i<options->n;i++){
		long long option_order=0;
		if(!parse_bounded_ll(options->v[i].domain,0,INT_MAX,&option_order)&&option_order==start_option){ *index_out=i; return 0; }
	}
	return -1;
}
static int family_purchase_run(const char *family_code,const FamilyPurchaseOptions *opts){
	Accounts accounts={0}; char *active=NULL; Account *acc=NULL; Tokens tokens={0};
	if(cli_begin(&accounts,&active,&acc,&tokens)) return 1;
	char *family_response=get_store_family_retry(&accounts,acc,&tokens,family_code,"");
	QuotaList options={0}; char *family_name=NULL,*err=NULL;
	if(family_purchase_options_load(family_response,family_code,&options,&family_name,&err)){
		fprintf(stderr,"purchase-by-family preflight failed: %s\n",err?err:"family lookup failed");
		free(err); free(family_response); tokens_free(&tokens); free(active); accounts_free(&accounts); return 1;
	}
	free(family_response);
	size_t start_index=0;
	if(family_purchase_start_index(&options,opts->start_option,&start_index)){
		fprintf(stderr,"purchase-by-family preflight failed: option order %d tidak tersedia pada family target\n",opts->start_option);
		ql_free(&options); free(family_name); tokens_free(&tokens); free(active); accounts_free(&accounts); return 1;
	}
	PaymentQuote preflight_decoy={0}; char *preflight_code=NULL;
	if(opts->use_decoy&&decoy_quote_resolve(&accounts,acc,&tokens,&preflight_decoy,&preflight_code,&err,NULL)){
		fprintf(stderr,"purchase-by-family preflight failed before settlement: %s\n",err?err:"decoy resolve/validation failed");
		free(err); free(preflight_code); ql_free(&options); free(family_name); tokens_free(&tokens); free(active); accounts_free(&accounts); return 1;
	}
	if(opts->use_decoy){
		char price[64]; money_id(preflight_decoy.price,price,sizeof(price));
		printf("Pastikan sisa balance KURANG DARI Rp %s (harga decoy).\n",price);
	}
	free(preflight_code); payment_quote_free(&preflight_decoy);
	printf("%s-------------------------------------------------------%s\n",CL,C0);
	printf("Beli Semua Paket dalam Family Code (Loop)\nFamily Name       : %s\nFamily Code       : %s\nTotal Paket       : %zu\nMulai Option Order: %d\nJumlah Diproses   : %zu\nUse Decoy         : %s\nPause Saat Sukses : %s\nDelay             : %u detik\nPayment For Awal : %s\nBizz Retry        : SHARE_PACKAGE\n",family_name,family_code,options.n,opts->start_option,options.n-start_index,opts->use_decoy?"Ya":"Tidak",opts->pause_on_success?"Ya":"Tidak",opts->delay_seconds,FAMILY_LOOP_PAYMENT_FOR);
	char confirm[32]; prompt_line("Ketik BELI SEMUA untuk konfirmasi: ",confirm,sizeof(confirm));
	if(strcmp(confirm,"BELI SEMUA")){
		puts("Beli semua paket dibatalkan.");
		ql_free(&options); free(family_name); tokens_free(&tokens); free(active); accounts_free(&accounts); return 0;
	}
	int success=0,failed=0,processed=0; StrList successful={0};
	for(size_t index=start_index;index<options.n;index++){
		PaymentQuote quote[2]; memset(quote,0,sizeof(quote));
		char *target_code=xstrdup(options.v[index].code),*decoy_code=NULL,*reason=NULL,*response=NULL,*status=NULL,*message=NULL;
		long long amount=0; int round_ok=0,settlement_attempted=0,decoy_refreshed=0,retried=0;
		processed++;
		tokens_free(&tokens);
		if(refresh_account(acc,&tokens)){ reason=xstrdup("active token retrieval failed"); goto family_round_done; }
		account_sync(&accounts,acc,&tokens);
		if(payment_quote_load_retry_track(&accounts,acc,&tokens,target_code,&quote[0],&err,NULL)){
			reason=err?err:xstrdup("target quote failed"); err=NULL; goto family_round_done;
		}
		if(opts->use_decoy&&decoy_quote_resolve(&accounts,acc,&tokens,&quote[1],&decoy_code,&err,&decoy_refreshed)){
			reason=err?err:xstrdup("decoy resolve/validation failed before settlement"); err=NULL; goto family_round_done;
		}
		if(opts->use_decoy&&decoy_refreshed){
			payment_quote_free(&quote[0]);
			if(payment_quote_load(&tokens,target_code,&quote[0],&err)){
				reason=err?err:xstrdup("target quote reload failed after decoy refresh"); err=NULL; goto family_round_done;
			}
		}
		amount=quote[0].price;
		if(opts->use_decoy){
			if(quote[1].price>LLONG_MAX-amount){ reason=xstrdup("payment amount overflow"); goto family_round_done; }
			amount+=quote[1].price;
		}
		repeat_prefix_item_name(&quote[0]);
		if(opts->use_decoy) repeat_prefix_item_name(&quote[1]);
		settlement_attempted=1;
		response=payment_pulsa_settle_many_raw(acc,&tokens,quote,opts->use_decoy?2:1,amount,opts->use_decoy?1:0,FAMILY_LOOP_PAYMENT_FOR);
		long long adjusted=payment_bizz_amount(response);
		if(adjusted>=0){
			printf("Adjusted total amount to: %lld\n",adjusted);
			free(response); response=NULL; amount=adjusted; retried=1;
			response=payment_pulsa_settle_many_raw(acc,&tokens,quote,opts->use_decoy?2:1,amount,opts->use_decoy?-1:0,"SHARE_PACKAGE");
		}
		response=payment_response_add_context(response,payment_cart_context(quote,opts->use_decoy?2:1));
		status=response?json_get_string(response,"status"):NULL;
		message=response?json_get_string(response,"message"):NULL;
		round_ok=payment_ok_status(status);
		if(!round_ok) reason=xstrdup(message?message:(status?status:((response&&*response)?"unparseable payment response":"empty payment response")));
family_round_done:
		if(round_ok){
			success++;
			sl_add(&successful,xasprintf("%s | %s | amount=%lld",options.v[index].group,options.v[index].name,amount));
			printf("[%zu/%zu] SUCCESS - %s | %s | amount=%lld%s\n",index+1,options.n,options.v[index].group,options.v[index].name,amount,retried?" | Bizz retry SHARE_PACKAGE":"");
			if(opts->pause_on_success) pause_enter();
		} else {
			failed++;
			printf("[%zu/%zu] FAILED - %s | %s - %s\n",index+1,options.n,options.v[index].group,options.v[index].name,reason?reason:"unknown error");
			if(response) printf("  Response: %s\n",response);
		}
		int should_delay=!message||!*message||round_ok||strstr(message,"Failed call ipaas purchase");
		free(target_code); free(decoy_code); free(reason); free(response); free(status); free(message); free(err); err=NULL;
		payment_quote_free(&quote[0]); payment_quote_free(&quote[1]);
		if(settlement_attempted&&opts->delay_seconds&&should_delay){
			printf("Waiting for %u seconds before next purchase...\n",opts->delay_seconds);
			sleep(opts->delay_seconds);
		}
	}
	printf("Purchase-by-family summary: family=%s requested=%zu processed=%d success=%d failed=%d\n",family_name,options.n-start_index,processed,success,failed);
	if(successful.n){ puts("Successful purchases:"); for(size_t i=0;i<successful.n;i++) printf("%zu. %s\n",i+1,successful.v[i]); }
	free_sl(&successful); ql_free(&options); free(family_name); tokens_free(&tokens); free(active); accounts_free(&accounts);
	return failed?1:0;
}
static void parse_family_purchase_options(int argc,char **argv,FamilyPurchaseOptions *opts){
	memset(opts,0,sizeof(*opts)); opts->start_option=1; opts->pause_on_success=1;
	for(int i=0;i<argc;i++){
		const char *arg=argv[i],*value=NULL;
		if(!strcmp(arg,"--start-from-option")||!strcmp(arg,"--start")){ if(++i>=argc) die("missing value for %s",arg); value=argv[i]; long long parsed=0; if(parse_bounded_ll(value,1,INT_MAX,&parsed)) die("invalid start option: %s",value); opts->start_option=(int)parsed; }
		else if(!strcmp(arg,"--delay")){ if(++i>=argc) die("missing value for --delay"); value=argv[i]; long long parsed=0; if(parse_bounded_ll(value,0,UINT_MAX,&parsed)) die("invalid delay: %s",value); opts->delay_seconds=(unsigned int)parsed; }
		else if(!strcmp(arg,"--use-decoy")){ if(++i>=argc||parse_bool_strict(argv[i],&opts->use_decoy)) die("invalid use_decoy"); }
		else if(!strcmp(arg,"--pause-on-success")){ if(++i>=argc||parse_bool_strict(argv[i],&opts->pause_on_success)) die("invalid pause_on_success"); }
		else die("unknown purchase-by-family option: %s",arg);
	}
}
static int cmd_purchase_by_family(int argc,char **argv){
	if(argc<1) die("usage: bitsxl purchase-by-family <family_code> [--delay seconds] [--use-decoy y|n] [--start-from-option number] [--pause-on-success y|n]");
	if(!family_code_ok(argv[0])) die("invalid family code");
	FamilyPurchaseOptions opts; parse_family_purchase_options(argc-1,argv+1,&opts);
	const char *missing=payment_config_missing(); if(missing) die("missing env %s",missing);
	return family_purchase_run(argv[0],&opts);
}
static void interactive_purchase_by_family(void){
	const char *missing=payment_config_missing();
	if(missing){ printf("Missing env %s. Set di Settings dulu.\n",missing); pause_enter(); return; }
	char family[160],input[64],answer[32]; long long parsed=0;
	FamilyPurchaseOptions opts; memset(&opts,0,sizeof(opts)); opts.start_option=1; opts.pause_on_success=1;
	printf("\033[H\033[J%s=======================================================%s\n",CC,C0);
	printf("%sBeli Semua Paket dalam Family Code (Loop)%s\n",CB,C0);
	printf("%s=======================================================%s\n",CC,C0);
	prompt_line("Enter family code (or '99' to cancel): ",family,sizeof(family));
	if(!strcmp(family,"99")) return;
	if(!family_code_ok(family)) goto family_input_invalid;
	prompt_line("Start purchasing from option number (default 1): ",input,sizeof(input));
	if(input[0]){ if(parse_bounded_ll(input,1,INT_MAX,&parsed)) goto family_input_invalid; opts.start_option=(int)parsed; }
	prompt_line("Use decoy package? (y/n): ",answer,sizeof(answer));
	if(parse_bool_strict(answer,&opts.use_decoy)) goto family_input_invalid;
	prompt_line("Pause on each successful purchase? (y/n): ",answer,sizeof(answer));
	if(parse_bool_strict(answer,&opts.pause_on_success)) goto family_input_invalid;
	prompt_line("Delay seconds between purchases (0 for no delay): ",input,sizeof(input));
	if(parse_bounded_ll(input,0,UINT_MAX,&parsed)) goto family_input_invalid;
	opts.delay_seconds=(unsigned int)parsed;
	family_purchase_run(family,&opts);
	pause_enter();
	return;
family_input_invalid:
	puts("Input Beli Semua Paket dalam Family Code tidak valid.");
	pause_enter();
}
static void cmd_transaction_history_human(void){ Accounts a={0}; char *act=NULL; Account *acc=NULL; Tokens t={0}; if(cli_begin(&a,&act,&acc,&t)) exit(1); char *history=get_transaction_history_retry(&a,acc,&t); puts(history); free(history); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_pending_transactions_human(void){ Accounts a={0}; char *act=NULL; Account *acc=NULL; Tokens t={0}; if(cli_begin(&a,&act,&acc,&t)) exit(1); char *pending=get_pending_transactions_retry(&a,acc,&t); puts(pending); free(pending); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_transaction_status_human(const char *tx,const char *status_arg){ if(!tx||!*tx) die("missing transaction id"); Accounts a={0}; char *act=NULL; Account *acc=NULL; Tokens t={0}; if(cli_begin(&a,&act,&acc,&t)) exit(1); char *detail=get_transaction_status_retry(&a,acc,&t,tx,status_arg?status_arg:""); puts(detail); free(detail); tokens_free(&t); free(act); accounts_free(&a); }
static void cmd_pay_human(const char *mode,int n,char **codes){
	const char *missing=payment_config_missing();
	if(missing) die("missing env %s",missing);
	long long custom_amount=-1;
	if(strip_custom_amount_args(&n,codes,&custom_amount)) die("invalid custom amount");
	if((!strcmp(mode,"point")||!strcmp(mode,"voucher")||!strcmp(mode,"gift"))&&custom_amount>=0) die("custom amount not supported for %s",mode);
	if(n<1) die("missing package code");
	if(n>3) die("cart max 3 packages");
	Accounts a={0}; char *act=NULL; Account *acc=NULL; Tokens t={0};
	if(cli_begin(&a,&act,&acc,&t)) exit(1);
	PaymentQuote q[4]; memset(q,0,sizeof(q));
	long long total=0; int loaded=0;
	for(int i=0;i<n;i++){
		char *err=NULL; int refreshed=0;
		if(payment_quote_load_retry_track(&a,acc,&t,codes[i],&q[i],&err,&refreshed)){
			fprintf(stderr,"quote failed %s: %s\n",codes[i],err?err:"unknown");
			free(err);
			for(int j=0;j<loaded;j++) payment_quote_free(&q[j]);
			tokens_free(&t); free(act); accounts_free(&a);
			exit(1);
		}
		if(refreshed&&loaded>0){
			long long reloaded_total=0;
			if(payment_quotes_reload_current(&t,q,loaded,&reloaded_total,&err)){
				fprintf(stderr,"prior quote reload failed after token refresh: %s\n",err?err:"unknown");
				free(err); payment_quote_free(&q[i]); tokens_free(&t); free(act); accounts_free(&a); exit(1);
			}
			total=reloaded_total;
		}
		payment_quote_print_pretty(&q[i]);
		if(q[i].price>LLONG_MAX-total){
			fprintf(stderr,"quote total overflow\n"); payment_quote_free(&q[i]); for(int j=0;j<loaded;j++) payment_quote_free(&q[j]); tokens_free(&t); free(act); accounts_free(&a); exit(1);
		}
		total+=q[i].price;
		loaded++;
	}
	if(!strcmp(mode,"balance")){
		long long amount=custom_amount_or_prompt(total,custom_amount);
		if(!confirm_amount("Balance",amount)){
			char *resp=payment_pulsa_settle_many_once(&a,acc,&t,q,loaded,amount);
			print_payment_response(resp);
			free(resp);
		}
	} else if(!strcmp(mode,"decoy-standard")) interactive_payment_decoy_quotes(&a,acc,&t,q,loaded,total,custom_amount,1);
	else if(!strcmp(mode,"decoy-v2")) interactive_payment_decoy_quotes(&a,acc,&t,q,loaded,total,custom_amount,0);
	else if(!strcmp(mode,"qris")) interactive_payment_qris_quotes(&a,acc,&t,q,loaded,total,custom_amount);
	else if(!strcmp(mode,"ewallet")) interactive_payment_ewallet_quotes(&a,acc,&t,q,loaded,total,custom_amount);
	else if(!strcmp(mode,"point")||!strcmp(mode,"voucher")||!strcmp(mode,"gift")){
		if(loaded!=1) puts("Mode ini hanya untuk satu paket.");
		else interactive_payment_special_quote(&a,acc,&t,&q[0],mode);
	} else puts("Mode pembayaran tidak valid.");
	for(int i=0;i<loaded;i++) payment_quote_free(&q[i]);
	tokens_free(&t); free(act); accounts_free(&a);
}
static void interactive_main_menu(void){ for(;;){ Accounts a=accounts_load(); char *act=active_get(); Account *active=(act&&*act)?accounts_find(&a,act):NULL; printf("\033[H\033[J"); printf("%s=======================================================%s\n",CC,C0); if(active){ printf("%sNomor:%s %s | %sType:%s %s\n",CB,C0,active->number,CB,C0,active->subscription_type); Tokens t={0}; if(refresh_account(active,&t)==0){ account_sync(&a,active,&t); char *bal=get_balance_api(&t); long long remaining=balance_remaining(bal),exp=balance_expired_at(bal); char rp[64],dt[32]; if(remaining>=0) money_id(remaining,rp,sizeof(rp)); else snprintf(rp,sizeof(rp),"N/A"); date_str(exp,dt,sizeof(dt)); printf("%sPulsa:%s Rp %s | %sAktif sampai:%s %s\n",CB,C0,rp,CB,C0,dt); free(bal); tokens_free(&t); } else printf("%sPulsa:%s N/A | %sAktif sampai:%s N/A\n",CB,C0,CB,C0); } else printf("Belum ada akun aktif\n"); printf("%s=======================================================%s\nMenu:\n1. Login/Ganti akun\n2. Lihat Paket Saya\n3. Store Segments\n4. Store Packages\n5. Beli Paket Berdasarkan Family Code\n6. Point / Redeemables\n7. Beli Semua Paket dalam Family Code (Loop)\n99. Tutup aplikasi\n-------------------------------------------------------\n",CC,C0); char choice[32]; prompt_line("Pilih menu: ",choice,sizeof(choice)); if(!strcmp(choice,"1")){ free(act); accounts_free(&a); interactive_account_menu(); continue; } if(!strcmp(choice,"2")){ if(!active){ puts("No active user. Pilih/login akun dulu."); pause_enter(); } else { char num[32]; snprintf(num,sizeof(num),"%s",active->number); free(act); accounts_free(&a); interactive_quota(num); continue; } } else if(!strcmp(choice,"3")){ free(act); accounts_free(&a); interactive_store_segments(); continue; } else if(!strcmp(choice,"4")){ free(act); accounts_free(&a); interactive_store_packages(); continue; } else if(!strcmp(choice,"5")){ free(act); accounts_free(&a); interactive_store_family(); continue; } else if(!strcmp(choice,"6")){ free(act); accounts_free(&a); interactive_redeemables(""); continue; } else if(!strcmp(choice,"7")){ if(!active){ puts("No active user. Pilih/login akun dulu."); pause_enter(); } else { free(act); accounts_free(&a); interactive_purchase_by_family(); continue; } } else if(!strcmp(choice,"99")){ free(act); accounts_free(&a); puts("Exiting the application."); return; } else { puts("Invalid choice."); pause_enter(); } free(act); accounts_free(&a); } }
static void usage(void){ puts("bitsxl                 # menu interaktif\nbitsxl login <08...|628...>\nbitsxl otp <08...|628...> <code>\nbitsxl accounts\nbitsxl use <628...>\nbitsxl del <628...>\nbitsxl logout\nbitsxl quota [628...]\nbitsxl segments [y|n]\nbitsxl point|redeemables [y|n]\nbitsxl history|transaction-history\nbitsxl pending|pending-transactions\nbitsxl transaction-status <id> [status]\nbitsxl pay-quote <package_code>\nbitsxl pay-pulsa <package_code> [--amount rupiah] BAYAR-<harga>\nbitsxl pay-balance|pay-qris|pay-ewallet [--amount rupiah] <package_code...>\nbitsxl pay-balance-decoy-standard [--amount rupiah] <option_code...>\nbitsxl pay-balance-decoy-v2 [--amount rupiah] <option_code...>\n  aliases: pay-decoy-standard; pay-decoy-v2; legacy pay-decoy/pay-balance-decoy -> V2\nbitsxl pay-point|pay-voucher|pay-gift <package_code>\nbitsxl purchase-n-times <family_code> <variant_code> <order> <count> [--delay seconds] [--use-decoy y|n] [--token-confirmation-idx idx]\nbitsxl purchase-n-times-by-option-code <option_code> <count> [--delay seconds] [--use-decoy y|n] [--token-confirmation-idx idx]\nbitsxl purchase-by-family <family_code> [--delay seconds] [--use-decoy y|n] [--start-from-option number] [--pause-on-success y|n]\nbitsxl env\nbitsxl accounts-json\nbitsxl status-json|dashboard-json\nbitsxl quota-json [628...]\nbitsxl env-json|settings-json\nbitsxl json family <family_code>\nbitsxl json notifications\nbitsxl json notification-detail <id>\nbitsxl json notification-read-all <id...>\nbitsxl json payment balance-decoy-standard <option_code...> confirm=1\nbitsxl json payment balance-decoy-v2 <option_code...> confirm=1\n  JSON aliases: decoy-standard; decoy-v2; legacy balance-decoy/decoy/prio -> V2\nbitsxl json tiering|history|transaction-history|pending|transaction-status|payment-status|family-list|point|redeemables\nbitsxl json shop family-list [y|n]\nbitsxl json dashboard|status|accounts|quota|segments|payment|env|settings|login|otp|use|del|logout|reset-session|unsub"); }
int main(int argc,char **argv){ load_config(); if(argc<2){ require_config(); interactive_main_menu(); return 0; } if(!strcmp(argv[1],"--version")||!strcmp(argv[1],"-V")||!strcmp(argv[1],"version")){ puts(BITSXL_VERSION); return 0; } if(!strcmp(argv[1],"--help")||!strcmp(argv[1],"-h")||!strcmp(argv[1],"help")){ usage(); return 0; } if(!strcmp(argv[1],"login")&&argc==3){ require_config(); cmd_login(argv[2]); } else if(!strcmp(argv[1],"otp")&&argc==4){ require_config(); cmd_otp(argv[2],argv[3]); } else if(!strcmp(argv[1],"accounts")) cmd_accounts(); else if(!strcmp(argv[1],"accounts-json")) cmd_json_accounts(); else if(!strcmp(argv[1],"use")&&argc==3) cmd_use(argv[2]); else if(!strcmp(argv[1],"del")&&argc==3) cmd_del(argv[2]); else if(!strcmp(argv[1],"logout")||!strcmp(argv[1],"reset-session")) cmd_logout(); else if(!strcmp(argv[1],"quota")){ require_config(); cmd_quota(argc>=3?argv[2]:NULL); } else if(!strcmp(argv[1],"quota-json")){ if(json_require_config()) return 1; cmd_json_quota(argc>=3?argv[2]:NULL,argc>=4&&!strcmp(argv[3],"fresh")); } else if(!strcmp(argv[1],"segments")||!strcmp(argv[1],"store-segments")){ require_config(); cmd_segments(argc>=3?argv[2]:""); } else if(!strcmp(argv[1],"point")||!strcmp(argv[1],"redeemables")){ require_config(); interactive_redeemables(argc>=3?argv[2]:""); } else if(!strcmp(argv[1],"shop")||!strcmp(argv[1],"store-packages")){ require_config(); cmd_shop(argc>=3?argv[2]:""); } else if(!strcmp(argv[1],"history")||!strcmp(argv[1],"transaction-history")){ require_config(); cmd_transaction_history_human(); } else if(!strcmp(argv[1],"pending")||!strcmp(argv[1],"pending-transactions")){ require_config(); cmd_pending_transactions_human(); } else if(!strcmp(argv[1],"transaction-status")||!strcmp(argv[1],"status-transaction")||!strcmp(argv[1],"payment-status")){ require_config(); cmd_transaction_status_human(argc>=3?argv[2]:NULL,argc>=4?argv[3]:""); } else if(!strcmp(argv[1],"pay-quote")&&argc>=3){ require_config(); cmd_pay_quote(argv[2]); } else if(!strcmp(argv[1],"pay-pulsa")&&argc>=3){ require_config(); int pay_argc=argc-2; char **pay_argv=argv+2; long long custom_amount=-1; if(strip_custom_amount_args(&pay_argc,pay_argv,&custom_amount)) die("invalid custom amount"); if(pay_argc<1) die("missing package code"); cmd_pay_pulsa(pay_argv[0],pay_argc>=2?pay_argv[1]:NULL,custom_amount); } else if(!strcmp(argv[1],"pay-balance")||!strcmp(argv[1],"pay-auto")){ require_config(); cmd_pay_human("balance",argc-2,argv+2); } else if(!strcmp(argv[1],"pay-balance-decoy-standard")||!strcmp(argv[1],"pay-decoy-standard")){ require_config(); cmd_pay_human("decoy-standard",argc-2,argv+2); } else if(!strcmp(argv[1],"pay-balance-decoy-v2")||!strcmp(argv[1],"pay-decoy-v2")||!strcmp(argv[1],"pay-decoy")||!strcmp(argv[1],"pay-balance-decoy")){ require_config(); cmd_pay_human("decoy-v2",argc-2,argv+2); } else if(!strcmp(argv[1],"pay-qris")){ require_config(); cmd_pay_human("qris",argc-2,argv+2); } else if(!strcmp(argv[1],"pay-ewallet")){ require_config(); cmd_pay_human("ewallet",argc-2,argv+2); } else if(!strcmp(argv[1],"pay-point")){ require_config(); cmd_pay_human("point",argc-2,argv+2); } else if(!strcmp(argv[1],"pay-voucher")){ require_config(); cmd_pay_human("voucher",argc-2,argv+2); } else if(!strcmp(argv[1],"pay-gift")){ require_config(); cmd_pay_human("gift",argc-2,argv+2); } else if(!strcmp(argv[1],"purchase-n-times")){ require_config(); return cmd_purchase_n_times(argc-2,argv+2); } else if(!strcmp(argv[1],"purchase-n-times-by-option-code")){ require_config(); return cmd_purchase_n_times_by_option_code(argc-2,argv+2); } else if(!strcmp(argv[1],"purchase-by-family")){ require_config(); return cmd_purchase_by_family(argc-2,argv+2); } else if(!strcmp(argv[1],"status-json")||!strcmp(argv[1],"dashboard-json")){ if(json_require_config()) return 1; cmd_json_dashboard(); } else if(!strcmp(argv[1],"env")) cmd_env(); else if(!strcmp(argv[1],"env-json")||!strcmp(argv[1],"settings-json")) cmd_json_env(); else if(!strcmp(argv[1],"json")) cmd_json(argc-2,argv+2); else { usage(); return 1; } return 0; }
