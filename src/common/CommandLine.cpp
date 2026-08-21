#include "CommandLine.h"
#include "Common.h"

#include <string>
#include <vector>

LLVM_CL_IGNORE_WARNINGS_BEGIN
static std::vector<char *> newArgv;
LLVM_CL_IGNORE_WARNINGS_END

CLIntercept::~CLIntercept() = default;

#if defined(__clang__) && __clang_major__ >= 16
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
void CLIntercept::go(int &argc, char **&argv) {
  // TODO: when adding new parsing rules, please be careful
  assert(argv && argc > 0);
  newArgv.clear();

  enum State {
    Normal,
    ExpectValue,
    PutToNewArgv
  };

  newArgv.reserve(static_cast<std::size_t>(argc) + 1);
  newArgv.push_back(argv[0]);

  State state = Normal;
  char *pendingToken;
  std::string pendingKey;

  for (int i = 1; i < argc; ++i) {
    char *token = argv[i];
    assert(token && token[0]);
    enum TokenType {
      ShortOption,
      LongOption,
      DoubleDash,
      Other
    };
    TokenType type;

    if (token[0] != '-' || token[1] == '\0') {
      type = Other;
    } else if (token[1] != '-') {
      type = ShortOption;
    } else if (token[2] == '\0') {
      type = DoubleDash;
    } else {
      type = LongOption;
    }

    switch (state) {
      case Normal:
      case ExpectValue: {
        if (type == LongOption || type == ShortOption) {
          if (state == ExpectValue) {
            if(!interceptOption(pendingKey, "")) newArgv.push_back(pendingToken);
          }
          std::string tokenStr = std::string(token);
          std::string skipped = tokenStr.substr(type == LongOption ? 2 : 1);
          size_t eq = skipped.find('=');
          if (eq != std::string::npos) {
            if (!interceptOption(skipped.substr(0, eq), skipped.substr(eq + 1))) 
              newArgv.push_back(token);
            state = Normal;
          } else {
            pendingKey = skipped;
            pendingToken = token;
            state = ExpectValue;
          }
        } else if (type == DoubleDash) {
          if (state == ExpectValue) {
            if(!interceptOption(pendingKey, "")) newArgv.push_back(pendingToken);
          }
          newArgv.push_back(token);
          state = PutToNewArgv;
        } else {
          if (state == ExpectValue) {
            if (!interceptOption(pendingKey, token)) { newArgv.push_back(pendingToken); newArgv.push_back(token); }
          }
          else newArgv.push_back(token);
          state = Normal;
        }
      }
      break;
      case PutToNewArgv:
        newArgv.push_back(token);
      break;
    }
  }

  if (state == ExpectValue) interceptOption(pendingKey, "");

  newArgv.push_back(nullptr);
  argc = static_cast<int>(newArgv.size()) - 1;
  argv = newArgv.data();
}
#if defined(__clang__) && __clang_major__ >= 16
#pragma clang diagnostic pop
#endif
