#pragma once

#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>

#include <cstdio>
#include <memory>
#include <string>
#include <iostream>

template <typename Impl> // CRTP
class QueryServer {
public:
  int run(int argc, char **argv) {
    llvm::cl::ParseCommandLineOptions(argc, argv);
    Impl backend;
    backend.init();
    while (true) {
      std::string input;
      std::getline(std::cin, input);
      std::string output = backend.handle_query(input);
      std::cout << "\n<queryresult>\n" << output << "\n</queryresult>\n";
      if (std::cin.eof()) break;
    }
    return 0;
  }
private:
  void init() { static_cast<Impl *>(this)->_init_impl(); }
  std::string handle_query(const std::string &req) {
    try {
    return static_cast<Impl *>(this)->_handle_query_impl(req);
    } catch (const std::exception &e) {
      return "error: " + std::string(e.what());
    }
  }
};
