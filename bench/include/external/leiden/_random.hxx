#pragma once
#include <cstdint>





/**
 * A 32-bit xorshift RNG.
 */
class xorshift32_engine {

  private:
  /** State of the RNG. */
  uint32_t state;




  public:
  /**
   * Generate a random number.
   */
  uint32_t operator()() {
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return state = x;
  }




  /**
   * Construct an RNG with a random seed.
   * @param state initial state
   */
  xorshift32_engine(uint32_t state)
  : state(state) {}

};
// - https://stackoverflow.com/a/71523041/1413259
// - https://www.jstatsoft.org/article/download/v008i14/916

