#pragma once

#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>

#include <cstdio>
#include <memory>
#include <string>
#include <iostream>

/** a CRTP class
 * @note void Impl::_init_impl(this, int, char **)
 * @note std::string Impl::_handle_query_impl(const std::string &)
 * @warning _init_impl should do llvm::cl::ParseCommandLineOptions
 */
template <typename Impl>
class QueryServer {
public:
  int run(int argc, char **argv) {
    Impl backend;
    backend.init(argc, argv);
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
  void init(int argc, char **argv) { static_cast<Impl *>(this)->_init_impl(argc, argv); }
  std::string handle_query(const std::string &req) {
    try {
    return static_cast<Impl *>(this)->_handle_query_impl(req);
    } catch (const std::exception &e) {
      return "error: " + std::string(e.what());
    }
  }
};
