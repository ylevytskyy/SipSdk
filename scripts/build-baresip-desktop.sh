#!/usr/bin/env bash
set -euo pipefail

# Reproducible native engine build for desktop platforms (macOS & Linux).
# Pinned to upstream v4.12.0 releases by immutable commit SHA.
readonly BARESIP_VERSION="4.12.0"
readonly BARESIP_COMMIT="9cfab81550acc4bd38d6580d79b45cc6eea65cd6"
readonly RE_VERSION="4.12.0"
readonly RE_COMMIT="65555dad439de9d6a81b3ee6092b9c096585249b"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREBUILT_ROOT="${1:-$ROOT_DIR/platforms/desktop/prebuilt}"

OS_NAME="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH_NAME="$(uname -m)"
case "$ARCH_NAME" in
  x86_64|amd64) ARCH_NAME="x86_64" ;;
  arm64|aarch64) ARCH_NAME="arm64" ;;
  *) echo "ERROR: Unsupported architecture $ARCH_NAME" >&2; exit 1 ;;
esac

TARGET_SLICE="${OS_NAME}-${ARCH_NAME}"
OUTPUT_DIR="$PREBUILT_ROOT/$TARGET_SLICE"
WORK_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/sipsdk-desktop-build-$$"
SOURCE_DIR="$WORK_ROOT/src"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

command -v git >/dev/null || die "git is required"
command -v cmake >/dev/null || die "cmake is required"

mkdir -p "$SOURCE_DIR" "$OUTPUT_DIR/lib" "$PREBUILT_ROOT/include"
trap 'rm -rf "$WORK_ROOT"' EXIT

clone_pinned_tag() {
  local url="$1"
  local tag="$2"
  local expected_commit="$3"
  local destination="$4"

  git clone --quiet --depth 1 --branch "$tag" "$url" "$destination"
  local actual_commit
  actual_commit="$(git -C "$destination" rev-parse HEAD)"
  [[ "$actual_commit" == "$expected_commit" ]] || die \
    "$url tag $tag resolved to $actual_commit, expected pinned commit $expected_commit"
}

echo "==> Fetching libre $RE_VERSION and Baresip $BARESIP_VERSION for desktop ($TARGET_SLICE)..."
clone_pinned_tag "https://github.com/baresip/re.git" "v$RE_VERSION" "$RE_COMMIT" "$SOURCE_DIR/re"
clone_pinned_tag "https://github.com/baresip/baresip.git" "v$BARESIP_VERSION" "$BARESIP_COMMIT" "$SOURCE_DIR/baresip"

# Apply platform patches
if [[ "$OS_NAME" == "darwin" ]]; then
  echo "==> Patching libre TLS stub for Apple desktop builds..."
  python3 - "$SOURCE_DIR/re/CMakeLists.txt" <<'PATCH_APPLE_TLS'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
needle = """elseif(APPLE)
  list(APPEND SRCS
    src/aes/apple/aes.c
    src/hmac/apple/hmac.c
  )
"""
replacement = """elseif(APPLE)
  list(APPEND SRCS
    src/aes/apple/aes.c
    src/hmac/apple/hmac.c
    src/tls/stub.c
  )
"""
if needle in source:
    path.write_text(source.replace(needle, replacement, 1))
PATCH_APPLE_TLS

elif [[ "$OS_NAME" == "linux" ]]; then
  echo "==> Patching libre crypto fallback for Linux non-OpenSSL builds..."
  python3 - "$SOURCE_DIR/re" <<'PATCH_LINUX_CRYPTO'
import sys
from pathlib import Path

re_dir = Path(sys.argv[1])

