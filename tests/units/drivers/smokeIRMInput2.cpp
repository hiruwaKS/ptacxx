#include <string>

// global constructor
std::string global_str = "hello";

int ptr(int* b) {
  int a;
  int** c;
  int *** d;
  b=&a;
  c=&b;
  d=&c;
  ***d = global_str.size();
  global_str = "world";
  return 0;
}

int main(){
  int b;
  return ptr(&b);
}