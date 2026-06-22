#pragma once
#include <string>
#include <stdexcept>

class ExBase : public std::exception {
public:
    const char* what() const noexcept override { return "Dictionary error"; }
};

#define DEF_EX(exName, exDescription, exParent) \
  class exName : public exParent { \
  public: \
    const char* what() const noexcept override { return exDescription; } \
  };

#define DEF_EX_STR(exName, exDescription, exParent) \
  class exName : public exParent { \
  public: \
    explicit exName(const std::string& msg) : message_(exDescription " " + msg) {} \
    const char* what() const noexcept override { return message_.c_str(); } \
  private: \
    std::string message_; \
  };