# 1. Write crypto_fallback.c
crypto_c = re_dir / "src/crypto_fallback.c"
crypto_c.write_text("""/* Self-contained RFC standard MD5, SHA1, SHA256, HMAC for platforms without OpenSSL/CommonCrypto */
#include <stdint.h>
#include <string.h>
#include <stddef.h>

/* --- MD5 --- */
typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
} MD5_CTX;

#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))
#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32-(n))))

#define FF(a, b, c, d, x, s, ac) { \
    (a) += F((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b); \
}
#define GG(a, b, c, d, x, s, ac) { \
    (a) += G((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b); \
}
#define HH(a, b, c, d, x, s, ac) { \
    (a) += H((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b); \
}
#define II(a, b, c, d, x, s, ac) { \
    (a) += I((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b); \
}

static void MD5Transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    for (int i = 0, j = 0; i < 16; i++, j += 4)
        x[i] = ((uint32_t)block[j]) | (((uint32_t)block[j+1]) << 8) |
               (((uint32_t)block[j+2]) << 16) | (((uint32_t)block[j+3]) << 24);

    FF(a, b, c, d, x[ 0],  7, 0xd76aa478); FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);
    FF(c, d, a, b, x[ 2], 17, 0x242070db); FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);
    FF(a, b, c, d, x[ 4],  7, 0xf57c0faf); FF(d, a, b, c, x[ 5], 12, 0x4787c62a);
    FF(c, d, a, b, x[ 6], 17, 0xa8304613); FF(b, c, d, a, x[ 7], 22, 0xfd469501);
    FF(a, b, c, d, x[ 8],  7, 0x698098d8); FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);
    FF(c, d, a, b, x[10], 17, 0xffff5bb1); FF(b, c, d, a, x[11], 22, 0x895cd7be);
    FF(a, b, c, d, x[12],  7, 0x6b901122); FF(d, a, b, c, x[13], 12, 0xfd987193);
    FF(c, d, a, b, x[14], 17, 0xa679438e); FF(b, c, d, a, x[15], 22, 0x49b40821);

    GG(a, b, c, d, x[ 1],  5, 0xf61e2562); GG(d, a, b, c, x[ 6],  9, 0xc040b340);
    GG(c, d, a, b, x[11], 14, 0x265e5a51); GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);
    GG(a, b, c, d, x[ 5],  5, 0xd62f105d); GG(d, a, b, c, x[10],  9, 0x02441453);
    GG(c, d, a, b, x[15], 14, 0xd8a1e681); GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);
    GG(a, b, c, d, x[ 9],  5, 0x21e1cde6); GG(d, a, b, c, x[14],  9, 0xc33707d6);
    GG(c, d, a, b, x[ 3], 14, 0xf4d50d87); GG(b, c, d, a, x[ 8], 20, 0x455a14ed);
    GG(a, b, c, d, x[13],  5, 0xa9e3e905); GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8);
    GG(c, d, a, b, x[ 7], 14, 0x676f02d9); GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

    HH(a, b, c, d, x[ 5],  4, 0xfffa3942); HH(d, a, b, c, x[ 8], 11, 0x8771f681);
    HH(c, d, a, b, x[11], 16, 0x6d9d6122); HH(b, c, d, a, x[14], 23, 0xfde5380c);
    HH(a, b, c, d, x[ 1],  4, 0xa4beea44); HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);
    HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60); HH(b, c, d, a, x[10], 23, 0xbebfbc70);
    HH(a, b, c, d, x[13],  4, 0x289b7ec6); HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);
    HH(c, d, a, b, x[ 3], 16, 0xd4ef3085); HH(b, c, d, a, x[ 6], 23, 0x04881d05);
    HH(a, b, c, d, x[ 9],  4, 0xd9d4d039); HH(d, a, b, c, x[12], 11, 0xe6db99e5);
    HH(c, d, a, b, x[15], 16, 0x1fa27cf8); HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);

    II(a, b, c, d, x[ 0],  6, 0xf4292244); II(d, a, b, c, x[ 7], 10, 0x432aff97);
    II(c, d, a, b, x[14], 15, 0xab9423a7); II(b, c, d, a, x[ 5], 21, 0xfc93a039);
    II(a, b, c, d, x[12],  6, 0x655b59c3); II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);
    II(c, d, a, b, x[10], 15, 0xffeff47d); II(b, c, d, a, x[ 1], 21, 0x85845dd1);
    II(a, b, c, d, x[ 8],  6, 0x6fa87e4f); II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
    II(c, d, a, b, x[ 6], 15, 0xa3014314); II(b, c, d, a, x[13], 21, 0x4e0811a1);
    II(a, b, c, d, x[ 4],  6, 0xf7537e82); II(d, a, b, c, x[11], 10, 0xbd3af235);
    II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb); II(b, c, d, a, x[ 9], 21, 0xeb86d391);
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void MD5Init(MD5_CTX *ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe; ctx->state[3] = 0x10325476;
}

static void MD5Update(MD5_CTX *ctx, const uint8_t *input, size_t inputLen) {
    size_t i, index, partLen;
    index = (size_t)((ctx->count[0] >> 3) & 0x3F);
    if ((ctx->count[0] += ((uint32_t)inputLen << 3)) < ((uint32_t)inputLen << 3))
        ctx->count[1]++;
    ctx->count[1] += ((uint32_t)inputLen >> 29);
    partLen = 64 - index;
    if (inputLen >= partLen) {
        memcpy(&ctx->buffer[index], input, partLen);
        MD5Transform(ctx->state, ctx->buffer);
        for (i = partLen; i + 63 < inputLen; i += 64)
            MD5Transform(ctx->state, &input[i]);
        index = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[index], &input[i], inputLen - i);
}

static void MD5Final(uint8_t digest[16], MD5_CTX *ctx) {
    static const uint8_t PADDING[64] = { 0x80 };
    uint8_t bits[8];
    for (int i = 0, j = 0; i < 2; i++, j += 4) {
        bits[j] = (uint8_t)((ctx->count[i]) & 0xff);
        bits[j+1] = (uint8_t)((ctx->count[i] >> 8) & 0xff);
        bits[j+2] = (uint8_t)((ctx->count[i] >> 16) & 0xff);
        bits[j+3] = (uint8_t)((ctx->count[i] >> 24) & 0xff);
    }
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3f);
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);
    MD5Update(ctx, PADDING, padLen);
    MD5Update(ctx, bits, 8);
    for (int i = 0, j = 0; i < 4; i++, j += 4) {
        digest[j] = (uint8_t)(ctx->state[i] & 0xff);
        digest[j+1] = (uint8_t)((ctx->state[i] >> 8) & 0xff);
        digest[j+2] = (uint8_t)((ctx->state[i] >> 16) & 0xff);
        digest[j+3] = (uint8_t)((ctx->state[i] >> 24) & 0xff);
    }
}

void fallback_md5(const uint8_t *d, size_t n, uint8_t *md) {
    MD5_CTX ctx;
    MD5Init(&ctx);
    MD5Update(&ctx, d, n);
    MD5Final(md, &ctx);
}

/* --- SHA-1 --- */
typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} SHA1_CTX;

#define SHA1_ROL(val, bits) (((val) << (bits)) | ((val) >> (32 - (bits))))

static void SHA1Transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)buffer[i * 4] << 24) | ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) | ((uint32_t)buffer[i * 4 + 3]);
    for (int i = 16; i < 80; i++)
        w[i] = SHA1_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 20; i++) {
        uint32_t t = SHA1_ROL(a, 5) + ((b & c) | ((~b) & d)) + e + 0x5A827999 + w[i];
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
    }
    for (int i = 20; i < 40; i++) {
        uint32_t t = SHA1_ROL(a, 5) + (b ^ c ^ d) + e + 0x6ED9EBA1 + w[i];
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
    }
    for (int i = 40; i < 60; i++) {
        uint32_t t = SHA1_ROL(a, 5) + ((b & c) | (b & d) | (c & d)) + e + 0x8F1BBCDC + w[i];
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
    }
    for (int i = 60; i < 80; i++) {
        uint32_t t = SHA1_ROL(a, 5) + (b ^ c ^ d) + e + 0xCA62C1D6 + w[i];
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void SHA1Init(SHA1_CTX *ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89; ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476; ctx->state[4] = 0xC3D2E1F0;
    ctx->count[0] = ctx->count[1] = 0;
}

static void SHA1Update(SHA1_CTX *ctx, const uint8_t *data, size_t len) {
    size_t i, j = (ctx->count[0] >> 3) & 63;
    if ((ctx->count[0] += (uint32_t)(len << 3)) < (len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);
    if ((j + len) > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        SHA1Transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) SHA1Transform(ctx->state, &data[i]);
        j = 0;
    } else i = 0;
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

static void SHA1Final(uint8_t digest[20], SHA1_CTX *ctx) {
    static const uint8_t PADDING[64] = { 0x80 };
    uint8_t bits[8];
    for (int i = 0, j = 0; i < 2; i++, j += 4) {
        bits[j] = (uint8_t)((ctx->count[1 - i] >> 24) & 0xff);
        bits[j+1] = (uint8_t)((ctx->count[1 - i] >> 16) & 0xff);
        bits[j+2] = (uint8_t)((ctx->count[1 - i] >> 8) & 0xff);
        bits[j+3] = (uint8_t)((ctx->count[1 - i]) & 0xff);
    }
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3f);
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);
    SHA1Update(ctx, PADDING, padLen);
    SHA1Update(ctx, bits, 8);
    for (int i = 0; i < 5; i++) {
        digest[i*4]   = (uint8_t)((ctx->state[i] >> 24) & 0xff);
        digest[i*4+1] = (uint8_t)((ctx->state[i] >> 16) & 0xff);
        digest[i*4+2] = (uint8_t)((ctx->state[i] >> 8) & 0xff);
        digest[i*4+3] = (uint8_t)((ctx->state[i]) & 0xff);
    }
}

void fallback_sha1(const uint8_t *d, size_t n, uint8_t *md) {
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, d, n);
    SHA1Final(md, &ctx);
}

/* --- SHA-256 --- */
typedef struct {
    uint32_t state[8];
    uint32_t count[2];
    uint8_t buffer[64];
} SHA256_CTX;

#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTATE_LEFT(x, 30) ^ ROTATE_LEFT(x, 19) ^ ROTATE_LEFT(x, 10))
#define EP1(x) (ROTATE_LEFT(x, 26) ^ ROTATE_LEFT(x, 21) ^ ROTATE_LEFT(x, 7))
#define SIG0(x) (ROTATE_LEFT(x, 25) ^ ROTATE_LEFT(x, 14) ^ ((x) >> 3))
#define SIG1(x) (ROTATE_LEFT(x, 15) ^ ROTATE_LEFT(x, 13) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void SHA256Transform(uint32_t state[8], const uint8_t data[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3],
             e = state[4], f = state[5], g = state[6], h = state[7], w[64];
    for (int i = 0, j = 0; i < 16; i++, j += 4)
        w[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | ((uint32_t)data[j+3]);
    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + K256[i] + w[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void SHA256Init(SHA256_CTX *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count[0] = ctx->count[1] = 0;
}

static void SHA256Update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    size_t i, j = (ctx->count[0] >> 3) & 63;
    if ((ctx->count[0] += (uint32_t)(len << 3)) < (len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);
    if ((j + len) > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        SHA256Transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) SHA256Transform(ctx->state, &data[i]);
        j = 0;
    } else i = 0;
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

static void SHA256Final(uint8_t digest[32], SHA256_CTX *ctx) {
    static const uint8_t PADDING[64] = { 0x80 };
    uint8_t bits[8];
    for (int i = 0, j = 0; i < 2; i++, j += 4) {
        bits[j] = (uint8_t)((ctx->count[1 - i] >> 24) & 0xff);
        bits[j+1] = (uint8_t)((ctx->count[1 - i] >> 16) & 0xff);
        bits[j+2] = (uint8_t)((ctx->count[1 - i] >> 8) & 0xff);
        bits[j+3] = (uint8_t)((ctx->count[1 - i]) & 0xff);
    }
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3f);
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);
    SHA256Update(ctx, PADDING, padLen);
    SHA256Update(ctx, bits, 8);
    for (int i = 0; i < 8; i++) {
        digest[i*4]   = (uint8_t)((ctx->state[i] >> 24) & 0xff);
        digest[i*4+1] = (uint8_t)((ctx->state[i] >> 16) & 0xff);
        digest[i*4+2] = (uint8_t)((ctx->state[i] >> 8) & 0xff);
        digest[i*4+3] = (uint8_t)((ctx->state[i]) & 0xff);
    }
}

void fallback_sha256(const uint8_t *d, size_t n, uint8_t *md) {
    SHA256_CTX ctx;
    SHA256Init(&ctx);
    SHA256Update(&ctx, d, n);
    SHA256Final(md, &ctx);
}

/* --- HMAC --- */
void fallback_hmac_sha1(const uint8_t *k, size_t kl, const uint8_t *d, size_t dl, uint8_t *md, size_t md_len) {
    uint8_t key[64] = {0};
    uint8_t ipad[64], opad[64];
    uint8_t inner[20];
    if (kl > 64) fallback_sha1(k, kl, key);
    else memcpy(key, k, kl);
    for (int i = 0; i < 64; i++) {
        ipad[i] = key[i] ^ 0x36;
        opad[i] = key[i] ^ 0x5c;
    }
    SHA1_CTX ictx, octx;
    SHA1Init(&ictx);
    SHA1Update(&ictx, ipad, 64);
    SHA1Update(&ictx, d, dl);
    SHA1Final(inner, &ictx);

    SHA1Init(&octx);
    SHA1Update(&octx, opad, 64);
    SHA1Update(&octx, inner, 20);
    uint8_t full_md[20];
    SHA1Final(full_md, &octx);
    size_t copy_len = md_len < 20 ? md_len : 20;
    memcpy(md, full_md, copy_len);
}

void fallback_hmac_sha256(const uint8_t *k, size_t kl, const uint8_t *d, size_t dl, uint8_t *md, size_t md_len) {
    uint8_t key[64] = {0};
    uint8_t ipad[64], opad[64];
    uint8_t inner[32];
    if (kl > 64) fallback_sha256(k, kl, key);
    else memcpy(key, k, kl);
    for (int i = 0; i < 64; i++) {
        ipad[i] = key[i] ^ 0x36;
        opad[i] = key[i] ^ 0x5c;
    }
    SHA256_CTX ictx, octx;
    SHA256Init(&ictx);
    SHA256Update(&ictx, ipad, 64);
    SHA256Update(&ictx, d, dl);
    SHA256Final(inner, &ictx);

    SHA256Init(&octx);
    SHA256Update(&octx, opad, 64);
    SHA256Update(&octx, inner, 32);
    uint8_t full_md[32];
    SHA256Final(full_md, &octx);
    size_t copy_len = md_len < 32 ? md_len : 32;
    memcpy(md, full_md, copy_len);
}
""")

