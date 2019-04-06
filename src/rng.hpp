﻿#pragma once

#include "stdafx.hpp"


namespace Math {


//PCG-32
//	http://www.pcg-random.org/download.html
//	16 bytes state, period 2¹²⁸.
class RNG {
	public:
		typedef uint32_t result_type;

	private:
		union {
			struct { uint64_t _state; uint64_t _inc; };
			std::array<result_type,4> _state_packed;
		};

	public:
		                                      RNG(                ) :
			_state(0x853C49E6748FEA9Bull),
			_inc  (0xDA3E39CB94B95BDBull)
		{}
		template <class TypeSeedSeq> explicit RNG(TypeSeedSeq& seq) {
			seq.generate(_state_packed.begin(),_state_packed.end());
		}

		static constexpr result_type min() { return std::numeric_limits<result_type>::min(); }
		static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }

		//Seed the source.  State and increment must not both be zero.
		                             void seed(result_type  seed_value=0x748FEA9Bu) {
			assert(seed_value!=0); //Seed must not be zero
			_state_packed[0] = _state_packed[1] = _state_packed[2] = _state_packed[3] = seed_value;
		}
		                             void seed(uint64_t state, uint64_t increment ) {
			assert(state!=0ull||increment!=0ull); //State and increment must not both be zero
			_state = state;
			_inc   = increment;
		}
		template <class TypeSeedSeq> void seed(TypeSeedSeq& sequence              ) {
			sequence.generate(_state_packed.begin(),_state_packed.end());
		}

		result_type operator()() {
			result_type xorshifted = static_cast<result_type>( ((_state>>18u)^_state) >> 27u );
			int rot = static_cast<int>( _state >> 59u ); //top five bits
			result_type result = ( xorshifted >> rot ) | ( xorshifted << ((-rot)&31) );
			_state = _state*6364136223846793005ull + _inc;
			return result;
		}
		void discard(unsigned long long count) {
			for (unsigned long long i=0;i<count;++i) {
				_state = _state*6364136223846793005ull + _inc;
			}
		}
};


inline float rand_1f(RNG& rng) {
	return std::uniform_real_distribution<float>()(rng);
}
inline Dir rand_coshemi(RNG& rng, float* pdf) {
	Dir result;
	do {
		float angle = rand_1f(rng) * (2.0f*PI<float>);
		float c = std::cos(angle);
		float s = std::sin(angle);

		float radius_sq = rand_1f(rng);
		float radius = std::sqrt(radius_sq);

		result = Dir(
			radius * c,
			std::sqrt( 1 - radius_sq ),
			radius * s
		);
		*pdf = result[1];
	} while (*pdf<=EPS);
	*pdf *= 1.0f / PI<float>;

	return result;
}


}
