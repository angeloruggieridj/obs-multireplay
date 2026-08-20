/*
obs-multireplay — SHA-256, for the one thing that decides which code runs next
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

FIPS 180-4, straight. Ninety lines of well-understood arithmetic, and it is here
rather than borrowed because the alternatives all cost more than they save:
libcurl exposes no digest, libobs exposes none either, and QCryptographicHash
would put Qt in the updater's path — while this file, being pure, can be tested
against the published NIST vectors on a machine that cannot build the plugin.

It exists for updater.cpp: the release body carries the SHA-256 of every asset
(push.yaml generates CHECKSUMS.txt and publishes it AS the body), and until now
the only check on a downloaded archive was its size against the same JSON
response the URL came from — a source compared with itself. Whoever could alter
that response could hand OBS an arbitrary DLL to load at the next start.
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace multireplay {

namespace sha256 {

namespace detail {

inline constexpr uint32_t kK[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t ror(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

} // namespace detail

// Streaming, because the thing being hashed is a file that must not be read
// into memory to be checked.
class Hasher {
public:
	void update(const void *data, size_t len)
	{
		const auto *p = static_cast<const uint8_t *>(data);
		total_ += len;
		while (len > 0) {
			const size_t take =
				(64 - fill_) < len ? (64 - fill_) : len;
			for (size_t i = 0; i < take; i++)
				block_[fill_ + i] = p[i];
			fill_ += take;
			p += take;
			len -= take;
			if (fill_ == 64) {
				compress(block_.data());
				fill_ = 0;
			}
		}
	}

	// Lower-case hex, 64 characters. The form CHECKSUMS.txt is written in.
	// CONST, and it works on a copy: padding mutates the state, so a hex()
	// that finished in place would answer differently the second time it
	// was asked — the kind of thing only ever discovered by a checksum that
	// mysteriously fails on a retry.
	std::string hex() const
	{
		Hasher tmp = *this;
		return tmp.finish();
	}

private:
	std::string finish()
	{
		const uint64_t bits = total_ * 8;
		uint8_t pad[72] = {};
		pad[0] = 0x80;
		// One byte of 0x80, zeroes, then the length: pad so the total is
		// 56 mod 64, then eight bytes of big-endian bit count.
		size_t padLen = (fill_ < 56) ? (56 - fill_) : (120 - fill_);
		for (int i = 0; i < 8; i++)
			pad[padLen + i] = (uint8_t)(bits >> (56 - 8 * i));
		const uint64_t keep = total_;
		update(pad, padLen + 8);
		total_ = keep; // update() must not count the padding

		std::string out;
		out.reserve(64);
		static const char *kHex = "0123456789abcdef";
		for (uint32_t h : h_) {
			for (int i = 3; i >= 0; i--) {
				const uint8_t b = (uint8_t)(h >> (8 * i));
				out += kHex[b >> 4];
				out += kHex[b & 0x0F];
			}
		}
		return out;
	}

	void compress(const uint8_t *p)
	{
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = ((uint32_t)p[i * 4] << 24) |
			       ((uint32_t)p[i * 4 + 1] << 16) |
			       ((uint32_t)p[i * 4 + 2] << 8) |
			       (uint32_t)p[i * 4 + 3];
		for (int i = 16; i < 64; i++) {
			const uint32_t s0 = detail::ror(w[i - 15], 7) ^
					    detail::ror(w[i - 15], 18) ^
					    (w[i - 15] >> 3);
			const uint32_t s1 = detail::ror(w[i - 2], 17) ^
					    detail::ror(w[i - 2], 19) ^
					    (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4],
			 f = h_[5], g = h_[6], hh = h_[7];
		for (int i = 0; i < 64; i++) {
			const uint32_t S1 = detail::ror(e, 6) ^
					    detail::ror(e, 11) ^
					    detail::ror(e, 25);
			const uint32_t ch = (e & f) ^ (~e & g);
			const uint32_t t1 = hh + S1 + ch + detail::kK[i] + w[i];
			const uint32_t S0 = detail::ror(a, 2) ^
					    detail::ror(a, 13) ^
					    detail::ror(a, 22);
			const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t t2 = S0 + maj;
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h_[0] += a;
		h_[1] += b;
		h_[2] += c;
		h_[3] += d;
		h_[4] += e;
		h_[5] += f;
		h_[6] += g;
		h_[7] += hh;
	}

	std::array<uint32_t, 8> h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
				      0xa54ff53a, 0x510e527f, 0x9b05688c,
				      0x1f83d9ab, 0x5be0cd19};
	std::array<uint8_t, 64> block_{};
	size_t fill_ = 0;
	uint64_t total_ = 0;
};

inline std::string hexOf(const std::string &s)
{
	Hasher h;
	h.update(s.data(), s.size());
	return h.hex();
}

} // namespace sha256

} // namespace multireplay