# 2. Patch md5/wrap.c
md5_wrap = re_dir / "src/md5/wrap.c"
txt = md5_wrap.read_text()
txt = txt.replace("#error missing MD5 backend", "fallback_md5(d, n, md);")
if "void fallback_md5" not in txt:
    txt = "#include <stdint.h>\n#include <stddef.h>\nvoid fallback_md5(const uint8_t *d, size_t n, uint8_t *md);\n" + txt
md5_wrap.write_text(txt)

# 3. Patch sha/wrap.c
sha_wrap = re_dir / "src/sha/wrap.c"
txt = sha_wrap.read_text()
txt = txt.replace("#error missing SHA-1 backend", "fallback_sha1(d, n, md);")
txt = txt.replace("#error missing SHA-256 backend", "fallback_sha256(d, n, md);")
if "void fallback_sha1" not in txt:
    txt = "#include <stdint.h>\n#include <stddef.h>\nvoid fallback_sha1(const uint8_t *d, size_t n, uint8_t *md);\nvoid fallback_sha256(const uint8_t *d, size_t n, uint8_t *md);\n" + txt
sha_wrap.write_text(txt)

# 4. Patch hmac/hmac_sha1.c
hmac_c = re_dir / "src/hmac/hmac_sha1.c"
txt = hmac_c.read_text()
txt = txt.replace("#error missing HMAC-SHA1 backend", "fallback_hmac_sha1(k, lk, d, ld, out, t);")
txt = txt.replace("#error missing HMAC-SHA256 backend", "fallback_hmac_sha256(key, key_len, data, data_len, out, out_len);")
if "void fallback_hmac_sha1" not in txt:
    txt = "#include <stdint.h>\n#include <stddef.h>\nvoid fallback_hmac_sha1(const uint8_t *k, size_t kl, const uint8_t *d, size_t dl, uint8_t *md, size_t md_len);\nvoid fallback_hmac_sha256(const uint8_t *k, size_t kl, const uint8_t *d, size_t dl, uint8_t *md, size_t md_len);\n" + txt
