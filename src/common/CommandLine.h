#pragma once

#include <string>

class CLIntercept {
public:  
  void go(int &argc, char **&argv);
  virtual ~CLIntercept();
private:
  /// @param value can be empty if the option doesn't have a value, don't save key and value
  virtual bool interceptOption(const std::string &key, const std::string &value) = 0;
};
