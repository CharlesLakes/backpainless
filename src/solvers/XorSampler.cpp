#include "solvers/XorSampler.hpp"

#include <algorithm>

unsigned long
drawRandomXors(const std::vector<int>& pool,
			   unsigned int m,
			   const XorDensity& density,
			   std::mt19937_64& rng,
			   std::vector<std::vector<int>>& xors)
{
	xors.clear();
	const size_t poolSize = pool.size();
	if (poolSize == 0)
		return 0;

	/* Rows are bitsets over the pool. The basis is kept in echelon form: basis[i] has a zero on the pivots of the
	 * rows before it, so reducing a row by the basis rows in order clears every pivot. A row reduced to zero is a sum
	 * of previous XORs */
	const size_t words = (poolSize + 63) / 64;
	const uint64_t tailMask = (poolSize % 64) ? ((1ULL << (poolSize % 64)) - 1) : ~0ULL;
	std::vector<std::vector<uint64_t>> basis;
	std::vector<size_t> pivots;
	std::vector<uint64_t> row(words), reduced(words);
	std::uniform_real_distribution<double> unit(0.0, 1.0);
	auto testBit = [](const std::vector<uint64_t>& bits, size_t i) { return (bits[i / 64] >> (i % 64)) & 1; };

	unsigned long dependent = 0;
	const unsigned int maxAttempts = 4 * m + 16;
	for (unsigned int attempt = 0; xors.size() < m && attempt < maxAttempts; attempt++) {
		if (density.length) {
			/* Exactly min(k, P) variables: draw the smaller of the set and its complement */
			size_t length = std::min<size_t>(density.length, poolSize);
			bool complement = length > poolSize / 2;
			size_t toDraw = complement ? poolSize - length : length;
			std::fill(row.begin(), row.end(), complement ? ~0ULL : 0ULL);
			row[words - 1] &= tailMask;
			for (size_t drawn = 0; drawn < toDraw;) {
				size_t i = rng() % poolSize;
				if (testBit(row, i) == complement) {
					row[i / 64] ^= 1ULL << (i % 64);
					drawn++;
				}
			}
		} else if (density.probability == 0.5) {
			for (auto& word : row)
				word = rng();
			row[words - 1] &= tailMask;
		} else {
			std::fill(row.begin(), row.end(), 0ULL);
			for (size_t i = 0; i < poolSize; i++)
				if (unit(rng) < density.probability)
					row[i / 64] |= 1ULL << (i % 64);
		}

		reduced = row;
		for (size_t b = 0; b < basis.size(); b++)
			if (testBit(reduced, pivots[b]))
				for (size_t w = 0; w < words; w++)
					reduced[w] ^= basis[b][w];

		size_t w = 0;
		while (w < words && reduced[w] == 0)
			w++;
		if (w == words) {
			dependent++;
			continue;
		}
		pivots.push_back(w * 64 + __builtin_ctzll(reduced[w]));
		basis.push_back(reduced);

		/* The XOR itself is the drawn row (any basis of the same span splits the same way) */
		xors.emplace_back();
		for (size_t i = 0; i < poolSize; i++)
			if (testBit(row, i))
				xors.back().push_back(pool[i]);
	}
	return dependent;
}