hmac_c.write_text(txt)

# 5. Add crypto_fallback.c and stub.c to re CMakeLists.txt
cm_path = re_dir / "CMakeLists.txt"
txt = cm_path.read_text()
if "src/crypto_fallback.c" not in txt:
    txt = txt.replace("src/aes/stub.c", "src/aes/stub.c\n    src/crypto_fallback.c\n    src/tls/stub.c")
cm_path.write_text(txt)
PATCH_LINUX_CRYPTO
fi

echo "==> Configuring libre..."
mkdir -p "$WORK_ROOT/re-build" "$WORK_ROOT/prefix"
cmake -S "$SOURCE_DIR/re" -B "$WORK_ROOT/re-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$WORK_ROOT/prefix" \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE \
  -DUSE_OPENSSL=OFF \
  -DHAVE_RESOLV=OFF \
  -DUSE_UNIXSOCK=ON \
  -DUSE_REM=ON \
  -DUSE_SIP=ON \
  -DLIBRE_BUILD_STATIC=ON \
  -DLIBRE_BUILD_SHARED=OFF

echo "==> Building and installing libre..."
cmake --build "$WORK_ROOT/re-build" --target re --parallel
cmake --install "$WORK_ROOT/re-build"

# Select modules based on OS
if [[ "$OS_NAME" == "darwin" ]]; then
  BARESIP_MODULES="g711;audiounit;account;stun;turn;ice;srtp"
