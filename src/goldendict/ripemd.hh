#pragma once
#include <cstdint>
#include <cstddef>
class RIPEMD128 {
public:
  RIPEMD128();
  void update(const uint8_t* data, size_t len);
  void digest(uint8_t* digest);
private:
  uint64_t count;
  uint8_t buffer[64];
  uint32_t state[10];
  void transform(const uint8_t buffer[64]);
};