else
  BARESIP_MODULES="g711;pulse;alsa;account;stun;turn;ice;srtp"
fi

echo "==> Configuring Baresip with modules: $BARESIP_MODULES..."
mkdir -p "$WORK_ROOT/baresip-build"
cmake -S "$SOURCE_DIR/baresip" -B "$WORK_ROOT/baresip-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE \
  -DCMAKE_PREFIX_PATH="$WORK_ROOT/prefix" \
  -Dre_DIR="$WORK_ROOT/prefix/lib/cmake/re" \
  -DRE_INCLUDE_DIR="$WORK_ROOT/prefix/include/re" \
  -DRE_LIBRARY="$WORK_ROOT/prefix/lib/libre.a" \
  -DSTATIC=ON \
  "-DMODULES=$BARESIP_MODULES" \
  -DAPP_MODULES=

echo "==> Building Baresip..."
cmake --build "$WORK_ROOT/baresip-build" --target baresip --parallel

echo "==> Copying prebuilt headers and libraries to $OUTPUT_DIR..."
cp "$WORK_ROOT/prefix/lib/libre.a" "$OUTPUT_DIR/lib/"

if [[ "$OS_NAME" == "darwin" ]]; then
  libtool -static -o "$OUTPUT_DIR/lib/libbaresip.a" "$WORK_ROOT/baresip-build/libbaresip.a" "$WORK_ROOT/prefix/lib/libre.a"
else
  # Use MRI script for GNU ar on Linux
  ar -M <<MRI_EOF
create $OUTPUT_DIR/lib/libbaresip.a
addlib $WORK_ROOT/baresip-build/libbaresip.a
addlib $WORK_ROOT/prefix/lib/libre.a
save
end
MRI_EOF
fi

cp -R "$WORK_ROOT/prefix/include/re" "$PREBUILT_ROOT/include/"
cp -R "$SOURCE_DIR/baresip/include" "$PREBUILT_ROOT/include/baresip"
cp -f "$WORK_ROOT/prefix/include/re"/* "$PREBUILT_ROOT/include/" 2>/dev/null || true
cp -f "$SOURCE_DIR/baresip/include"/* "$PREBUILT_ROOT/include/" 2>/dev/null || true

echo "==> Prebuilts successfully built for $TARGET_SLICE:"
ls -lh "$OUTPUT_DIR/lib"
